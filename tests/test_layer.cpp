// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/storage/layer.hpp"
#include "aeris/storage/provenance.hpp"
#include "aeris/storage/resource.hpp"
#include "aeris/util/sha256.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <sqlite3.h>

namespace {

int failures = 0;

void expect_true(const std::string_view name, const bool condition) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << name << '\n';
    }
}

void expect_error(
    const std::string_view name,
    const aeris::storage::Status& status,
    const aeris::storage::StorageError expected) {
    if (status.error != expected) {
        ++failures;
        std::cerr << "FAIL " << name
                  << " expected=" << static_cast<int>(expected)
                  << " actual=" << static_cast<int>(status.error)
                  << " diagnostic=" << status.diagnostic << '\n';
    }
}

class Fixture final {
public:
    Fixture() {
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path() /
                ("aeris-layer-v0-" + std::to_string(stamp));
        std::filesystem::create_directories(root_);

        asset_path_ = root_ / "flag-atlas.bin";
        {
            std::ofstream output(asset_path_, std::ios::binary | std::ios::trunc);
            output << "AERIS deterministic flag-atlas fixture bytes";
        }

        aeris::storage::ProjectCreateOptions options{};
        options.timestamp_utc = "2026-08-17T09:20:00Z";
        options.project_uuid = "abcdef01-2345-4abc-8def-1234567890ab";
        auto created = aeris::storage::ProjectStore::create(root_ / "world.aeris", options);
        if (created.ok()) project_ = std::move(created.store);
    }

    ~Fixture() {
        project_.reset();
        std::error_code ignored{};
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] aeris::storage::ProjectStore* project() noexcept { return project_.get(); }
    [[nodiscard]] const std::filesystem::path& asset_path() const noexcept { return asset_path_; }
    [[nodiscard]] std::filesystem::path project_path() const { return root_ / "world.aeris"; }

private:
    std::filesystem::path root_;
    std::filesystem::path asset_path_;
    std::unique_ptr<aeris::storage::ProjectStore> project_;
};

[[nodiscard]] aeris::storage::SourceSnapshotRecord source_record() {
    aeris::storage::SourceSnapshotRecord source{};
    source.source_id = "world.political.primary";
    source.adapter_id = "fixture.political.v1";
    source.capability_bits = 1U;
    source.temporal_class = 1U;
    source.provider = "fixture-provider";
    source.dataset = "political-world";
    source.snapshot = "snapshot-1";
    source.dataset_version = "v1";
    source.source_uri = "fixture://political-world";
    source.license_id = "CC0-1.0";
    source.content_sha256 = std::string(64U, 'a');
    source.retrieved_at_utc = "2026-08-17T09:20:01Z";
    return source;
}

[[nodiscard]] aeris::storage::ProjectResourceIdentity optional_asset(
    const std::filesystem::path& path) {
    aeris::storage::ProjectResourceIdentity resource{};
    resource.resource_id = "flags.atlas.primary";
    resource.sha256 = aeris::util::sha256_file(path).digest.hex();
    resource.media_type = "application/octet-stream";
    resource.size_bytes = static_cast<std::uint64_t>(std::filesystem::file_size(path));
    resource.retrieval_uri = "https://example.invalid/aeris/flags-atlas";
    resource.required_for_reproduction = false;
    return resource;
}

[[nodiscard]] aeris::storage::LayerCreateRequest political_layer() {
    aeris::storage::LayerCreateRequest layer{};
    layer.layer_id = "layer.political.land";
    layer.role_id = "aeris.layer.political.land.v1";
    layer.name = "Political land";
    layer.visible = true;
    layer.sources.push_back({"geometry", "world.political.primary"});
    layer.resources.push_back({"flags", "flags.atlas.primary"});
    return layer;
}

