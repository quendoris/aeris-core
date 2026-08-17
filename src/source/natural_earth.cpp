// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/source/natural_earth.hpp"

#include "aeris/source/dbf.hpp"
#include "aeris/source/shapefile.hpp"
#include "aeris/util/text.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace aeris::source {
namespace {

constexpr const char* kProviderName = "Natural Earth";
constexpr const char* kLicenseId = "LicenseRef-Natural-Earth-Public-Domain";

constexpr const char* kLandDatasetName = "ne_110m_land";
constexpr const char* kLandGeometryResource = "geometry.shp";
constexpr const char* kLandVersionResource = "dataset.version";
constexpr const char* kLandProjectionResource = "crs.prj";

constexpr const char* kAdmin0DatasetName = "ne_110m_admin_0_countries";
constexpr const char* kAdmin0GeometryResource = "geometry.shp";
constexpr const char* kAdmin0AttributesResource = "attributes.dbf";
constexpr const char* kAdmin0EncodingResource = "attributes.cpg";
constexpr const char* kAdmin0VersionResource = "dataset.version";
constexpr const char* kAdmin0ProjectionResource = "crs.prj";
constexpr const char* kAdmin0Worldview = "natural-earth.de-facto";

[[nodiscard]] std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

[[nodiscard]] std::string trim_ascii(std::string value) {
    const auto not_space = [](const unsigned char c) { return std::isspace(c) == 0; };
    const auto first = std::find_if(value.begin(), value.end(), [&](const char c) {
        return not_space(static_cast<unsigned char>(c));
    });
    const auto last = std::find_if(value.rbegin(), value.rend(), [&](const char c) {
        return not_space(static_cast<unsigned char>(c));
    }).base();
    if (first >= last) return {};
    return std::string(first, last);
}

[[nodiscard]] std::string trim_dbf_numeric(const std::string& value) {
    const std::size_t first = value.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    const std::size_t last = value.find_last_not_of(' ');
    return value.substr(first, last - first + 1U);
}

// Natural Earth's pinned DBF declares UTF-8 in its CPG, but character fields
// use both ASCII space and trailing NUL bytes as fixed-width storage padding.
// NUL is accepted only in the removable suffix; an embedded NUL remains a
// malformed semantic string and never enters AERIS feature properties.
[[nodiscard]] bool decode_dbf_character(const std::string& raw, std::string& value) {
    std::size_t end = raw.size();
    while (end > 0U && (raw[end - 1U] == ' ' || raw[end - 1U] == '\0')) --end;

    std::size_t begin = 0U;
    while (begin < end && raw[begin] == ' ') ++begin;
    for (std::size_t index = begin; index < end; ++index) {
        if (raw[index] == '\0') return false;
    }

    value.assign(raw.data() + begin, end - begin);
    return util::is_valid_utf8_nul_free(value);
}

[[nodiscard]] bool recognized_wgs84_prj(const std::string& wkt) {
    if (wkt.empty()) return false;
    const bool geographic = wkt.find("GEOGCS") != std::string::npos ||
                            wkt.find("GEOGCRS") != std::string::npos;
    const bool wgs84 = wkt.find("WGS_1984") != std::string::npos ||
                       wkt.find("WGS 84") != std::string::npos;
    return geographic && wgs84;
}

[[nodiscard]] Result failure(const SourceError error, std::string diagnostic) {
    Result result{};
    result.error = error;
    result.diagnostic = std::move(diagnostic);
    return result;
}

[[nodiscard]] std::string shapefile_failure_diagnostic(const ShapefilePolygonResult& parsed) {
    std::string diagnostic = "Natural Earth Polygon Shapefile failed strict decoding/canonicalization";
    if (parsed.failed_record_number != 0U) {
        diagnostic += " at record " + std::to_string(parsed.failed_record_number);
    }
    diagnostic += " [shapefile_error=" + std::to_string(static_cast<unsigned>(parsed.error));
    if (parsed.geographic_error != geometry::GeographicError::none) {
        diagnostic += ", geographic_error=" +
            std::to_string(static_cast<unsigned>(parsed.geographic_error));
    }
    diagnostic += ']';
    if (!parsed.diagnostic.empty()) diagnostic += ": " + parsed.diagnostic;
    return diagnostic;
}

[[nodiscard]] std::string dbf_failure_diagnostic(const DbfTableResult& parsed) {
    std::string diagnostic = "Natural Earth DBF failed strict structural decoding";
    if (parsed.failed_record_number != 0U) {
        diagnostic += " at record " + std::to_string(parsed.failed_record_number);
    }
    diagnostic += " [dbf_error=" + std::to_string(static_cast<unsigned>(parsed.error)) + ']';
    if (!parsed.diagnostic.empty()) diagnostic += ": " + parsed.diagnostic;
    return diagnostic;
}

void copy_geometry(const ShapefileRecord& record, Feature& feature) {
    feature.rings.reserve(record.rings.size());
    for (const ShapefileRing& source_ring : record.rings) {
        FeatureRing ring{};
        ring.geometry = source_ring.geometry;
        ring.role = source_ring.role;
        ring.geometry.interior_side =
            ring.role == RingRole::exterior
                ? geometry::RingInteriorSide::right
                : geometry::RingInteriorSide::left;
        feature.rings.push_back(std::move(ring));
    }
}

[[nodiscard]] std::optional<std::size_t> field_index(
    const DbfTableResult& table,
    const std::string_view name) {
    for (std::size_t index = 0U; index < table.fields.size(); ++index) {
        const std::string& candidate = table.fields[index].name;
        if (candidate.size() == name.size() &&
            std::equal(candidate.begin(), candidate.end(), name.begin())) {
            return index;
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool require_character_field(
    const DbfTableResult& table,
    const std::string_view name,
    std::size_t& index) {
    const auto found = field_index(table, name);
    if (!found.has_value() || table.fields[*found].type != 'C') return false;
    index = *found;
    return true;
}

[[nodiscard]] bool require_integer_text_field(
    const DbfTableResult& table,
    const std::string_view name,
    std::size_t& index) {
    const auto found = field_index(table, name);
    if (!found.has_value()) return false;
    const DbfField& field = table.fields[*found];
    if (field.type != 'N' || field.decimal_count != 0U) return false;
    index = *found;
    return true;
}

[[nodiscard]] bool decode_text_property(
    const DbfRecord& record,
    const std::size_t index,
    std::string& value) {
    return index < record.values.size() && decode_dbf_character(record.values[index], value);
}

[[nodiscard]] bool decode_positive_int64(
    const DbfRecord& record,
    const std::size_t index,
    std::int64_t& value) {
    if (index >= record.values.size()) return false;
    const std::string text = trim_dbf_numeric(record.values[index]);
    if (text.empty()) return false;
    std::int64_t parsed = 0;
    const auto converted = std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
    if (converted.ec != std::errc{} || converted.ptr != text.data() + text.size() || parsed <= 0) {
        return false;
    }
    value = parsed;
    return true;
}

void add_text_property(Feature& feature, std::string key, std::string value) {
    FeatureProperty property{};
    property.key = std::move(key);
    property.value = std::move(value);
    feature.properties.push_back(std::move(property));
}

void populate_provenance(
    Result& result,
    const SnapshotManifest& manifest,
    const VerifiedSnapshot& snapshot,
    std::string dataset_version,
    std::string worldview = {}) {
    result.provenance.provider = manifest.provider;
    result.provenance.dataset = manifest.dataset;
    result.provenance.snapshot = manifest.snapshot;
    result.provenance.dataset_version = std::move(dataset_version);
    result.provenance.source_uri = manifest.source_uri;
    result.provenance.license_id = kLicenseId;
    result.provenance.content_sha256 = snapshot.content_sha256();
    result.provenance.retrieved_at_utc = manifest.retrieved_at_utc;
    result.provenance.worldview = std::move(worldview);
}

}  // namespace

AdapterDescriptor NaturalEarthLand110mAdapter::descriptor() const noexcept {
    return {
        "natural-earth.ne-110m-land.shapefile.v1",
        kProviderName,
        capability_bit(Capability::land),
        TemporalClass::slow_change,
    };
}

Result NaturalEarthLand110mAdapter::load(
    const VerifiedSnapshot& snapshot,
    const Request& request) const {
    if (request.capability != Capability::land) {
        return failure(SourceError::unsupported_capability,
                       "Natural Earth 110m land adapter supplies land only");
    }
    if (!request.worldview.empty()) {
        return failure(SourceError::unsupported_worldview,
                       "physical land geometry has no worldview selector");
    }

    const SnapshotManifest& manifest = snapshot.manifest();
    if (manifest.provider != kProviderName || manifest.dataset != kLandDatasetName) {
        return failure(SourceError::malformed_source,
                       "verified snapshot identity does not match Natural Earth 110m land");
    }
    if (!request.snapshot.empty() && request.snapshot != manifest.snapshot) {
        return failure(SourceError::unavailable_snapshot,
                       "requested snapshot differs from verified snapshot");
    }

    const auto shp_path = snapshot.resource_path(kLandGeometryResource);
    const auto version_path = snapshot.resource_path(kLandVersionResource);
    const auto projection_path = snapshot.resource_path(kLandProjectionResource);
    if (!shp_path.has_value() || !version_path.has_value() || !projection_path.has_value()) {
        return failure(SourceError::provenance_incomplete,
                       "verified Natural Earth snapshot lacks required logical resources");
    }

    const std::string dataset_version = trim_ascii(read_text_file(*version_path));
    if (dataset_version.empty()) {
        return failure(SourceError::provenance_incomplete,
                       "Natural Earth dataset VERSION.txt is missing or empty");
    }
    if (!recognized_wgs84_prj(read_text_file(*projection_path))) {
        return failure(SourceError::malformed_source,
                       "Natural Earth .prj is not recognized as WGS84 geographic CRS");
    }

    const ShapefilePolygonResult parsed = read_polygon_shapefile(*shp_path);
    if (!parsed.ok()) {
        return failure(SourceError::normalization_failed,
                       shapefile_failure_diagnostic(parsed));
    }

    Result result{};
    populate_provenance(result, manifest, snapshot, dataset_version);
    result.features.reserve(parsed.records.size());
    for (const ShapefileRecord& record : parsed.records) {
        Feature feature{};
        feature.source_id = "record:" + std::to_string(record.record_number);
        feature.stable_id = std::string(kLandDatasetName) + ":" +
                            manifest.snapshot + ":" + feature.source_id;
        copy_geometry(record, feature);
        result.features.push_back(std::move(feature));
    }

    const SourceError validation = validate_result(*this, request, result);
    if (validation != SourceError::none) {
        return failure(validation,
                       "Natural Earth adapter output failed common source-adapter validation");
    }
    return result;
}

AdapterDescriptor NaturalEarthAdmin0Countries110mAdapter::descriptor() const noexcept {
    return {
        "natural-earth.ne-110m-admin0-countries.shapefile-dbf.v1",
        kProviderName,
        capability_bit(Capability::admin0),
        TemporalClass::slow_change,
    };
}

Result NaturalEarthAdmin0Countries110mAdapter::load(
    const VerifiedSnapshot& snapshot,
    const Request& request) const {
    if (request.capability != Capability::admin0) {
        return failure(SourceError::unsupported_capability,
                       "Natural Earth 110m admin0 adapter supplies admin0 countries only");
    }
    if (!request.worldview.empty() && request.worldview != kAdmin0Worldview) {
        return failure(SourceError::unsupported_worldview,
                       "Natural Earth admin0 countries expose only the pinned de-facto worldview");
    }

    const SnapshotManifest& manifest = snapshot.manifest();
    if (manifest.provider != kProviderName || manifest.dataset != kAdmin0DatasetName) {
        return failure(SourceError::malformed_source,
                       "verified snapshot identity does not match Natural Earth 110m admin0 countries");
    }
    if (!request.snapshot.empty() && request.snapshot != manifest.snapshot) {
        return failure(SourceError::unavailable_snapshot,
                       "requested snapshot differs from verified admin0 snapshot");
    }

    const auto shp_path = snapshot.resource_path(kAdmin0GeometryResource);
    const auto dbf_path = snapshot.resource_path(kAdmin0AttributesResource);
    const auto cpg_path = snapshot.resource_path(kAdmin0EncodingResource);
    const auto version_path = snapshot.resource_path(kAdmin0VersionResource);
    const auto projection_path = snapshot.resource_path(kAdmin0ProjectionResource);
    if (!shp_path.has_value() || !dbf_path.has_value() || !cpg_path.has_value() ||
        !version_path.has_value() || !projection_path.has_value()) {
        return failure(SourceError::provenance_incomplete,
                       "verified Natural Earth admin0 snapshot lacks a required SHP/DBF/CPG/PRJ/VERSION resource");
    }

    const std::string dataset_version = trim_ascii(read_text_file(*version_path));
    if (dataset_version.empty()) {
        return failure(SourceError::provenance_incomplete,
                       "Natural Earth admin0 VERSION.txt is missing or empty");
    }
    if (trim_ascii(read_text_file(*cpg_path)) != "UTF-8") {
        return failure(SourceError::malformed_source,
                       "Natural Earth admin0 DBF encoding marker is not exactly UTF-8");
    }
    if (!recognized_wgs84_prj(read_text_file(*projection_path))) {
        return failure(SourceError::malformed_source,
                       "Natural Earth admin0 .prj is not recognized as WGS84 geographic CRS");
    }

    const ShapefilePolygonResult geometry_table = read_polygon_shapefile(*shp_path);
    if (!geometry_table.ok()) {
        return failure(SourceError::normalization_failed,
                       shapefile_failure_diagnostic(geometry_table));
    }
    const DbfTableResult attributes_table = read_dbf_table(*dbf_path);
    if (!attributes_table.ok()) {
        return failure(SourceError::normalization_failed,
                       dbf_failure_diagnostic(attributes_table));
    }
    if (geometry_table.records.size() != attributes_table.records.size()) {
        return failure(SourceError::malformed_source,
                       "Natural Earth admin0 SHP and DBF physical record counts differ");
    }

    std::size_t name = 0U;
    std::size_t name_long = 0U;
    std::size_t admin = 0U;
    std::size_t sovereignt = 0U;
    std::size_t type = 0U;
    std::size_t adm0_a3 = 0U;
    std::size_t sov_a3 = 0U;
    std::size_t iso_a2 = 0U;
    std::size_t iso_a3 = 0U;
    std::size_t un_a3 = 0U;
    std::size_t continent = 0U;
    std::size_t region_un = 0U;
    std::size_t subregion = 0U;
    std::size_t ne_id = 0U;

    const bool required_fields =
        require_character_field(attributes_table, "NAME", name) &&
        require_character_field(attributes_table, "NAME_LONG", name_long) &&
        require_character_field(attributes_table, "ADMIN", admin) &&
        require_character_field(attributes_table, "SOVEREIGNT", sovereignt) &&
        require_character_field(attributes_table, "TYPE", type) &&
        require_character_field(attributes_table, "ADM0_A3", adm0_a3) &&
        require_character_field(attributes_table, "SOV_A3", sov_a3) &&
        require_character_field(attributes_table, "ISO_A2", iso_a2) &&
        require_character_field(attributes_table, "ISO_A3", iso_a3) &&
        require_character_field(attributes_table, "UN_A3", un_a3) &&
        require_character_field(attributes_table, "CONTINENT", continent) &&
        require_character_field(attributes_table, "REGION_UN", region_un) &&
        require_character_field(attributes_table, "SUBREGION", subregion) &&
        require_integer_text_field(attributes_table, "NE_ID", ne_id);
    if (!required_fields) {
        return failure(SourceError::malformed_source,
                       "Natural Earth admin0 DBF lacks the pinned field/type contract");
    }

    Result result{};
    populate_provenance(result, manifest, snapshot, dataset_version, kAdmin0Worldview);
    result.features.reserve(geometry_table.records.size());

    std::set<std::int64_t> seen_ne_ids;
    std::set<std::string> seen_adm0_codes;
    for (std::size_t row_index = 0U; row_index < geometry_table.records.size(); ++row_index) {
        const ShapefileRecord& geometry_record = geometry_table.records[row_index];
        const DbfRecord& attribute_record = attributes_table.records[row_index];
        if (attribute_record.deleted ||
            geometry_record.record_number != attribute_record.record_number) {
            return failure(SourceError::malformed_source,
                           "Natural Earth admin0 SHP/DBF physical record alignment is not exact and active");
        }

        std::string name_value;
        std::string name_long_value;
        std::string admin_value;
        std::string sovereignt_value;
        std::string type_value;
        std::string adm0_a3_value;
        std::string sov_a3_value;
        std::string iso_a2_value;
        std::string iso_a3_value;
        std::string un_a3_value;
        std::string continent_value;
        std::string region_un_value;
        std::string subregion_value;
        std::int64_t ne_id_value = 0;

        if (!decode_text_property(attribute_record, name, name_value) ||
            !decode_text_property(attribute_record, name_long, name_long_value) ||
            !decode_text_property(attribute_record, admin, admin_value) ||
            !decode_text_property(attribute_record, sovereignt, sovereignt_value) ||
            !decode_text_property(attribute_record, type, type_value) ||
            !decode_text_property(attribute_record, adm0_a3, adm0_a3_value) ||
            !decode_text_property(attribute_record, sov_a3, sov_a3_value) ||
            !decode_text_property(attribute_record, iso_a2, iso_a2_value) ||
            !decode_text_property(attribute_record, iso_a3, iso_a3_value) ||
            !decode_text_property(attribute_record, un_a3, un_a3_value) ||
            !decode_text_property(attribute_record, continent, continent_value) ||
            !decode_text_property(attribute_record, region_un, region_un_value) ||
            !decode_text_property(attribute_record, subregion, subregion_value) ||
            !decode_positive_int64(attribute_record, ne_id, ne_id_value)) {
            return failure(SourceError::malformed_source,
                           "Natural Earth admin0 row contains malformed text padding/UTF-8 or NE_ID data at physical row " +
                               std::to_string(attribute_record.record_number));
        }
        if (adm0_a3_value.empty() || !seen_adm0_codes.insert(adm0_a3_value).second ||
            !seen_ne_ids.insert(ne_id_value).second) {
            return failure(SourceError::malformed_source,
                           "Natural Earth admin0 identity fields are empty or non-unique at physical row " +
                               std::to_string(attribute_record.record_number));
        }

        Feature feature{};
        feature.source_id = "record:" + std::to_string(geometry_record.record_number);
        feature.stable_id = std::string(kAdmin0DatasetName) + ":ne_id:" +
                            std::to_string(ne_id_value);
        copy_geometry(geometry_record, feature);
        feature.properties.reserve(14U);
        add_text_property(feature, "name", std::move(name_value));
        add_text_property(feature, "name_long", std::move(name_long_value));
        add_text_property(feature, "admin", std::move(admin_value));
        add_text_property(feature, "sovereignt", std::move(sovereignt_value));
        add_text_property(feature, "type", std::move(type_value));
        add_text_property(feature, "adm0_a3", std::move(adm0_a3_value));
        add_text_property(feature, "sov_a3", std::move(sov_a3_value));
        add_text_property(feature, "iso_a2", std::move(iso_a2_value));
        add_text_property(feature, "iso_a3", std::move(iso_a3_value));
        add_text_property(feature, "un_a3", std::move(un_a3_value));
        add_text_property(feature, "continent", std::move(continent_value));
        add_text_property(feature, "region_un", std::move(region_un_value));
        add_text_property(feature, "subregion", std::move(subregion_value));
        FeatureProperty identity{};
        identity.key = "ne_id";
        identity.value = ne_id_value;
        feature.properties.push_back(std::move(identity));
        result.features.push_back(std::move(feature));
    }
    result.feature_properties_complete = true;

    const SourceError validation = validate_result(*this, request, result);
    if (validation != SourceError::none) {
        return failure(validation,
                       "Natural Earth admin0 adapter output failed common source-adapter validation");
    }
    return result;
}

}  // namespace aeris::source
