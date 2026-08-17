// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

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
        root_ = std::filesystem::temp_directory_path() / ("aeris-project-bridge-" + std::to_string(stamp));
        std::filesystem::create_directories(root_ / "meta");

        const auto geometry_path = root_ / "data.bin";
        const auto metadata_path = root_ / "meta" / "info.txt";
        {
            std::ofstream output(geometry_path, std::ios::binary | std::ios::trunc);
            output << "verified geometry bytes";
        }
        {
            std::ofstream output(metadata_path, std::ios::binary | std::ios::trunc);
            output << "dataset metadata";
        }

        aeris::source::SnapshotManifest manifest{};
        manifest.provider = "bridge-provider";
        manifest.dataset = "bridge-dataset";
        manifest.snapshot = "snapshot-1";
        manifest.source_uri = "fixture://bridge";
        manifest.retrieved_at_utc = "2026-08-16T20:00:00Z";
        manifest.resources.push_back({
            "geometry",
            "data.bin",
            aeris::util::sha256_file(geometry_path).digest.hex(),
            std::filesystem::file_size(geometry_path)
        });
        manifest.resources.push_back({
            "metadata",
            "meta/info.txt",
            aeris::util::sha256_file(metadata_path).digest.hex(),
            std::nullopt
        });

        auto verified = aeris::source::verify_local_snapshot(root_, manifest);
        if (verified.ok()) {
            snapshot_ = std::move(verified.snapshot);
        }
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
    wrong_dataset,
};

class BridgeAdapter final : public aeris::source::Adapter {
public:
    explicit BridgeAdapter(const AdapterMode mode = AdapterMode::valid) : mode_(mode) {}

    [[nodiscard]] aeris::source::AdapterDescriptor descriptor() const noexcept override {
        return {
            mode_ == AdapterMode::valid ? "bridge.adapter.v1" : "bridge.bad-dataset.v1",
            "bridge-provider",
            aeris::source::capability_bit(aeris::source::Capability::land),
            aeris::source::TemporalClass::slow_change,
        };
    }

    [[nodiscard]] aeris::source::Result load(
        const aeris::source::VerifiedSnapshot& snapshot,
        const aeris::source::Request& request
    ) const override {
        aeris::source::Result result{};
        result.provenance.provider = "bridge-provider";
        result.provenance.dataset = mode_ == AdapterMode::valid ? "bridge-dataset" : "other-dataset";
        result.provenance.snapshot = request.snapshot;
        result.provenance.dataset_version = "fixture-v1";
        result.provenance.source_uri = snapshot.manifest().source_uri;
        result.provenance.license_id = "CC0-1.0";
        result.provenance.content_sha256 = snapshot.content_sha256();
        result.provenance.retrieved_at_utc = snapshot.manifest().retrieved_at_utc;
        result.provenance.worldview = request.worldview;

        aeris::geometry::LinearRing ring{};
        ring.vertices = {{-0.1, -0.1}, {0.1, -0.1}, {0.1, 0.1}, {-0.1, 0.1}};
        ring.closing_longitude_rad = -0.1;
        ring.longitude_winding = 0;
        ring.interior_side = aeris::geometry::RingInteriorSide::left;

        aeris::source::Feature feature{};
        feature.stable_id = "bridge-feature";
        feature.source_id = "fixture-record";
        feature.rings.push_back({std::move(ring), aeris::source::RingRole::exterior});
        result.features.push_back(std::move(feature));
        return result;
    }

private:
    AdapterMode mode_;
};

class TempProject final {
public:
    TempProject() {
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path() / ("aeris-project-bridge-db-" + std::to_string(stamp));
        std::filesystem::create_directories(root_);

        aeris::storage::ProjectCreateOptions options{};
        options.timestamp_utc = "2026-08-16T20:01:00Z";
        auto created = aeris::storage::ProjectStore::create(root_ / "world.aeris", options);
        if (created.ok()) {
            project_ = std::move(created.store);
        }
    }

    ~TempProject() {
        project_.reset();
        std::error_code ignored{};
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] aeris::storage::ProjectStore* get() noexcept { return project_.get(); }
    void close() noexcept { project_.reset(); }

private:
    std::filesystem::path root_;
    std::unique_ptr<aeris::storage::ProjectStore> project_;
};