void test_layer_graph_and_portability() {
    using namespace aeris::storage;

    Fixture fixture{};
    ProjectStore* project = fixture.project();
    expect_true("layer project creates", project != nullptr);
    if (project == nullptr) return;
    expect_true("generation 5 project advertises draft 0.5",
                project->metadata().format_major == 0 && project->metadata().format_minor == 5);

    const SourceSnapshotRecord source = source_record();
    const auto source_insert = store_source_snapshot(
        *project, source, "2026-08-17T09:20:02Z");
    expect_true("layer source provenance stores", source_insert.ok());
    expect_true("source insertion is revision one", project->metadata().revision == 1U);

    const ProjectResourceIdentity asset = optional_asset(fixture.asset_path());
    const auto resource_insert = store_external_resource(
        *project, asset, "2026-08-17T09:20:03Z");
    expect_true("optional layer asset stores externally", resource_insert.ok());
    expect_true("resource insertion is revision two", project->metadata().revision == 2U);

    const auto initial_freeze = freeze_project(*project, "2026-08-17T09:20:04Z");
    expect_true("project with only optional external asset can freeze", initial_freeze.ok());
    expect_true("initial freeze is revision three", project->metadata().revision == 3U);
    expect_true("project begins layer test frozen", project->metadata().frozen);

    LayerCreateRequest rollback_layer = political_layer();
    rollback_layer.layer_id = "layer.rollback";
    rollback_layer.name = "Rollback fixture";
    rollback_layer.resources.push_back({"missing", "resource.does.not.exist"});
    const auto rolled_back = append_layer(
        *project, rollback_layer, "2026-08-17T09:20:05Z");
    expect_error("missing second resource fails layer transaction",
                 rolled_back.status, StorageError::record_not_found);
    expect_true("failed layer transaction keeps revision three", project->metadata().revision == 3U);
    expect_true("failed layer transaction preserves frozen state", project->metadata().frozen);
    const auto after_rollback_layers = list_project_layers(*project);
    expect_true("failed layer transaction leaves no layer",
                after_rollback_layers.ok() && after_rollback_layers.records.empty());
    const auto after_rollback_resources = list_project_resources(*project);
    expect_true("failed layer transaction leaves resource catalog intact",
                after_rollback_resources.ok() && after_rollback_resources.records.size() == 1U);
    if (after_rollback_resources.ok() && after_rollback_resources.records.size() == 1U) {
        expect_true("failed layer transaction rolls requirement promotion back",
                    !after_rollback_resources.records.front().identity.required_for_reproduction);
    }

    LayerCreateRequest political = political_layer();
    const auto appended = append_layer(
        *project, political, "2026-08-17T09:20:06Z");
    expect_true("political layer appends", appended.ok() && appended.changed && appended.durably_committed);
    expect_true("successful layer append is one revision", project->metadata().revision == 4U);
    expect_true("binding external layer asset atomically thaws project", !project->metadata().frozen);

    const auto promoted_resource = list_project_resources(*project);
    expect_true("bound resource still lists", promoted_resource.ok() && promoted_resource.records.size() == 1U);
    if (promoted_resource.ok() && promoted_resource.records.size() == 1U) {
        expect_true("layer binding promotes resource to required",
                    promoted_resource.records.front().identity.required_for_reproduction);
        expect_true("bound resource remains external before embed",
                    promoted_resource.records.front().storage_mode == ResourceStorageMode::external);
    }

    const auto first_list = list_project_layers(*project);
    expect_true("layer list contains political layer", first_list.ok() && first_list.records.size() == 1U);
    if (first_list.ok() && first_list.records.size() == 1U) {
        const auto& layer = first_list.records.front();
        expect_true("political layer role persists", layer.role_id == "aeris.layer.political.land.v1");
        expect_true("political layer ordinal zero", layer.ordinal == 0U);
        expect_true("political layer visible", layer.visible);
        expect_true("political source slot persists",
                    layer.sources.size() == 1U && layer.sources.front().slot_id == "geometry" &&
                    layer.sources.front().source_id == source.source_id);
        expect_true("flag resource slot persists",
                    layer.resources.size() == 1U && layer.resources.front().slot_id == "flags" &&
                    layer.resources.front().resource_id == asset.resource_id);
    }

    const auto append_retry = append_layer(
        *project, political, "2026-08-17T09:20:07Z");
    expect_true("exact layer append retry is idempotent",
                append_retry.ok() && !append_retry.changed && !append_retry.durably_committed);
    expect_true("exact layer retry keeps revision four", project->metadata().revision == 4U);

    LayerCreateRequest labels{};
    labels.layer_id = "layer.labels";
    labels.role_id = "aeris.layer.labels.v1";
    labels.name = "Labels";
    labels.visible = true;
    labels.sources.push_back({"places", source.source_id});
    const auto labels_append = append_layer(
        *project, labels, "2026-08-17T09:20:08Z");
    expect_true("second modular layer appends", labels_append.ok());
    expect_true("second layer is revision five", project->metadata().revision == 5U);

    const std::vector<std::string> reordered{"layer.labels", "layer.political.land"};
    const auto reorder = set_layer_order(
        *project, reordered, "2026-08-17T09:20:09Z");
    expect_true("layer stack reorders atomically", reorder.ok() && reorder.changed);
    expect_true("reorder is revision six", project->metadata().revision == 6U);
    const auto reordered_list = list_project_layers(*project);
    expect_true("reordered stack lists", reordered_list.ok() && reordered_list.records.size() == 2U);
    if (reordered_list.ok() && reordered_list.records.size() == 2U) {
        expect_true("labels now ordinal zero",
                    reordered_list.records[0].layer_id == "layer.labels" &&
                    reordered_list.records[0].ordinal == 0U);
        expect_true("political now ordinal one",
                    reordered_list.records[1].layer_id == "layer.political.land" &&
                    reordered_list.records[1].ordinal == 1U);
    }

    const auto reorder_retry = set_layer_order(
        *project, reordered, "2026-08-17T09:20:10Z");
    expect_true("exact layer reorder is idempotent",
                reorder_retry.ok() && !reorder_retry.changed);
    expect_true("exact reorder keeps revision six", project->metadata().revision == 6U);

    LayerStateUpdate state{};
    state.modified_utc = "2026-08-17T09:20:11Z";
    state.name = "Political world";
    state.visible = false;
    const auto state_update = update_layer_state(
        *project, political.layer_id, state);
    expect_true("layer state update commits", state_update.ok() && state_update.changed);
    expect_true("layer state update is revision seven", project->metadata().revision == 7U);

    LayerStateUpdate same_state = state;
    same_state.modified_utc = "2026-08-17T09:20:12Z";
    const auto state_retry = update_layer_state(
        *project, political.layer_id, same_state);
    expect_true("exact layer state retry is idempotent",
                state_retry.ok() && !state_retry.changed);
    expect_true("state retry keeps revision seven", project->metadata().revision == 7U);

    const auto blocked_freeze = freeze_project(*project, "2026-08-17T09:20:13Z");
    expect_error("layer-bound external resource blocks freeze",
                 blocked_freeze.status, StorageError::invalid_argument);
    expect_true("blocked freeze keeps revision seven", project->metadata().revision == 7U);

    const auto embedded = embed_resource_file(
        *project, asset, fixture.asset_path(), "2026-08-17T09:20:14Z");
    expect_true("layer asset embeds", embedded.ok());
    expect_true("embedding layer asset is revision eight", project->metadata().revision == 8U);

    const auto refreeze = freeze_project(*project, "2026-08-17T09:20:15Z");
    expect_true("layer project refreezes after asset embed", refreeze.ok());
    expect_true("refreeze is revision nine", project->metadata().revision == 9U);
    expect_true("layer project is frozen", project->metadata().frozen);

    LayerCreateRequest flags{};
    flags.layer_id = "layer.flags";
    flags.role_id = "aeris.layer.flags.satellites.v1";
    flags.name = "Flag satellites";
    flags.visible = true;
    flags.sources.push_back({"anchors", source.source_id});
    flags.resources.push_back({"atlas", asset.resource_id});
    const auto flags_append = append_layer(
        *project, flags, "2026-08-17T09:20:16Z");
    expect_true("third layer can reuse embedded required asset", flags_append.ok());
    expect_true("third layer is revision ten", project->metadata().revision == 10U);
    expect_true("binding already-embedded asset preserves frozen state", project->metadata().frozen);

    const auto removed = remove_layer(
        *project, labels.layer_id, "2026-08-17T09:20:17Z");
    expect_true("layer removal commits", removed.ok() && removed.changed);
    expect_true("layer removal is revision eleven", project->metadata().revision == 11U);
    const auto after_remove = list_project_layers(*project);
    expect_true("remaining layer stack compacts", after_remove.ok() && after_remove.records.size() == 2U);
    if (after_remove.ok() && after_remove.records.size() == 2U) {
        expect_true("political layer compacts to ordinal zero",
                    after_remove.records[0].layer_id == political.layer_id &&
                    after_remove.records[0].ordinal == 0U);
        expect_true("flags layer compacts to ordinal one",
                    after_remove.records[1].layer_id == flags.layer_id &&
                    after_remove.records[1].ordinal == 1U);
    }
    expect_true("layer removal never deletes shared resource",
                list_project_resources(*project).records.size() == 1U);
    expect_true("valid modular layer graph passes deep integrity",
                project->verify_integrity().ok());

    sqlite3* raw = nullptr;
    const std::string path = fixture.project_path().string();
    const int open_rc = sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr);
    expect_true("hostile layer database opens", open_rc == SQLITE_OK && raw != nullptr);
    if (open_rc == SQLITE_OK && raw != nullptr) {
        char* error = nullptr;
        const int rc = sqlite3_exec(
            raw,
            "UPDATE aeris_resource SET required_for_reproduction=0 "
            "WHERE resource_id='flags.atlas.primary';",
            nullptr, nullptr, &error);
        if (error != nullptr) sqlite3_free(error);
        expect_true("hostile requirement downgrade writes", rc == SQLITE_OK);
        sqlite3_close(raw);
        raw = nullptr;
    }

    const Status hostile = project->verify_integrity();
    expect_error("deep integrity rejects layer bound to non-required resource",
                 hostile, StorageError::schema_invalid);
}

}  // namespace

int main() {
    test_layer_graph_and_portability();

    if (failures != 0) {
        std::cerr << failures << " layer assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "project_storage_layer: PASS\n";
    return EXIT_SUCCESS;
}
