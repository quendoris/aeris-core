// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/storage/dataset.hpp"
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
    const aeris::storage::StorageError expected
) {
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
                ("aeris-layer-stack-atomic-" + std::to_string(stamp));
        std::filesystem::create_directories(root_);

        asset_path_ = root_ / "optional.bin";
        {
            std::ofstream output(asset_path_, std::ios::binary | std::ios::trunc);
            output << "AERIS atomic layer stack optional resource";
        }

        aeris::storage::ProjectCreateOptions options{};
        options.timestamp_utc = "2026-08-17T17:50:00Z";
        options.project_uuid = "34135f4e-e7d3-4f3d-97ea-c317ea175408";
        auto created = aeris::storage::ProjectStore::create(root_ / "world.aeris", options);
        if (created.ok()) project_ = std::move(created.store);
    }

    ~Fixture() {
        project_.reset();
        std::error_code ignored{};
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] aeris::storage::ProjectStore* project() noexcept {
        return project_.get();
    }

    [[nodiscard]] const std::filesystem::path& asset_path() const noexcept {
        return asset_path_;
    }

private:
    std::filesystem::path root_;
    std::filesystem::path asset_path_;
    std::unique_ptr<aeris::storage::ProjectStore> project_;
};

[[nodiscard]] aeris::storage::SourceSnapshotRecord source_record() {
    aeris::storage::SourceSnapshotRecord source{};
    source.source_id = "world.political.primary";
    source.adapter_id = "fixture.admin0.v1";
    source.capability_bits = 1U;
    source.temporal_class = 1U;
    source.provider = "fixture-provider";
    source.dataset = "admin0";
    source.snapshot = "snapshot-1";
    source.dataset_version = "v1";
    source.source_uri = "fixture://admin0";
    source.license_id = "CC0-1.0";
    source.content_sha256 = std::string(64U, 'b');
    source.retrieved_at_utc = "2026-08-17T17:50:01Z";
    return source;
}

[[nodiscard]] aeris::storage::ProjectResourceIdentity optional_resource(
    const std::filesystem::path& path
) {
    aeris::storage::ProjectResourceIdentity resource{};
    resource.resource_id = "labels.font.primary";
    resource.sha256 = aeris::util::sha256_file(path).digest.hex();
    resource.media_type = "application/octet-stream";
    resource.size_bytes = static_cast<std::uint64_t>(std::filesystem::file_size(path));
    resource.retrieval_uri = "https://example.invalid/aeris/font";
    resource.required_for_reproduction = false;
    return resource;
}

[[nodiscard]] std::vector<aeris::storage::LayerCreateRequest> good_stack() {
    using namespace aeris::storage;

    LayerCreateRequest labels{};
    labels.layer_id = "builtin.political.labels";
    labels.role_id = std::string(kLayerRoleCountryLabelV1);
    labels.name = "Country labels";
    labels.visible = true;
    labels.sources.push_back({"properties", "world.political.primary"});
    labels.resources.push_back({"font", "labels.font.primary"});

    LayerCreateRequest borders{};
    borders.layer_id = "builtin.political.borders";
    borders.role_id = std::string(kLayerRolePoliticalBoundaryV1);
    borders.name = "Borders";
    borders.visible = true;
    borders.sources.push_back({"geometry", "world.political.primary"});

    LayerCreateRequest countries{};
    countries.layer_id = "builtin.political.countries";
    countries.role_id = std::string(kLayerRolePoliticalCountryFillV1);
    countries.name = "Countries";
    countries.visible = true;
    // Deliberately unsorted input proves slot canonicalization is not caller-order dependent.
    countries.sources.push_back({"properties", "world.political.primary"});
    countries.sources.push_back({"geometry", "world.political.primary"});

    return {labels, borders, countries};
}

void seed_project(Fixture& fixture) {
    using namespace aeris::storage;
    ProjectStore* project = fixture.project();
    if (project == nullptr) return;

    SourceDatasetRecord source_dataset{};
    source_dataset.source = source_record();
    source_dataset.geometry.source_id = source_dataset.source.source_id;
    const auto source = store_source_dataset(
        *project, source_dataset, "2026-08-17T17:50:02Z");
    expect_true("seed materialized source stores", source.ok());

    const auto resource = store_external_resource(
        *project,
        optional_resource(fixture.asset_path()),
        "2026-08-17T17:50:03Z");
    expect_true("seed optional resource stores", resource.ok());

    const auto frozen = freeze_project(*project, "2026-08-17T17:50:04Z");
    expect_true("seed project freezes", frozen.ok());
    expect_true("seed reaches revision three", project->metadata().revision == 3U);
    expect_true("seed project frozen", project->metadata().frozen);
}

