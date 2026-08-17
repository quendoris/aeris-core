// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#include "aeris/project/dataset_ingest.hpp"
#include "aeris/project/source_bridge.hpp"
#include "aeris/storage/geometry.hpp"
#include "aeris/storage/provenance.hpp"
#include "aeris/util/sha256.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

int failures = 0;

void expect_true(const std::string_view name, const bool condition) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << name << '\n';
    }
}

class TempSnapshot final {
public:
    TempSnapshot() {
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path() /
                ("aeris-project-dataset-" + std::to_string(stamp));
        std::filesystem::create_directories(root_);
        const auto path = root_ / "data.bin";
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << "atomic verified dataset bytes";
        }

        aeris::source::SnapshotManifest manifest{};
        manifest.provider = "dataset-provider";
        manifest.dataset = "dataset-world";
        manifest.snapshot = "snapshot-1";
        manifest.source_uri = "fixture://project-dataset";
        manifest.retrieved_at_utc = "2026-08-17T09:10:00Z";
        manifest.resources.push_back({
            "geometry",
            "data.bin",
            aeris::util::sha256_file(path).digest.hex(),
            std::filesystem::file_size(path),
        });
        auto verified = aeris::source::verify_local_snapshot(root_, manifest);
        if (verified.ok()) snapshot_ = std::move(verified.snapshot);
    }

    ~TempSnapshot() {
        std::error_code ignored{};
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] const aeris::source::VerifiedSnapshot* get() const noexcept {
        return snapshot_.has_value() ? &*snapshot_ : nullptr;
    }

private:
    std::filesystem::path root_;
    std::optional<aeris::source::VerifiedSnapshot> snapshot_;
};

enum class AdapterMode {
    valid,
    empty,
    duplicate_source_feature_id,
};

class DatasetAdapter final : public aeris::source::Adapter {
public:
    explicit DatasetAdapter(const AdapterMode mode) : mode_(mode) {}

    [[nodiscard]] aeris::source::AdapterDescriptor descriptor() const noexcept override {
        return {
            "dataset.adapter.v1",
            "dataset-provider",
            aeris::source::capability_bit(aeris::source::Capability::land),
            aeris::source::TemporalClass::slow_change,
        };
    }

    [[nodiscard]] aeris::source::Result load(
        const aeris::source::VerifiedSnapshot& snapshot,
        const aeris::source::Request& request
    ) const override {
        aeris::source::Result result{};
        result.provenance.provider = "dataset-provider";
        result.provenance.dataset = "dataset-world";
        result.provenance.snapshot = request.snapshot;
        result.provenance.dataset_version = "fixture-v1";
        result.provenance.source_uri = snapshot.manifest().source_uri;
        result.provenance.license_id = "CC0-1.0";
        result.provenance.content_sha256 = snapshot.content_sha256();
        result.provenance.retrieved_at_utc = snapshot.manifest().retrieved_at_utc;
        result.provenance.worldview = request.worldview;

        if (mode_ == AdapterMode::empty) return result;

        auto make_feature = [](std::string stable_id, std::string source_id, const double offset) {
            aeris::source::Feature feature{};
            feature.stable_id = std::move(stable_id);
            feature.source_id = std::move(source_id);

            aeris::geometry::LinearRing ring{};
            ring.vertices = {
                {offset + 0.00, 0.00},
                {offset + 0.10, 0.00},
                {offset + 0.10, 0.10},
                {offset + 0.00, 0.10},
            };
            ring.closing_longitude_rad = offset;
            ring.longitude_winding = 0;
            ring.interior_side = aeris::geometry::RingInteriorSide::right;
            feature.rings.push_back({std::move(ring), aeris::source::RingRole::exterior});
            return feature;
        };

        if (mode_ == AdapterMode::duplicate_source_feature_id) {
            result.features.push_back(make_feature("feature:a", "record:duplicate", -0.3));
            result.features.push_back(make_feature("feature:b", "record:duplicate", 0.3));
        } else {
            result.features.push_back(make_feature("feature:land", "record:1", 0.0));
        }
        return result;
    }

private:
    AdapterMode mode_;
};

class TempProject final {
public:
    explicit TempProject(const std::string& tag) {
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path() /
                ("aeris-project-dataset-db-" + tag + "-" + std::to_string(stamp));
        std::filesystem::create_directories(root_);

        aeris::storage::ProjectCreateOptions options{};
        options.timestamp_utc = "2026-08-17T09:10:01Z";
        auto created = aeris::storage::ProjectStore::create(root_ / "world.aeris", options);
        if (created.ok()) store_ = std::move(created.store);
    }

