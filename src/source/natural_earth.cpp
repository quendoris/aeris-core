// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/source/natural_earth.hpp"

#include "aeris/source/shapefile.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

namespace aeris::source {
namespace {

constexpr const char* kProviderName = "Natural Earth";
constexpr const char* kDatasetName = "ne_110m_land";
constexpr const char* kGeometryResource = "geometry.shp";
constexpr const char* kVersionResource = "dataset.version";
constexpr const char* kProjectionResource = "crs.prj";

[[nodiscard]] std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
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
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

[[nodiscard]] bool recognized_wgs84_prj(const std::string& wkt) {
    if (wkt.empty()) {
        return false;
    }
    const bool has_geographic = wkt.find("GEOGCS") != std::string::npos ||
                                wkt.find("GEOGCRS") != std::string::npos;
    const bool has_wgs84 = wkt.find("WGS_1984") != std::string::npos ||
                           wkt.find("WGS 84") != std::string::npos;
    return has_geographic && has_wgs84;
}

[[nodiscard]] std::string shapefile_failure_diagnostic(
    const ShapefilePolygonResult& parsed
) {
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
    if (!parsed.diagnostic.empty()) {
        diagnostic += ": " + parsed.diagnostic;
    }
    return diagnostic;
}

[[nodiscard]] Result failure(const SourceError error, std::string diagnostic) {
    Result result{};
    result.error = error;
    result.diagnostic = std::move(diagnostic);
    return result;
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
    const Request& request
) const {
    if (request.capability != Capability::land) {
        return failure(SourceError::unsupported_capability, "Natural Earth 110m land adapter supplies land only");
    }
    if (!request.worldview.empty()) {
        return failure(SourceError::unsupported_worldview, "physical land geometry has no worldview selector");
    }

    const SnapshotManifest& manifest = snapshot.manifest();
    if (manifest.provider != kProviderName || manifest.dataset != kDatasetName) {
        return failure(SourceError::malformed_source, "verified snapshot identity does not match Natural Earth 110m land");
    }
    if (!request.snapshot.empty() && request.snapshot != manifest.snapshot) {
        return failure(SourceError::unavailable_snapshot, "requested snapshot differs from verified snapshot");
    }

    const auto shp_path = snapshot.resource_path(kGeometryResource);
    const auto version_path = snapshot.resource_path(kVersionResource);
    const auto projection_path = snapshot.resource_path(kProjectionResource);
    if (!shp_path.has_value() || !version_path.has_value() || !projection_path.has_value()) {
        return failure(SourceError::provenance_incomplete, "verified Natural Earth snapshot lacks required logical resources");
    }

    const std::string dataset_version = trim_ascii(read_text_file(*version_path));
    if (dataset_version.empty()) {
        return failure(SourceError::provenance_incomplete, "Natural Earth dataset VERSION.txt is missing or empty");
    }
    const std::string projection_wkt = read_text_file(*projection_path);
    if (!recognized_wgs84_prj(projection_wkt)) {
        return failure(SourceError::malformed_source, "Natural Earth .prj is not recognized as WGS84 geographic CRS");
    }

    const ShapefilePolygonResult parsed = read_polygon_shapefile(*shp_path);
    if (!parsed.ok()) {
        return failure(
            SourceError::normalization_failed,
            shapefile_failure_diagnostic(parsed)
        );
    }

    Result result{};
    result.provenance.provider = manifest.provider;
    result.provenance.dataset = manifest.dataset;
    result.provenance.snapshot = manifest.snapshot;
    result.provenance.dataset_version = dataset_version;
    result.provenance.source_uri = manifest.source_uri;
    result.provenance.license_id = "LicenseRef-Natural-Earth-Public-Domain";
    result.provenance.content_sha256 = snapshot.content_sha256();
    result.provenance.retrieved_at_utc = manifest.retrieved_at_utc;

    result.features.reserve(parsed.records.size());
    for (const ShapefileRecord& record : parsed.records) {
        Feature feature{};
        feature.source_id = "record:" + std::to_string(record.record_number);
        feature.stable_id = std::string(kDatasetName) + ":" + manifest.snapshot + ":" + feature.source_id;
        feature.rings.reserve(record.rings.size());
        for (const ShapefileRing& source_ring : record.rings) {
            FeatureRing ring{};
            ring.geometry = source_ring.geometry;
            ring.role = source_ring.role;
            feature.rings.push_back(std::move(ring));
        }
        result.features.push_back(std::move(feature));
    }

    const SourceError validation = validate_result(*this, request, result);
    if (validation != SourceError::none) {
        return failure(validation, "Natural Earth adapter output failed common source-adapter validation");
    }
    return result;
}

}  // namespace aeris::source