[[nodiscard]] aeris::project::VerifiedSourceRecordRequest valid_request(
    const aeris::source::VerifiedSnapshot& snapshot,
    std::string adapter_id = "bridge.adapter.v1"
) {
    aeris::project::VerifiedSourceRecordRequest request{};
    request.source_id = "world.land.primary";
    request.binding.adapter_id = std::move(adapter_id);
    request.binding.capability = aeris::source::Capability::land;
    request.binding.snapshot = "snapshot-1";
    request.binding.expected_content_sha256 = snapshot.content_sha256();
    request.modified_utc = "2026-08-16T20:02:00Z";
    return request;
}

void expect_geometry_round_trip(
    aeris::storage::ProjectStore& project,
    const std::string_view prefix) {
    const auto index = aeris::storage::list_source_geometry_index(project, "world.land.primary");
    expect_true(std::string(prefix) + " geometry index lists", index.ok());
    expect_true(std::string(prefix) + " geometry feature count", index.features.size() == 1U);
    if (!index.ok() || index.features.size() != 1U) return;

    const auto& entry = index.features.front();
    expect_true(std::string(prefix) + " stable feature id", entry.stable_id == "bridge-feature");
    expect_true(std::string(prefix) + " source feature id", entry.source_feature_id == "fixture-record");
    expect_true(std::string(prefix) + " ring count", entry.ring_count == 1U);

    const auto feature = aeris::storage::load_feature_geometry(
        project, "world.land.primary", "bridge-feature");
    expect_true(std::string(prefix) + " geometry feature loads", feature.ok());
    if (!feature.ok()) return;
    expect_true(std::string(prefix) + " loaded ring count", feature.feature->rings.size() == 1U);
    if (feature.feature->rings.size() == 1U) {
        const auto& ring = feature.feature->rings.front();
        expect_true(std::string(prefix) + " loaded vertex count", ring.vertices.size() == 4U);
        expect_true(
            std::string(prefix) + " loaded topology side",
            ring.interior_side == aeris::storage::StoredInteriorSide::left
        );
        expect_true(
            std::string(prefix) + " loaded closing longitude",
            ring.closing_longitude_rad == -0.1
        );
    }
}

void test_verified_snapshot_persists_and_retries_idempotently() {
    TempSnapshot snapshot_fixture{};
    TempProject project_fixture{};
    const auto* snapshot = snapshot_fixture.get();
    auto* project = project_fixture.get();
    expect_true("bridge snapshot verifies", snapshot != nullptr);
    expect_true("bridge project creates", project != nullptr);
    if (snapshot == nullptr || project == nullptr) return;

    aeris::source::AdapterRegistry registry{};
    expect_true(
        "bridge adapter registers",
        registry.add(std::make_unique<BridgeAdapter>()) == aeris::source::RegistryError::none
    );

    const auto request = valid_request(*snapshot);
    const auto first = aeris::project::record_verified_source_snapshot(*project, registry, *snapshot, request);
    expect_true("verified source dataset bridge succeeds", first.ok());
    expect_true("verified source dataset inserted", first.inserted);
    expect_true("verified source dataset committed", first.durably_committed);
    expect_true("full dataset advances exactly one project revision", project->metadata().revision == 1U);

    const auto listed = aeris::storage::list_source_snapshots(*project);
    expect_true("stored source lists", listed.ok());
    expect_true("stored source count", listed.records.size() == 1U);
    if (listed.ok() && listed.records.size() == 1U) {
        const auto& record = listed.records.front();
        expect_true("source id preserved", record.source_id == "world.land.primary");
        expect_true("adapter id preserved", record.adapter_id == "bridge.adapter.v1");
        expect_true(
            "selected capability persisted",
            record.capability_bits == aeris::source::capability_bit(aeris::source::Capability::land)
        );
        expect_true(
            "temporal class persisted",
            record.temporal_class == static_cast<std::uint8_t>(aeris::source::TemporalClass::slow_change)
        );
        expect_true("provider preserved", record.provider == snapshot->manifest().provider);
        expect_true("dataset preserved", record.dataset == snapshot->manifest().dataset);
        expect_true("snapshot preserved", record.snapshot == snapshot->manifest().snapshot);
        expect_true("aggregate content hash preserved", record.content_sha256 == snapshot->content_sha256());
        expect_true("resource count preserved", record.resources.size() == 2U);
        if (record.resources.size() == 2U) {
            expect_true("resource order deterministic", record.resources[0].logical_name == "geometry");
            expect_true("manifest size preserved when supplied", record.resources[0].size_bytes.has_value());
            expect_true("resource order deterministic second", record.resources[1].logical_name == "metadata");
            expect_true("omitted manifest size remains omitted", !record.resources[1].size_bytes.has_value());
        }
    }
    expect_geometry_round_trip(*project, "stored");

    const auto retry = aeris::project::record_verified_source_snapshot(*project, registry, *snapshot, request);
    expect_true("exact verified dataset retry succeeds", retry.ok());
    expect_true("exact verified dataset retry does not reinsert", !retry.inserted);
    expect_true("exact verified dataset retry does not recommit", !retry.durably_committed);
    expect_true("exact verified dataset retry keeps revision", project->metadata().revision == 1U);

    const auto project_path = project->path();
    project_fixture.close();
    project = nullptr;
    auto reopened = aeris::storage::ProjectStore::open(project_path);
    expect_true("bridge project reopens after original handle closes", reopened.ok());
    if (reopened.ok()) {
        const auto reopened_list = aeris::storage::list_source_snapshots(*reopened.store);
        expect_true("reopened provenance lists", reopened_list.ok() && reopened_list.records.size() == 1U);
        expect_geometry_round_trip(*reopened.store, "reopened");
    }
}

