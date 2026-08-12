// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/source/natural_earth.hpp"

#include "aeris/source/shapefile.hpp"
#include "aeris/util/sha256.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

namespace aeris::source {
namespace {

constexpr const char* kDatasetName = "ne_110m_land";
constexpr const char* kShapefileName = "ne_110m_land.shp";
constexpr const char* kVersionName = "ne_110m_land.VERSION.txt";
constexpr const char* kProjectionName = "ne_110m_land.prj";

[[nodiscard]] std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    );
}

[[nodiscard]] std::string trim_ascii(std::string value) {
    const auto not_space = [](const unsigned char c) {
        return std::isspace(c) == 0;
    };

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

[[nodiscard]] Result failure(const SourceError error, std::string diagnostic) {
    Result result{};
    result.error = error;
    result.diagnostic = std::move(diagnostic);
    return result;
}

}  // namespace

NaturalEarthLand110mAdapter::NaturalEarthLand110mAdapter(NaturalEarthLandConfig config)
    : config_(std::move(config)) {}

AdapterDescriptor NaturalEarthLand110mAdapter::descriptor() const noexcept {
    return {
        "natural-earth.ne-110m-land.shapefile.v1",
        "Natural Earth",
        capability_bit(Capability::land),
        TemporalClass::slow_change,
    };
}

Result NaturalEarthLand110mAdapter::load(const Request& request) const {
    if (request.capability != Capability::land) {
        return failure(SourceError::unsupported_capability, "Natural Earth 110m land adapter supplies land only");
    }
    if (!request.worldview.empty()) {
        return failure(SourceError::unsupported_worldview, "physical land geometry has no worldview selector");
    }
    if (config_.dataset_directory.empty() || config_.snapshot.empty() ||
        config_.source_uri.empty() || config_.retrieved_at_utc.empty()) {
        return failure(SourceError::invalid_request, "Natural Earth adapter configuration is incomplete");
    }
    if (!request.snapshot.empty() && request.snapshot != config_.snapshot) {
        return failure(SourceError::unavailable_snapshot, "requested snapshot differs from configured local snapshot");
    }

    const std::filesystem::path shp_path = config_.dataset_directory / kShapefileName;
    const std::filesystem::path version_path = config_.dataset_directory / kVersionName;
    const std::filesystem::path projection_path = config_.dataset_directory / kProjectionName;

    const std::string dataset_version = trim_ascii(read_text_file(version_path));
    if (dataset_version.empty()) {
        return failure(SourceError::provenance_incomplete, "Natural Earth dataset VERSION.txt is missing or empty");
    }

    const std::string projection_wkt = read_text_file(projection_path);
    if (!recognized_wgs84_prj(projection_wkt)) {
        return failure(SourceError::malformed_source, "Natural Earth .prj is missing or is not recognized as WGS84 geographic CRS");
    }

    const util::Sha256FileResult hash = util::sha256_file(shp_path);
    if (!hash.ok()) {
        return failure(SourceError::malformed_source, "unable to hash Natural Earth .shp bytes");
    }
    const std::string hash_hex = hash.digest.hex();
    if (!config_.expected_shp_sha256.empty() && hash_hex != config_.expected_shp_sha256) {
        return failure(SourceError::content_hash_mismatch, "Natural Earth .shp SHA-256 does not match configured snapshot hash");
    }

    const ShapefilePolygonResult parsed = read_polygon_shapefile(shp_path);
    if (!parsed.ok()) {
        return failure(SourceError::normalization_failed, "Natural Earth Polygon Shapefile failed strict decoding/canonicalization");
    }

    Result result{};
    result.provenance.provider = "Natural Earth";
    result.provenance.dataset = kDatasetName;
    result.provenance.snapshot = config_.snapshot;
    result.provenance.dataset_version = dataset_version;
    result.provenance.source_uri = config_.source_uri;
    result.provenance.license_id = "LicenseRef-Natural-Earth-Public-Domain";
    result.provenance.content_sha256 = hash_hex;
    result.provenance.retrieved_at_utc = config_.retrieved_at_utc;

    result.features.reserve(parsed.records.size());
    for (const ShapefileRecord& record : parsed.records) {
        Feature feature{};
        feature.source_id = "record:" + std::to_string(record.record_number);
        feature.stable_id =
            std::string(kDatasetName) + ":" + config_.snapshot + ":" + feature.source_id;
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
