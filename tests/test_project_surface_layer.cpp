// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/project/world_layers.hpp"

#include "aeris/source/adapter.hpp"
#include "aeris/storage/feature_property.hpp"
#include "aeris/storage/geometry.hpp"
#include "aeris/storage/provenance.hpp"
#include "aeris/surface/classification.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect_true(const std::string_view name, const bool condition) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << name << '\n';
    }
}

struct Fixture final {
    std::filesystem::path root;
    std::unique_ptr<aeris::storage::ProjectStore> project;

    explicit Fixture(const std::string& suffix) {
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() /
               ("aeris-project-surface-layer-" + suffix + '-' + std::to_string(stamp));
        std::filesystem::create_directories(root);
        aeris::storage::ProjectCreateOptions options{};
        options.timestamp_utc = "2026-09-17T09:00:00Z";
        auto created = aeris::storage::ProjectStore::create(root / "surface.aeris", options);
        if (created.ok()) project = std::move(created.store);
    }

    ~Fixture() {
        project.reset();
        std::error_code ignored{};
        std::filesystem::remove_all(root, ignored);
    }
};

[[nodiscard]] aeris::storage::SourceSnapshotRecord source_record(
    const aeris::source::Capability capability
) {
    aeris::storage::SourceSnapshotRecord record{};
    record.source_id = "world.surface.fixture";
    record.adapter_id = "fixture.surface.v1";
    record.capability_bits = aeris::source::capability_bit(capability);
    record.temporal_class = static_cast<std::uint8_t>(aeris::source::TemporalClass::slow_change);
    record.provider = "fixture-provider";
    record.dataset = "surface";
    record.snapshot = "snapshot-1";
    record.dataset_version = "v1";
    record.source_uri = "fixture://surface";
    record.license_id = "CC0-1.0";
    record.content_sha256 = std::string(64U, 'c');
    record.retrieved_at_utc = "2026-09-17T09:00:01Z";
    return record;
}

[[nodiscard]] aeris::storage::SourceGeometryRecord geometry_record() {
    aeris::storage::SourceGeometryRecord geometry{};
    geometry.source_id = "world.surface.fixture";
    aeris::storage::FeatureGeometryRecord feature{};
    feature.stable_id = "surface:alpha";
    feature.source_feature_id = "record:1";
    aeris::storage::GeographicRingRecord ring{};
    ring.role = aeris::storage::StoredRingRole::exterior;
    ring.interior_side = aeris::storage::StoredInteriorSide::right;
    ring.longitude_winding = 0;
    ring.closing_longitude_rad = 0.0;
    ring.vertices = {
        {0.0, 0.0},
        {0.1, 0.0},
        {0.1, 0.1},
        {0.0, 0.1},
    };
    feature.rings.push_back(std::move(ring));
    geometry.features.push_back(std::move(feature));
    return geometry;
}

[[nodiscard]] bool seed_surface(
    aeris::storage::ProjectStore& project,
    const aeris::source::Capability capability,
    std::string class_id
) {
    const auto provenance = aeris::storage::store_source_snapshot(
        project,
        source_record(capability),
        "2026-09-17T09:00:02Z"
    );
    if (!provenance.ok()) return false;
    const auto geometry = aeris::storage::store_source_geometry(
        project,
        geometry_record(),
        "2026-09-17T09:00:03Z"
    );
    if (!geometry.ok()) return false;

    aeris::storage::SourceFeaturePropertiesRecord properties{};
    properties.source_id = "world.surface.fixture";
    aeris::storage::FeaturePropertiesRecord feature{};
    feature.stable_id = "surface:alpha";
    feature.properties.push_back({
        std::string(aeris::surface::kSurfaceClassPropertyKey),
        std::move(class_id),
    });
    properties.features.push_back(std::move(feature));
    return aeris::storage::store_source_feature_properties(
        project,
        properties,
        "2026-09-17T09:00:04Z"
    ).ok();
}

