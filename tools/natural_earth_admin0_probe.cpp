// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/projection/sinu_mollweide_ring.hpp"
#include "aeris/source/acquisition.hpp"
#include "aeris/source/natural_earth.hpp"
#include "aeris/util/sha256.hpp"
#include "aeris/view/scene.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr std::string_view kProvider = "Natural Earth";
constexpr std::string_view kDataset = "ne_110m_admin_0_countries";
constexpr std::string_view kSnapshot = "v5.1.2";
constexpr std::string_view kSourceUri =
    "https://github.com/nvkelso/natural-earth-vector/tree/f1890d9f152c896d250a77557a5751a93d494776/110m_cultural";

int fail(const int code, const std::string& diagnostic) {
    std::cerr << "Natural Earth admin0 proof failed: " << diagnostic << '\n';
    return code;
}

bool add_resource(
    aeris::source::SnapshotManifest& manifest,
    const std::filesystem::path& root,
    std::string logical_name,
    std::filesystem::path relative_path) {
    const std::filesystem::path full = root / relative_path;
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(full, error);
    if (error) return false;
    const aeris::util::Sha256FileResult hash = aeris::util::sha256_file(full);
    if (!hash.ok()) return false;

    aeris::source::ResourceSpec resource{};
    resource.logical_name = std::move(logical_name);
    resource.relative_path = std::move(relative_path);
    resource.sha256 = hash.digest.hex();
    resource.size_bytes = size;
    manifest.resources.push_back(std::move(resource));
    return true;
}

const aeris::source::FeatureProperty* property(
    const aeris::source::Feature& feature,
    const std::string_view key) {
    for (const aeris::source::FeatureProperty& candidate : feature.properties) {
        if (candidate.key == key) return &candidate;
    }
    return nullptr;
}

[[nodiscard]] aeris::projection::RingProjectionOptions scene_projection_options() {
    aeris::projection::RingProjectionOptions options{};
    options.primitive = aeris::projection::EqualAreaPrimitive::sinu_mollweide;
    options.central_meridian_rad = 0.0;
    options.relative_area_tolerance = 1e-7;
    options.absolute_area_tolerance_m2 = 10'000.0;
    options.initial_geometric_tolerance_m = 2'000.0;
    options.initial_local_area_tolerance_m2 = 1.0e8;
    options.max_refinement_rounds = 18U;
    options.subdivision_max_depth = 32U;
    options.subdivision_max_segments_per_edge = 1'000'000U;
    options.max_projection_pieces = 4096U;
    return options;
}

}  // namespace