    ~TempProject() {
        store_.reset();
        std::error_code ignored{};
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] aeris::storage::ProjectStore* get() noexcept { return store_.get(); }
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }
    void close() noexcept { store_.reset(); }

private:
    std::filesystem::path root_;
    std::unique_ptr<aeris::storage::ProjectStore> store_;
};

aeris::project::VerifiedSourceRecordRequest request_for(
    const aeris::source::VerifiedSnapshot& snapshot,
    std::string source_id) {
    aeris::project::VerifiedSourceRecordRequest request{};
    request.source_id = std::move(source_id);
    request.binding.adapter_id = "dataset.adapter.v1";
    request.binding.capability = aeris::source::Capability::land;
    request.binding.snapshot = snapshot.manifest().snapshot;
    request.binding.expected_content_sha256 = snapshot.content_sha256();
    request.modified_utc = "2026-08-17T09:10:02Z";
    return request;
}

std::unique_ptr<aeris::source::AdapterRegistry> registry_for(const AdapterMode mode) {
    auto registry = std::make_unique<aeris::source::AdapterRegistry>();
    if (registry->add(std::make_unique<DatasetAdapter>(mode)) != aeris::source::RegistryError::none) {
        return {};
    }
    return registry;
}

void test_verified_dataset_is_one_revision_and_reopens() {
    TempSnapshot snapshot_fixture{};
    TempProject project_fixture{"success"};
    const auto* snapshot = snapshot_fixture.get();
    auto* project = project_fixture.get();
    auto registry = registry_for(AdapterMode::valid);
    expect_true("dataset snapshot verifies", snapshot != nullptr);
    expect_true("dataset project creates", project != nullptr);
    expect_true("dataset registry creates", registry != nullptr);
    if (snapshot == nullptr || project == nullptr || registry == nullptr) return;

    const auto request = request_for(*snapshot, "world.land.atomic");
    const auto first = aeris::project::ingest_verified_source_dataset(
        *project, *registry, *snapshot, request);
    expect_true("verified dataset ingest succeeds", first.ok());
    expect_true("verified dataset reports inserted", first.inserted);
    expect_true("verified dataset reports committed", first.durably_committed);
    expect_true("verified dataset is exactly one project revision", project->metadata().revision == 1U);

    const auto sources = aeris::storage::list_source_snapshots(*project);
    const auto geometry = aeris::storage::list_source_geometry_index(*project, request.source_id);
    expect_true("verified dataset provenance exists", sources.ok() && sources.records.size() == 1U);
    expect_true("verified dataset geometry exists", geometry.ok() && geometry.features.size() == 1U);
    if (geometry.ok() && geometry.features.size() == 1U) {
        expect_true("stable feature identity persisted", geometry.features[0].stable_id == "feature:land");
        expect_true("source feature identity persisted", geometry.features[0].source_feature_id == "record:1");
    }

    const auto loaded = aeris::storage::load_feature_geometry(*project, request.source_id, "feature:land");
    expect_true("verified feature geometry loads", loaded.ok());
    if (loaded.ok()) {
        const auto& ring = loaded.feature->rings.front();
        expect_true("source ring role mapped", ring.role == aeris::storage::StoredRingRole::exterior);
        expect_true("source interior side mapped", ring.interior_side == aeris::storage::StoredInteriorSide::right);
        expect_true("source winding mapped", ring.longitude_winding == 0);
    }

    const auto retry = aeris::project::ingest_verified_source_dataset(
        *project, *registry, *snapshot, request);
    expect_true("verified dataset retry succeeds", retry.ok());
    expect_true("verified dataset retry is not inserted", !retry.inserted);
    expect_true("verified dataset retry is not recommitted", !retry.durably_committed);
    expect_true("verified dataset retry keeps one revision", project->metadata().revision == 1U);
    expect_true("verified dataset passes deep project audit", project->verify_integrity().ok());

    const auto path = project->path();
    project_fixture.close();
    auto reopened = aeris::storage::ProjectStore::open(path);
    expect_true("verified dataset project reopens", reopened.ok());
    if (reopened.ok()) {
        expect_true("reopened dataset keeps one revision", reopened.store->metadata().revision == 1U);
        expect_true(
            "reopened dataset keeps geometry",
            aeris::storage::list_source_geometry_index(*reopened.store, request.source_id).features.size() == 1U);
    }
}