void test_append_retry_and_user_state_preservation() {
    using namespace aeris;

    Fixture fixture("success");
    expect_true("surface project creates", fixture.project != nullptr);
    if (fixture.project == nullptr) return;
    expect_true(
        "canonical surface source seeds",
        seed_surface(
            *fixture.project,
            source::Capability::surface_classification,
            std::string(surface::surface_class_id(surface::SurfaceClass::floating_ice_shelf))
        )
    );

    // Simulate an existing project stack. The semantic channel must append
    // without requiring that old layers be rebuilt or byte-for-byte replaced.
    storage::LayerCreateRequest existing{};
    existing.layer_id = "user.existing";
    existing.role_id = "fixture.existing.v1";
    existing.name = "Existing layer";
    existing.visible = true;
    const auto existing_result = storage::append_layer(
        *fixture.project,
        existing,
        "2026-09-17T09:00:05Z"
    );
    expect_true("existing layer seeds", existing_result.ok() && existing_result.changed);

    const std::uint64_t before = fixture.project->metadata().revision;
    const auto ensured = project::ensure_builtin_surface_classification_layer(
        *fixture.project,
        "world.surface.fixture",
        "2026-09-17T09:00:06Z"
    );
    expect_true(
        "surface layer appends",
        ensured.ok() && ensured.changed && ensured.durably_committed
    );
    expect_true(
        "surface append advances one revision",
        fixture.project->metadata().revision == before + 1U
    );

    auto layers = storage::list_project_layers(*fixture.project);
    expect_true("existing plus surface layers persist", layers.ok() && layers.records.size() == 2U);
    if (layers.ok() && layers.records.size() == 2U) {
        const auto& surface_layer = layers.records[1];
        expect_true(
            "surface layer has canonical identity",
            surface_layer.layer_id == project::kBuiltinSurfaceClassificationLayerId &&
            surface_layer.role_id == storage::kLayerRolePhysicalSurfaceClassificationV1 &&
            surface_layer.sources.size() == 2U &&
            surface_layer.resources.empty()
        );
    }

    storage::LayerStateUpdate user_state{};
    user_state.modified_utc = "2026-09-17T09:00:07Z";
    user_state.name = "My semantic surface";
    user_state.visible = false;
    const auto changed_state = storage::update_layer_state(
        *fixture.project,
        project::kBuiltinSurfaceClassificationLayerId,
        user_state
    );
    expect_true("user surface state commits", changed_state.ok() && changed_state.changed);
    const std::uint64_t user_revision = fixture.project->metadata().revision;

    const auto retry = project::ensure_builtin_surface_classification_layer(
        *fixture.project,
        "world.surface.fixture",
        "2026-09-17T09:00:08Z"
    );
    expect_true(
        "surface structural retry is no-op",
        retry.ok() && !retry.changed && !retry.durably_committed
    );
    expect_true(
        "surface retry preserves user revision",
        fixture.project->metadata().revision == user_revision
    );
    layers = storage::list_project_layers(*fixture.project);
    expect_true(
        "surface retry preserves user name and visibility",
        layers.ok() && layers.records.size() == 2U &&
        layers.records[1].name == "My semantic surface" &&
        !layers.records[1].visible
    );
}

void test_invalid_semantics_fail_closed() {
    using namespace aeris;

    Fixture bad_class("bad-class");
    expect_true("bad-class project creates", bad_class.project != nullptr);
    if (bad_class.project != nullptr) {
        expect_true(
            "bad class source seeds",
            seed_surface(
                *bad_class.project,
                source::Capability::surface_classification,
                "definitely_not_a_surface_class"
            )
        );
        const std::uint64_t before = bad_class.project->metadata().revision;
        const auto result = project::ensure_builtin_surface_classification_layer(
            *bad_class.project,
            "world.surface.fixture",
            "2026-09-17T09:00:06Z"
        );
        expect_true(
            "invalid class is rejected",
            result.error == project::WorldLayerStackError::source_contract_mismatch &&
            !result.changed && !result.durably_committed &&
            bad_class.project->metadata().revision == before
        );
    }

    Fixture wrong_capability("wrong-capability");
    expect_true("wrong-capability project creates", wrong_capability.project != nullptr);
    if (wrong_capability.project != nullptr) {
        expect_true(
            "wrong capability source seeds",
            seed_surface(
                *wrong_capability.project,
                source::Capability::land,
                std::string(surface::surface_class_id(surface::SurfaceClass::land))
            )
        );
        const auto result = project::ensure_builtin_surface_classification_layer(
            *wrong_capability.project,
            "world.surface.fixture",
            "2026-09-17T09:00:06Z"
        );
        expect_true(
            "wrong capability is rejected",
            result.error == project::WorldLayerStackError::source_contract_mismatch
        );
    }
}

}  // namespace

int main() {
    test_append_retry_and_user_state_preservation();
    test_invalid_semantics_fail_closed();

    if (failures != 0) {
        std::cerr << failures << " project surface-layer assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "project_surface_layer: PASS\n";
    return EXIT_SUCCESS;
}