void test_success_and_exact_retry() {
    using namespace aeris::storage;
    Fixture fixture{};
    ProjectStore* project = fixture.project();
    expect_true("success fixture creates", project != nullptr);
    if (project == nullptr) return;
    seed_project(fixture);

    const auto stack = good_stack();
    const auto initialized = initialize_layer_stack(
        *project, stack, "2026-08-17T17:50:05Z");
    expect_true("stack initializes",
                initialized.ok() && initialized.changed && initialized.durably_committed);
    expect_true("entire stack is one revision", project->metadata().revision == 4U);
    expect_true("required external resource thaws project", !project->metadata().frozen);

    const auto listed = list_project_layers(*project);
    expect_true("three layers list", listed.ok() && listed.records.size() == 3U);
    if (listed.ok() && listed.records.size() == 3U) {
        expect_true("labels ordinal zero",
                    listed.records[0].layer_id == "builtin.political.labels" &&
                    listed.records[0].ordinal == 0U);
        expect_true("borders ordinal one",
                    listed.records[1].layer_id == "builtin.political.borders" &&
                    listed.records[1].ordinal == 1U);
        expect_true("countries ordinal two",
                    listed.records[2].layer_id == "builtin.political.countries" &&
                    listed.records[2].ordinal == 2U);
        expect_true("country source slots canonicalized",
                    listed.records[2].sources.size() == 2U &&
                    listed.records[2].sources[0].slot_id == "geometry" &&
                    listed.records[2].sources[1].slot_id == "properties");
    }

    auto retry_stack = good_stack();
    std::swap(retry_stack[2].sources[0], retry_stack[2].sources[1]);
    const auto retry = initialize_layer_stack(
        *project, retry_stack, "2026-08-17T17:50:06Z");
    expect_true("exact stack retry is idempotent",
                retry.ok() && !retry.changed && !retry.durably_committed);
    expect_true("exact retry keeps revision four", project->metadata().revision == 4U);

    auto conflict_stack = good_stack();
    conflict_stack[1].visible = false;
    const auto conflict = initialize_layer_stack(
        *project, conflict_stack, "2026-08-17T17:50:07Z");
    expect_error("different existing stack rejected", conflict.status, StorageError::record_exists);
    expect_true("conflict keeps revision four", project->metadata().revision == 4U);
}

void test_late_failure_rolls_back_everything() {
    using namespace aeris::storage;
    Fixture fixture{};
    ProjectStore* project = fixture.project();
    expect_true("rollback fixture creates", project != nullptr);
    if (project == nullptr) return;
    seed_project(fixture);

    auto stack = good_stack();
    // The first layer binds and promotes the optional external resource. The
    // final layer then fails its source reference. Both effects must roll back.
    stack[2].sources.clear();
    stack[2].sources.push_back({"geometry", "source.does.not.exist"});

    const auto failed = initialize_layer_stack(
        *project, stack, "2026-08-17T17:50:05Z");
    expect_error("late missing source rejects whole stack",
                 failed.status, StorageError::record_not_found);
    expect_true("failed stack reports no durable commit",
                !failed.changed && !failed.durably_committed);
    expect_true("failed stack keeps revision three", project->metadata().revision == 3U);
    expect_true("failed stack preserves frozen project", project->metadata().frozen);

    const auto layers = list_project_layers(*project);
    expect_true("failed stack leaves zero layers", layers.ok() && layers.records.empty());

    const auto resources = list_project_resources(*project);
    expect_true("rollback resource still exists",
                resources.ok() && resources.records.size() == 1U);
    if (resources.ok() && resources.records.size() == 1U) {
        expect_true("rollback undoes resource requirement promotion",
                    !resources.records.front().identity.required_for_reproduction);
    }
}

void test_preflight_rejects_duplicate_ids() {
    using namespace aeris::storage;
    Fixture fixture{};
    ProjectStore* project = fixture.project();
    expect_true("duplicate fixture creates", project != nullptr);
    if (project == nullptr) return;

    auto stack = good_stack();
    stack[1].layer_id = stack[0].layer_id;
    const auto duplicate = initialize_layer_stack(
        *project, stack, "2026-08-17T17:50:01Z");
    expect_error("duplicate layer IDs rejected preflight",
                 duplicate.status, StorageError::invalid_argument);
    expect_true("preflight failure keeps revision zero", project->metadata().revision == 0U);
    const auto layers = list_project_layers(*project);
    expect_true("preflight failure writes no layers", layers.ok() && layers.records.empty());
}

}  // namespace

int main() {
    test_success_and_exact_retry();
    test_late_failure_rolls_back_everything();
    test_preflight_rejects_duplicate_ids();

    if (failures != 0) {
        std::cerr << failures << " layer-stack atomicity assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "layer stack atomicity: PASS\n";
    return EXIT_SUCCESS;
}