int main(const int argc, char** argv) {
    if (argc != 3) {
        return fail(2, "usage: <snapshot-root> <retrieved-at-utc>");
    }

    const std::filesystem::path root = argv[1];
    const std::string retrieved_at_utc = argv[2];

    aeris::source::SnapshotManifest manifest{};
    manifest.provider = std::string(kProvider);
    manifest.dataset = std::string(kDataset);
    manifest.snapshot = std::string(kSnapshot);
    manifest.source_uri = std::string(kSourceUri);
    manifest.retrieved_at_utc = retrieved_at_utc;

    if (!add_resource(manifest, root, "geometry.shp", "ne_110m_admin_0_countries.shp") ||
        !add_resource(manifest, root, "attributes.dbf", "ne_110m_admin_0_countries.dbf") ||
        !add_resource(manifest, root, "attributes.cpg", "ne_110m_admin_0_countries.cpg") ||
        !add_resource(manifest, root, "crs.prj", "ne_110m_admin_0_countries.prj") ||
        !add_resource(manifest, root, "dataset.version", "ne_110m_admin_0_countries.VERSION.txt")) {
        return fail(3, "could not hash all pinned admin0 resources");
    }

    aeris::source::SnapshotVerificationResult verified =
        aeris::source::verify_local_snapshot(root, manifest);
    if (!verified.ok() || !verified.snapshot.has_value()) {
        return fail(4, "verified snapshot construction failed: " + verified.diagnostic);
    }

    const aeris::source::NaturalEarthAdmin0Countries110mAdapter adapter{};
    aeris::source::Request request{};
    request.capability = aeris::source::Capability::admin0;
    request.snapshot = std::string(kSnapshot);
    request.worldview = "natural-earth.de-facto";
    const aeris::source::Result result = adapter.load(*verified.snapshot, request);
    if (!result.ok()) {
        return fail(5, "adapter load failed: " + result.diagnostic);
    }
    if (!result.feature_properties_complete || result.features.empty()) {
        return fail(6, "adapter did not produce a nonempty complete feature-property channel");
    }
    if (result.provenance.worldview != request.worldview) {
        return fail(7, "adapter provenance worldview differs from requested pinned worldview");
    }

    std::set<std::string> stable_ids;
    std::set<std::int64_t> ne_ids;
    std::string antarctica_stable_id;
    std::size_t rings = 0U;
    std::size_t vertices = 0U;
    const aeris::projection::RingProjectionOptions projection_options =
        scene_projection_options();

    for (const aeris::source::Feature& feature : result.features) {
        if (!stable_ids.insert(feature.stable_id).second || feature.properties.size() != 14U) {
            return fail(8, "feature identity or complete property cardinality is malformed");
        }

        const auto* name = property(feature, "name");
        const auto* iso_a2 = property(feature, "iso_a2");
        const auto* adm0_a3 = property(feature, "adm0_a3");
        const auto* ne_id = property(feature, "ne_id");
        if (name == nullptr || iso_a2 == nullptr || adm0_a3 == nullptr || ne_id == nullptr ||
            !std::holds_alternative<std::string>(name->value) ||
            !std::holds_alternative<std::string>(iso_a2->value) ||
            !std::holds_alternative<std::string>(adm0_a3->value) ||
            !std::holds_alternative<std::int64_t>(ne_id->value)) {
            return fail(9, "required typed political properties are absent or have wrong types");
        }
        const std::string country_name = std::get<std::string>(name->value);
        if (country_name.empty() || std::get<std::string>(adm0_a3->value).empty()) {
            return fail(10, "required country name/admin code is empty");
        }
        if (!ne_ids.insert(std::get<std::int64_t>(ne_id->value)).second) {
            return fail(11, "NE_ID is not unique across the adapter result");
        }
        if (country_name == "Antarctica") {
            antarctica_stable_id = feature.stable_id;
        }

        rings += feature.rings.size();
        for (std::size_t ring_index = 0U;
             ring_index < feature.rings.size();
             ++ring_index) {
            const aeris::source::FeatureRing& ring = feature.rings[ring_index];
            vertices += ring.geometry.vertices.size();

            const auto projected =
                aeris::projection::project_philbrick_wgs84_linear_ring_piecewise_verified(
                    ring.geometry,
                    projection_options
                );
            if (!projected.ok()) {
                std::cerr
                    << "Sinu-Mollweide real-ring preflight failed"
                    << " country=" << country_name
                    << " stable_id=" << feature.stable_id
                    << " ring=" << ring_index
                    << " source_vertices=" << ring.geometry.vertices.size()
                    << " winding=" << ring.geometry.longitude_winding
                    << " interior_side=" << static_cast<unsigned>(ring.geometry.interior_side)
                    << " error=" << static_cast<unsigned>(projected.error)
                    << " projection_error=" << static_cast<unsigned>(projected.projection_error)
                    << " piece_error=" << static_cast<unsigned>(projected.piece_error)
                    << " seam_error=" << static_cast<unsigned>(projected.seam_error)
                    << " geographic_error=" << static_cast<unsigned>(projected.geographic_error)
                    << " subdivision_error=" << static_cast<unsigned>(projected.subdivision_error)
                    << " sample_error=" << static_cast<unsigned>(projected.sample_error)
                    << " failed_edge=" << projected.failed_edge
                    << " source_area_m2=" << projected.source_signed_area_m2
                    << " frame_area_m2=" << projected.frame_signed_area_m2
                    << " frame_error_m2=" << projected.frame_absolute_area_error_m2
                    << " allowed_m2=" << projected.allowed_area_error_m2
                    << " frame_round=" << projected.frame_refinement_rounds
                    << '\n';
                return fail(17, "real-ring Sinu-Mollweide preflight rejected a pinned admin0 ring");
            }
        }
    }
    if (antarctica_stable_id.empty()) {
        return fail(12, "pinned admin0 snapshot did not expose Antarctica by stable identity");
    }

    aeris::view::SceneRequest scene_request{};
    scene_request.mode = aeris::view::SurfaceMode::sinu_mollweide;
    scene_request.quality = aeris::view::SceneQuality::verified;
    scene_request.projection_central_meridian_deg = 0.0;
    const aeris::view::SceneGeometry scene =
        aeris::view::build_scene_geometry(result, scene_request);
    if (!scene.ok || scene.canceled) {
        return fail(13, "verified Sinu-Mollweide world scene failed: " + scene.diagnostic);
    }
    if (scene.mode != aeris::view::SurfaceMode::sinu_mollweide ||
        scene.features.size() != result.features.size() ||
        scene.fill_rings == 0U || scene.outline_parts == 0U || scene.vertices == 0U) {
        return fail(14, "Sinu-Mollweide scene cardinality or aggregate geometry is malformed");
    }

    std::size_t antarctica_pieces = 0U;
    for (const aeris::view::SceneFeatureGeometry& feature : scene.features) {
        if (stable_ids.find(feature.stable_id) == stable_ids.end() ||
            feature.fill_rings.empty() || feature.outlines.empty()) {
            return fail(15, "Sinu-Mollweide scene lost source identity or feature geometry");
        }
        if (feature.stable_id == antarctica_stable_id) {
            antarctica_pieces = feature.fill_rings.size();
        }
    }
    if (antarctica_pieces == 0U) {
        return fail(16, "Antarctica disappeared from the verified Sinu-Mollweide scene");
    }

    std::cout
        << "natural_earth_admin0: PASS"
        << " features=" << result.features.size()
        << " rings=" << rings
        << " vertices=" << vertices
        << " sinu_mollweide_fill_rings=" << scene.fill_rings
        << " sinu_mollweide_vertices=" << scene.vertices
        << " antarctica_pieces=" << antarctica_pieces
        << " content_sha256=" << verified.snapshot->content_sha256()
        << '\n';
    return EXIT_SUCCESS;
}
