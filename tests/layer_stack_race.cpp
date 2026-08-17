// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/storage/layer.hpp"
#include "aeris/storage/provenance.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using aeris::storage::LayerCreateRequest;
using aeris::storage::LayerMutationResult;
using aeris::storage::ProjectStore;
using aeris::storage::StorageError;

struct Fixture final {
    std::filesystem::path root;
    std::filesystem::path project_path;

    explicit Fixture(const std::string& suffix) {
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() /
               ("aeris-layer-stack-race-" + suffix + '-' + std::to_string(stamp));
        project_path = root / "world.aeris";
        std::filesystem::create_directories(root);

        aeris::storage::ProjectCreateOptions options{};
        options.timestamp_utc = "2026-08-17T18:00:00Z";
        options.project_uuid = suffix == "same"
            ? "098b2587-c379-4bb4-8853-0f8cc9aa398a"
            : "5385b302-eb60-4ac4-a5e3-c4d65019417b";
        auto created = ProjectStore::create(project_path, options);
        if (!created.ok()) return;

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
        source.content_sha256 = std::string(64U, 'c');
        source.retrieved_at_utc = "2026-08-17T18:00:01Z";
        const auto stored = aeris::storage::store_source_snapshot(
            *created.store, source, "2026-08-17T18:00:02Z");
        if (!stored.ok()) return;
        seeded = true;
    }

    ~Fixture() {
        std::error_code ignored{};
        std::filesystem::remove_all(root, ignored);
    }

    bool seeded{false};
};

[[nodiscard]] std::vector<LayerCreateRequest> stack_variant(const bool labels_visible) {
    using namespace aeris::storage;

    LayerCreateRequest labels{};
    labels.layer_id = "builtin.political.labels";
    labels.role_id = std::string(kLayerRoleCountryLabelV1);
    labels.name = "Country labels";
    labels.visible = labels_visible;
    labels.sources.push_back({"properties", "world.political.primary"});

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
    countries.sources.push_back({"geometry", "world.political.primary"});
    countries.sources.push_back({"properties", "world.political.primary"});

    return {labels, borders, countries};
}

void wait_for_start(std::atomic<int>& ready, std::atomic<bool>& go) {
    ready.fetch_add(1, std::memory_order_release);
    while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
}

[[nodiscard]] bool same_final_stack(
    const std::filesystem::path& path,
    const bool labels_visible
) {
    auto opened = ProjectStore::open(path);
    if (!opened.ok()) {
        std::cerr << "final project reopen failed: " << opened.status.diagnostic << '\n';
        return false;
    }
    if (opened.store->metadata().revision != 2U) {
        std::cerr << "race advanced project by more than one stack revision: "
                  << opened.store->metadata().revision << '\n';
        return false;
    }
    const auto listed = aeris::storage::list_project_layers(*opened.store);
    if (!listed.ok() || listed.records.size() != 3U) {
        std::cerr << "final race stack is missing or partial\n";
        return false;
    }
    const auto& labels = listed.records[0];
    const auto& borders = listed.records[1];
    const auto& countries = listed.records[2];
    return labels.layer_id == "builtin.political.labels" && labels.ordinal == 0U &&
           labels.visible == labels_visible && labels.sources.size() == 1U &&
           borders.layer_id == "builtin.political.borders" && borders.ordinal == 1U &&
           borders.visible && borders.sources.size() == 1U &&
           countries.layer_id == "builtin.political.countries" && countries.ordinal == 2U &&
           countries.visible && countries.sources.size() == 2U;
}

[[nodiscard]] bool run_same_stack_race() {
    Fixture fixture("same");
    if (!fixture.seeded) return false;

    auto first_open = ProjectStore::open(fixture.project_path);
    auto second_open = ProjectStore::open(fixture.project_path);
    if (!first_open.ok() || !second_open.ok()) return false;

    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    LayerMutationResult outcomes[2];
    const auto stack = stack_variant(true);

    const auto worker = [&](const int index, ProjectStore& project) {
        wait_for_start(ready, go);
        outcomes[index] = aeris::storage::initialize_layer_stack(
            project, stack, "2026-08-17T18:00:03Z");
    };

    std::thread first(worker, 0, std::ref(*first_open.store));
    std::thread second(worker, 1, std::ref(*second_open.store));
    while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    first.join();
    second.join();

    const int changed = static_cast<int>(outcomes[0].ok() && outcomes[0].changed &&
                                         outcomes[0].durably_committed) +
                        static_cast<int>(outcomes[1].ok() && outcomes[1].changed &&
                                         outcomes[1].durably_committed);
    const int noops = static_cast<int>(outcomes[0].ok() && !outcomes[0].changed &&
                                       !outcomes[0].durably_committed) +
                      static_cast<int>(outcomes[1].ok() && !outcomes[1].changed &&
                                       !outcomes[1].durably_committed);
    if (changed != 1 || noops != 1) {
        std::cerr << "same-stack race expected one commit and one exact no-op\n";
        return false;
    }
    return same_final_stack(fixture.project_path, true);
}

[[nodiscard]] bool run_different_stack_race() {
    Fixture fixture("different");
    if (!fixture.seeded) return false;

    auto first_open = ProjectStore::open(fixture.project_path);
    auto second_open = ProjectStore::open(fixture.project_path);
    if (!first_open.ok() || !second_open.ok()) return false;

    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    LayerMutationResult outcomes[2];
    const auto visible_stack = stack_variant(true);
    const auto hidden_stack = stack_variant(false);

    const auto worker = [&] (
        const int index,
        ProjectStore& project,
        const std::vector<LayerCreateRequest>& stack
    ) {
        wait_for_start(ready, go);
        outcomes[index] = aeris::storage::initialize_layer_stack(
            project, stack, "2026-08-17T18:00:03Z");
    };

    std::thread first(worker, 0, std::ref(*first_open.store), std::cref(visible_stack));
    std::thread second(worker, 1, std::ref(*second_open.store), std::cref(hidden_stack));
    while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    first.join();
    second.join();

    const int changed = static_cast<int>(outcomes[0].ok() && outcomes[0].changed &&
                                         outcomes[0].durably_committed) +
                        static_cast<int>(outcomes[1].ok() && outcomes[1].changed &&
                                         outcomes[1].durably_committed);
    const int conflicts = static_cast<int>(outcomes[0].status.error == StorageError::record_exists) +
                          static_cast<int>(outcomes[1].status.error == StorageError::record_exists);
    if (changed != 1 || conflicts != 1) {
        std::cerr << "different-stack race expected one commit and one conflict\n";
        return false;
    }

    const bool winning_visibility = outcomes[0].ok() && outcomes[0].changed;
    return same_final_stack(fixture.project_path, winning_visibility);
}

}  // namespace

int main() {
    if (!run_same_stack_race()) return EXIT_FAILURE;
    if (!run_different_stack_race()) return EXIT_FAILURE;
    std::cout << "layer stack race: PASS\n";
    return EXIT_SUCCESS;
}
