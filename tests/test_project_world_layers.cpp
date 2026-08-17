// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/project/world_layers.hpp"

#include "aeris/source/adapter.hpp"
#include "aeris/storage/feature_property.hpp"
#include "aeris/storage/geometry.hpp"
#include "aeris/storage/provenance.hpp"

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
               ("aeris-project-world-layers-" + suffix + '-' + std::to_string(stamp));
        std::filesystem::create_directories(root);

        aeris::storage::ProjectCreateOptions options{};
        options.timestamp_utc = "2026-08-17T18:10:00Z";
        auto created = aeris::storage::ProjectStore::create(root / "world.aeris", options);
        if (created.ok()) project = std::move(created.store);
    }

    ~Fixture() {
        project.reset();
        std::error_code ignored{};
        std::filesystem::remove_all(root, ignored);
    }
};

[[nodiscard]] aeris::storage::SourceSnapshotRecord source_record(
    std::string source_id,
    const aeris::source::Capability capability,
    std::string worldview = {}
) {
    aeris::storage::SourceSnapshotRecord record{};
    record.source_id = std::move(source_id);
    record.adapter_id = capability == aeris::source::Capability::land
        ? "fixture.land.v1"
        : "fixture.admin0.v1";
    record.capability_bits = aeris::source::capability_bit(capability);
    record.temporal_class = static_cast<std::uint8_t>(aeris::source::TemporalClass::slow_change);
    record.provider = "fixture-provider";
    record.dataset = capability == aeris::source::Capability::land ? "land" : "admin0";
    record.snapshot = "snapshot-1";
    record.dataset_version = "v1";
    record.source_uri = "fixture://world-layer-source";
    record.license_id = "CC0-1.0";
    record.content_sha256 = capability == aeris::source::Capability::land
        ? std::string(64U, 'a')
        : std::string(64U, 'b');
    record.retrieved_at_utc = "2026-08-17T18:10:01Z";
    record.worldview = std::move(worldview);
    return record;
}

[[nodiscard]] aeris::storage::GeographicRingRecord local_ring() {
    aeris::storage::GeographicRingRecord ring{};
    ring.role = aeris::storage::StoredRingRole::exterior;
    ring.interior_side = aeris::storage::StoredInteriorSide::right;
    ring.longitude_winding = 0;
    ring.closing_longitude_rad = 0.0;
    ring.vertices = {
        {0.0, 0.0},
        {0.20, 0.00},
        {0.20, 0.20},
        {0.00, 0.20},
    };
    return ring;
}

[[nodiscard]] aeris::storage::SourceGeometryRecord geometry_record(
    const std::string& source_id,
    std::string stable_id
) {
    aeris::storage::SourceGeometryRecord geometry{};
    geometry.source_id = source_id;
    aeris::storage::FeatureGeometryRecord feature{};
    feature.stable_id = std::move(stable_id);
    feature.source_feature_id = "record:1";
    feature.rings.push_back(local_ring());
    geometry.features.push_back(std::move(feature));
    return geometry;
}

[[nodiscard]] bool seed_source(
    aeris::storage::ProjectStore& project,
    const aeris::storage::SourceSnapshotRecord& source,
    const std::string& stable_id,
    const bool complete_properties
) {
    const auto provenance = aeris::storage::store_source_snapshot(
        project, source, "2026-08-17T18:10:02Z");
    if (!provenance.ok()) return false;

    const auto geometry = aeris::storage::store_source_geometry(
        project,
        geometry_record(source.source_id, stable_id),
        "2026-08-17T18:10:03Z");
    if (!geometry.ok()) return false;

    if (complete_properties) {
        aeris::storage::SourceFeaturePropertiesRecord properties{};
        properties.source_id = source.source_id;
        aeris::storage::FeaturePropertiesRecord feature{};
        feature.stable_id = stable_id;
        feature.properties.push_back({"NAME", std::string("Alpha")});
        properties.features.push_back(std::move(feature));
        const auto stored = aeris::storage::store_source_feature_properties(
            project, properties, "2026-08-17T18:10:04Z");
        if (!stored.ok()) return false;
    }
    return true;
}

[[nodiscard]] aeris::project::BuiltinWorldLayerSources source_ids() {
    return {"world.physical.primary", "world.political.primary"};
}

