// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/source/natural_earth.hpp"

#include "aeris/source/shapefile.hpp"
#include "aeris/surface/classification.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

namespace aeris::source {
namespace {

constexpr const char* kProviderName = "Natural Earth";
constexpr const char* kLicenseId = "LicenseRef-Natural-Earth-Public-Domain";
constexpr const char* kIceShelfDatasetName = "ne_50m_antarctic_ice_shelves_polys";
constexpr const char* kGeometryResource = "geometry.shp";
constexpr const char* kVersionResource = "dataset.version";
constexpr const char* kProjectionResource = "crs.prj";

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

[[nodiscard]] std::string shapefile_failure_diagnostic(
    const ShapefilePolygonResult& parsed
) {
    std::string diagnostic =
        "Natural Earth Antarctic ice-shelf Polygon Shapefile failed strict decoding/canonicalization";
    if (parsed.failed_record_number != 0U) {
        diagnostic += " at record " + std::to_string(parsed.failed_record_number);
    }
    diagnostic += " [shapefile_error=" +
        std::to_string(static_cast<unsigned>(parsed.error));
    if (parsed.geographic_error != geometry::GeographicError::none) {
        diagnostic += ", geographic_error=" +
            std::to_string(static_cast<unsigned>(parsed.geographic_error));
    }
    diagnostic += ']';
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

void add_surface_class_property(Feature& feature) {
    FeatureProperty property{};
    property.key = std::string(surface::kSurfaceClassPropertyKey);
    property.value = std::string(
        surface::surface_class_id(surface::SurfaceClass::floating_ice_shelf)
    );
    feature.properties.push_back(std::move(property));
}

}  // namespace

AdapterDescriptor NaturalEarthAntarcticIceShelves50mAdapter::descriptor() const noexcept {
    return {
        "natural-earth.ne-50m-antarctic-ice-shelves.surface-classification.v1",
        kProviderName,
        capability_bit(Capability::surface_classification),
        TemporalClass::slow_change,
    };
}

Result NaturalEarthAntarcticIceShelves50mAdapter::load(
    const VerifiedSnapshot& snapshot,
    const Request& request
) const {
    if (request.capability != Capability::surface_classification) {
        return failure(
            SourceError::unsupported_capability,
            "Natural Earth Antarctic ice-shelf adapter supplies surface classification only"
        );
    }
    if (!request.worldview.empty()) {
        return failure(
            SourceError::unsupported_worldview,
            "Antarctic ice-shelf physical geometry has no worldview selector"
        );
    }

    const SnapshotManifest& manifest = snapshot.manifest();
    if (manifest.provider != kProviderName || manifest.dataset != kIceShelfDatasetName) {
        return failure(
            SourceError::malformed_source,
            "verified snapshot identity does not match Natural Earth 50m Antarctic ice shelves"
        );
    }
    if (!request.snapshot.empty() && request.snapshot != manifest.snapshot) {
        return failure(
            SourceError::unavailable_snapshot,
            "requested snapshot differs from verified Antarctic ice-shelf snapshot"
        );
    }

    const auto shp_path = snapshot.resource_path(kGeometryResource);
    const auto version_path = snapshot.resource_path(kVersionResource);
    const auto projection_path = snapshot.resource_path(kProjectionResource);
    if (!shp_path.has_value() || !version_path.has_value() || !projection_path.has_value()) {
        return failure(
            SourceError::provenance_incomplete,
            "verified Antarctic ice-shelf snapshot lacks required SHP/PRJ/VERSION resources"
        );
    }

    const std::string dataset_version = trim_ascii(read_text_file(*version_path));
    if (dataset_version.empty()) {
        return failure(
            SourceError::provenance_incomplete,
            "Natural Earth Antarctic ice-shelf VERSION.txt is missing or empty"
        );
    }
    if (!recognized_wgs84_prj(read_text_file(*projection_path))) {
        return failure(
            SourceError::malformed_source,
            "Natural Earth Antarctic ice-shelf .prj is not recognized as WGS84 geographic CRS"
        );
    }

    const ShapefilePolygonResult parsed = read_polygon_shapefile(*shp_path);
    if (!parsed.ok()) {
        return failure(
            SourceError::normalization_failed,
            shapefile_failure_diagnostic(parsed)
        );
    }
    if (parsed.records.empty()) {
        return failure(
            SourceError::malformed_source,
            "Natural Earth Antarctic ice-shelf source contains no polygon records"
        );
    }

    Result result{};
    result.provenance.provider = manifest.provider;
    result.provenance.dataset = manifest.dataset;
    result.provenance.snapshot = manifest.snapshot;
    result.provenance.dataset_version = dataset_version;
    result.provenance.source_uri = manifest.source_uri;
    result.provenance.license_id = kLicenseId;
    result.provenance.content_sha256 = snapshot.content_sha256();
    result.provenance.retrieved_at_utc = manifest.retrieved_at_utc;
    result.feature_properties_complete = true;
    result.features.reserve(parsed.records.size());

    for (const ShapefileRecord& record : parsed.records) {
        Feature feature{};
        feature.source_id = "record:" + std::to_string(record.record_number);
        feature.stable_id = std::string(kIceShelfDatasetName) + ":" +
                            manifest.snapshot + ":" + feature.source_id;
        copy_geometry(record, feature);
        add_surface_class_property(feature);
        result.features.push_back(std::move(feature));
    }

    const SourceError validation = validate_result(*this, request, result);
    if (validation != SourceError::none) {
        return failure(
            validation,
            "Natural Earth Antarctic ice-shelf adapter output failed common source-adapter validation"
        );
    }
    return result;
}

}  // namespace aeris::source