void test_registry_rejection_is_fail_closed() {
    TempSnapshot snapshot_fixture{};
    TempProject project_fixture{};
    const auto* snapshot = snapshot_fixture.get();
    auto* project = project_fixture.get();
    if (snapshot == nullptr || project == nullptr) {
        ++failures;
        return;
    }

    aeris::source::AdapterRegistry registry{};
    expect_true(
        "registry rejection adapter registers",
        registry.add(std::make_unique<BridgeAdapter>()) == aeris::source::RegistryError::none
    );

    auto request = valid_request(*snapshot);
    request.binding.expected_content_sha256 = std::string(64U, '0');
    const auto result = aeris::project::record_verified_source_snapshot(*project, registry, *snapshot, request);
    expect_true("wrong pinned hash rejected by bridge", result.error == aeris::project::SourceBridgeError::registry_rejected);
    expect_true("registry error preserved", result.registry_error == aeris::source::RegistryError::snapshot_content_mismatch);
    expect_true("registry rejection leaves revision zero", project->metadata().revision == 0U);
    const auto listed = aeris::storage::list_source_snapshots(*project);
    expect_true("registry rejection stores nothing", listed.ok() && listed.records.empty());
    const auto geometry = aeris::storage::list_source_geometry_index(*project, "world.land.primary");
    expect_true(
        "registry rejection stores no geometry marker",
        geometry.status.error == aeris::storage::StorageError::record_not_found
    );
}

void test_manifest_cross_check_rejects_adapter_drift() {
    TempSnapshot snapshot_fixture{};
    TempProject project_fixture{};
    const auto* snapshot = snapshot_fixture.get();
    auto* project = project_fixture.get();
    if (snapshot == nullptr || project == nullptr) {
        ++failures;
        return;
    }

    aeris::source::AdapterRegistry registry{};
    expect_true(
        "drift adapter registers",
        registry.add(std::make_unique<BridgeAdapter>(AdapterMode::wrong_dataset)) == aeris::source::RegistryError::none
    );

    auto request = valid_request(*snapshot, "bridge.bad-dataset.v1");
    const auto drift = aeris::project::record_verified_source_snapshot(*project, registry, *snapshot, request);
    expect_true(
        "adapter dataset drift rejected after registry validation",
        drift.error == aeris::project::SourceBridgeError::verified_snapshot_mismatch
    );
    expect_true("adapter drift leaves revision zero", project->metadata().revision == 0U);

    aeris::source::AdapterRegistry normal_registry{};
    expect_true(
        "normal adapter registers for binding drift",
        normal_registry.add(std::make_unique<BridgeAdapter>()) == aeris::source::RegistryError::none
    );
    request = valid_request(*snapshot);
    request.binding.snapshot = "different-snapshot-label";
    const auto binding_drift = aeris::project::record_verified_source_snapshot(
        *project, normal_registry, *snapshot, request
    );
    expect_true(
        "binding snapshot drift from verified manifest rejected",
        binding_drift.error == aeris::project::SourceBridgeError::verified_snapshot_mismatch
    );
    expect_true("binding drift stores nothing", aeris::storage::list_source_snapshots(*project).records.empty());
}

}  // namespace

int main() {
    test_verified_snapshot_persists_and_retries_idempotently();
    test_registry_rejection_is_fail_closed();
    test_manifest_cross_check_rejects_adapter_drift();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "project_source_bridge: PASS\n";
    return EXIT_SUCCESS;
}