void test_success_retry_and_user_state_protection() {
    using namespace aeris;

    Fixture fixture("success");
    expect_true("success project creates", fixture.project != nullptr);
    if (fixture.project == nullptr) return;

    const auto physical = source_record(
        "world.physical.primary", source::Capability::land);
    const auto political = source_record(
        "world.political.primary", source::Capability::admin0, "fixture.de-facto");
    expect_true("physical source seeds",
                seed_source(*fixture.project, physical, "land:alpha", false));
    expect_true("political source seeds",
                seed_source(*fixture.project, political, "country:alpha", true));

    const std::uint64_t before = fixture.project->metadata().revision;
    const auto initialized = project::initialize_builtin_world_layer_stack(
        *fixture.project, source_ids(), "2026-08-17T18:10:05Z");
    expect_true("built-in world stack initializes",
                initialized.ok() && initialized.changed && initialized.durably_committed);
    expect_true("built-in world stack is one revision",
                fixture.project->metadata().revision == before + 1U);

    const auto layers = storage::list_project_layers(*fixture.project);
    expect_true("five built-in layers persist", layers.ok() && layers.records.size() == 5U);
    if (layers.ok() && layers.records.size() == 5U) {
        expect_true("labels top and property-bound",
                    layers.records[0].layer_id == project::kBuiltinPoliticalLabelsLayerId &&
                    layers.records[0].role_id == storage::kLayerRoleCountryLabelV1 &&
                    layers.records[0].sources.size() == 1U &&
                    layers.records[0].sources[0].slot_id == "properties" &&
                    layers.records[0].sources[0].source_id == "world.political.primary");
        expect_true("borders bind political geometry",
                    layers.records[1].layer_id == project::kBuiltinPoliticalBordersLayerId &&
                    layers.records[1].sources.size() == 1U &&
                    layers.records[1].sources[0].slot_id == "geometry");
        expect_true("countries bind geometry and properties",
                    layers.records[2].layer_id == project::kBuiltinPoliticalCountriesLayerId &&
                    layers.records[2].sources.size() == 2U &&
                    layers.records[2].sources[0].slot_id == "geometry" &&
                    layers.records[2].sources[1].slot_id == "properties");
        expect_true("physical layers remain below political stack",
                    layers.records[3].layer_id == project::kBuiltinPhysicalCoastlineLayerId &&
                    layers.records[4].layer_id == project::kBuiltinPhysicalLandLayerId);
    }

    const auto retry = project::initialize_builtin_world_layer_stack(
        *fixture.project, source_ids(), "2026-08-17T18:10:06Z");
    expect_true("exact project composition retry is no-op",
                retry.ok() && !retry.changed && !retry.durably_committed);
    expect_true("exact retry does not advance revision",
                fixture.project->metadata().revision == before + 1U);

    storage::LayerStateUpdate hide{};
    hide.modified_utc = "2026-08-17T18:10:07Z";
    hide.visible = false;
    const auto hidden = storage::update_layer_state(
        *fixture.project, project::kBuiltinPoliticalBordersLayerId, hide);
    expect_true("user layer visibility commits", hidden.ok() && hidden.changed);
    const std::uint64_t user_revision = fixture.project->metadata().revision;

    const auto after_user_edit = project::initialize_builtin_world_layer_stack(
        *fixture.project, source_ids(), "2026-08-17T18:10:08Z");
    expect_true("bootstrap never overwrites user-modified stack",
                after_user_edit.error == project::WorldLayerStackError::storage_rejected &&
                after_user_edit.storage_error == storage::StorageError::record_exists &&
                !after_user_edit.changed && !after_user_edit.durably_committed);
    expect_true("bootstrap conflict preserves user revision",
                fixture.project->metadata().revision == user_revision);
    const auto after = storage::list_project_layers(*fixture.project);
    expect_true("user-hidden border remains hidden",
                after.ok() && after.records.size() == 5U && !after.records[1].visible);
}

void test_incomplete_political_source_is_rejected() {
    using namespace aeris;

    Fixture fixture("incomplete");
    expect_true("incomplete project creates", fixture.project != nullptr);
    if (fixture.project == nullptr) return;

    const auto physical = source_record(
        "world.physical.primary", source::Capability::land);
    const auto political = source_record(
        "world.political.primary", source::Capability::admin0, "fixture.de-facto");
    expect_true("incomplete physical source seeds",
                seed_source(*fixture.project, physical, "land:alpha", false));
    expect_true("geometry-only political source seeds",
                seed_source(*fixture.project, political, "country:alpha", false));
    const std::uint64_t before = fixture.project->metadata().revision;

    const auto result = project::initialize_builtin_world_layer_stack(
        *fixture.project, source_ids(), "2026-08-17T18:10:05Z");
    expect_true("missing complete political properties rejected",
                result.error == project::WorldLayerStackError::source_contract_mismatch &&
                !result.changed && !result.durably_committed);
    expect_true("rejected composition does not advance revision",
                fixture.project->metadata().revision == before);
    const auto layers = storage::list_project_layers(*fixture.project);
    expect_true("rejected composition writes no layers",
                layers.ok() && layers.records.empty());
}

void test_worldview_and_capability_are_required() {
    using namespace aeris;

    Fixture fixture("contract");
    expect_true("contract project creates", fixture.project != nullptr);
    if (fixture.project == nullptr) return;

    const auto physical = source_record(
        "world.physical.primary", source::Capability::admin0);
    const auto political = source_record(
        "world.political.primary", source::Capability::admin0);
    expect_true("wrong-capability physical source seeds",
                seed_source(*fixture.project, physical, "land:alpha", false));
    expect_true("worldviewless political source seeds",
                seed_source(*fixture.project, political, "country:alpha", true));
    const std::uint64_t before = fixture.project->metadata().revision;

    const auto result = project::initialize_builtin_world_layer_stack(
        *fixture.project, source_ids(), "2026-08-17T18:10:05Z");
    expect_true("capability/worldview contract rejected",
                result.error == project::WorldLayerStackError::source_contract_mismatch);
    expect_true("contract mismatch writes nothing",
                fixture.project->metadata().revision == before &&
                storage::list_project_layers(*fixture.project).records.empty());
}

}  // namespace

int main() {
    test_success_retry_and_user_state_protection();
    test_incomplete_political_source_is_rejected();
    test_worldview_and_capability_are_required();

    if (failures != 0) {
        std::cerr << failures << " project world-layer assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "project_world_layers: PASS\n";
    return EXIT_SUCCESS;
}