void test_registry_valid_geometry_rejection_leaves_no_provenance() {
    TempSnapshot snapshot_fixture{};
    TempProject project_fixture{"rollback"};
    const auto* snapshot = snapshot_fixture.get();
    auto* project = project_fixture.get();
    auto registry = registry_for(AdapterMode::duplicate_source_feature_id);
    if (snapshot == nullptr || project == nullptr || registry == nullptr) {
        ++failures;
        return;
    }

    const auto request = request_for(*snapshot, "world.duplicate-source-id");
    const auto loaded = registry->load(request.binding, *snapshot);
    expect_true("duplicate source-id adapter result passes registry validation", loaded.ok());

    const auto result = aeris::project::ingest_verified_source_dataset(
        *project, *registry, *snapshot, request);
    expect_true("duplicate source-id full ingest is rejected", !result.ok());
    expect_true("duplicate source-id rejection comes from storage boundary",
                result.error == aeris::project::SourceBridgeError::storage_rejected);
    expect_true("duplicate source-id storage error is invalid argument",
                result.storage_error == aeris::storage::StorageError::invalid_argument);
    expect_true("rejected full ingest leaves project revision zero", project->metadata().revision == 0U);
    const auto sources = aeris::storage::list_source_snapshots(*project);
    expect_true("rejected full ingest leaves provenance absent", sources.ok() && sources.records.empty());
    expect_true(
        "rejected full ingest leaves geometry marker absent",
        aeris::storage::list_source_geometry_index(*project, request.source_id).status.error ==
            aeris::storage::StorageError::record_not_found);
}

void test_verified_empty_dataset_is_explicit_and_atomic() {
    TempSnapshot snapshot_fixture{};
    TempProject project_fixture{"empty"};
    const auto* snapshot = snapshot_fixture.get();
    auto* project = project_fixture.get();
    auto registry = registry_for(AdapterMode::empty);
    if (snapshot == nullptr || project == nullptr || registry == nullptr) {
        ++failures;
        return;
    }

    const auto request = request_for(*snapshot, "world.empty.atomic");
    const auto result = aeris::project::ingest_verified_source_dataset(
        *project, *registry, *snapshot, request);
    expect_true("verified empty dataset ingest succeeds", result.ok() && result.durably_committed);
    expect_true("verified empty dataset is one revision", project->metadata().revision == 1U);
    expect_true("verified empty dataset persists provenance",
                aeris::storage::list_source_snapshots(*project).records.size() == 1U);
    const auto geometry = aeris::storage::list_source_geometry_index(*project, request.source_id);
    expect_true("verified empty dataset has explicit empty geometry marker",
                geometry.ok() && geometry.features.empty());
}

void test_provenance_only_history_is_not_reclassified() {
    TempSnapshot snapshot_fixture{};
    TempProject project_fixture{"partial"};
    const auto* snapshot = snapshot_fixture.get();
    auto* project = project_fixture.get();
    auto registry = registry_for(AdapterMode::valid);
    if (snapshot == nullptr || project == nullptr || registry == nullptr) {
        ++failures;
        return;
    }

    const auto request = request_for(*snapshot, "world.partial.history");
    const auto provenance_only = aeris::project::record_verified_source_snapshot(
        *project, *registry, *snapshot, request);
    expect_true("legacy provenance-only bridge fixture succeeds", provenance_only.ok());
    expect_true("provenance-only bridge owns one revision", project->metadata().revision == 1U);

    const auto full = aeris::project::ingest_verified_source_dataset(
        *project, *registry, *snapshot, request);
    expect_true("full ingest rejects provenance-only prehistory", !full.ok());
    expect_true("partial prehistory rejected as immutable existing state",
                full.error == aeris::project::SourceBridgeError::storage_rejected &&
                full.storage_error == aeris::storage::StorageError::record_exists);
    expect_true("partial prehistory rejection keeps existing revision", project->metadata().revision == 1U);
    expect_true(
        "partial prehistory remains provenance-only rather than silently completed",
        aeris::storage::list_source_geometry_index(*project, request.source_id).status.error ==
            aeris::storage::StorageError::record_not_found);
}

}  // namespace

int main() {
    test_verified_dataset_is_one_revision_and_reopens();
    test_registry_valid_geometry_rejection_leaves_no_provenance();
    test_verified_empty_dataset_is_explicit_and_atomic();
    test_provenance_only_history_is_not_reclassified();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "project_dataset_ingest: PASS\n";
    return EXIT_SUCCESS;
}
