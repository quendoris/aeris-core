// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/project/source_bridge.hpp"
#include "aeris/storage/feature_property.hpp"
#include "aeris/util/sha256.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
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

class SnapshotFixture final {
public:
    SnapshotFixture() {
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path() /
                ("aeris-feature-property-bridge-" + std::to_string(stamp));
        std::filesystem::create_directories(root_);
        const auto payload = root_ / "payload.bin";
        {
            std::ofstream output(payload, std::ios::binary | std::ios::trunc);
            output << "verified feature property bridge bytes";
        }

        aeris::source::SnapshotManifest manifest{};
        manifest.provider = "feature-provider";
        manifest.dataset = "admin0";
        manifest.snapshot = "snapshot-1";
        manifest.source_uri = "fixture://feature-property-bridge";
        manifest.retrieved_at_utc = "2026-08-17T11:50:00Z";
        manifest.resources.push_back({
            "payload",
            "payload.bin",
            aeris::util::sha256_file(payload).digest.hex(),
            std::filesystem::file_size(payload),
        });
        auto verified = aeris::source::verify_local_snapshot(root_, manifest);
        if (verified.ok()) snapshot_ = std::move(verified.snapshot);
    }

    ~SnapshotFixture() {
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

class AttributeAdapter final : public aeris::source::Adapter {
public:
    explicit AttributeAdapter(bool complete) : complete_(complete) {}

    [[nodiscard]] aeris::source::AdapterDescriptor descriptor() const noexcept override {
        return {
            complete_ ? "feature.complete.v1" : "feature.geometry-only.v1",
            "feature-provider",
            aeris::source::capability_bit(aeris::source::Capability::admin0),
            aeris::source::TemporalClass::slow_change,
        };
    }

    [[nodiscard]] aeris::source::Result load(
        const aeris::source::VerifiedSnapshot& snapshot,
        const aeris::source::Request& request) const override {
        aeris::source::Result result{};
        result.provenance.provider = "feature-provider";
        result.provenance.dataset = "admin0";
        result.provenance.snapshot = request.snapshot;
        result.provenance.dataset_version = "fixture-v1";
        result.provenance.source_uri = snapshot.manifest().source_uri;
        result.provenance.license_id = "CC0-1.0";
        result.provenance.content_sha256 = snapshot.content_sha256();
        result.provenance.retrieved_at_utc = snapshot.manifest().retrieved_at_utc;
        result.provenance.worldview = request.worldview;
        result.feature_properties_complete = complete_;

        aeris::geometry::LinearRing ring{};
        ring.vertices = {{-0.1, -0.1}, {0.1, -0.1}, {0.1, 0.1}, {-0.1, 0.1}};
        ring.closing_longitude_rad = -0.1;
        ring.longitude_winding = 0;
        ring.interior_side = aeris::geometry::RingInteriorSide::left;

        aeris::source::Feature feature{};
        feature.stable_id = "country:alpha";
        feature.source_id = "record:1";
        feature.rings.push_back({std::move(ring), aeris::source::RingRole::exterior});
        if (complete_) {
            feature.properties.push_back({"name", std::string("Alpha")});
            feature.properties.push_back({"iso_a2", std::string("AA")});
            feature.properties.push_back({"admin_level", static_cast<std::int64_t>(0)});
            feature.properties.push_back({"claimed", false});
        }
        result.features.push_back(std::move(feature));
        return result;
    }

private:
    bool complete_;
};

struct ProjectFixture final {
    std::filesystem::path root;
    std::unique_ptr<aeris::storage::ProjectStore> store;

    explicit ProjectFixture(std::string suffix) {
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() /
               ("aeris-feature-bridge-project-" + suffix + "-" + std::to_string(stamp));
        std::filesystem::create_directories(root);
        aeris::storage::ProjectCreateOptions options{};
        options.timestamp_utc = "2026-08-17T11:50:01Z";
        auto created = aeris::storage::ProjectStore::create(root / "world.aeris", options);
        if (created.ok()) store = std::move(created.store);
    }

    ~ProjectFixture() {
        store.reset();
        std::error_code ignored{};
        std::filesystem::remove_all(root, ignored);
    }
};

aeris::project::VerifiedSourceRecordRequest request_for(
    const aeris::source::VerifiedSnapshot& snapshot,
    std::string source_id,
    std::string adapter_id) {
    aeris::project::VerifiedSourceRecordRequest request{};
    request.source_id = std::move(source_id);
    request.binding.adapter_id = std::move(adapter_id);
    request.binding.capability = aeris::source::Capability::admin0;
    request.binding.snapshot = "snapshot-1";
    request.binding.expected_content_sha256 = snapshot.content_sha256();
    request.modified_utc = "2026-08-17T11:50:02Z";
    return request;
}

}  // namespace

int main() {
    using namespace aeris;

    SnapshotFixture snapshot_fixture{};
    const source::VerifiedSnapshot* snapshot = snapshot_fixture.get();
    expect_true("feature-property bridge snapshot verifies", snapshot != nullptr);
    if (snapshot == nullptr) return EXIT_FAILURE;

    ProjectFixture complete_project{"complete"};
    expect_true("complete bridge project creates", complete_project.store != nullptr);
    if (complete_project.store != nullptr) {
        source::AdapterRegistry registry{};
        expect_true("complete attribute adapter registers",
                    registry.add(std::make_unique<AttributeAdapter>(true)) == source::RegistryError::none);
        const auto request = request_for(
            *snapshot, "world.admin0.complete", "feature.complete.v1");
        const auto bridged = project::record_verified_source_snapshot(
            *complete_project.store, registry, *snapshot, request);
        expect_true("complete attribute dataset bridge succeeds",
                    bridged.ok() && bridged.inserted && bridged.durably_committed);
        expect_true("complete bridge is one project revision",
                    complete_project.store->metadata().revision == 1U);

        const auto index = storage::list_source_feature_properties_index(
            *complete_project.store, "world.admin0.complete");
        expect_true("complete bridge writes feature property marker",
                    index.ok() && index.features.size() == 1U &&
                    index.features.front().property_count == 4U);
        const auto properties = storage::load_feature_properties(
            *complete_project.store, "world.admin0.complete", "country:alpha");
        expect_true("bridged feature properties load",
                    properties.ok() && properties.properties.size() == 4U);
        if (properties.ok() && properties.properties.size() == 4U) {
            expect_true("bridged properties sort canonically",
                        properties.properties[0].key == "admin_level" &&
                        properties.properties[1].key == "claimed" &&
                        properties.properties[2].key == "iso_a2" &&
                        properties.properties[3].key == "name");
            expect_true("bridged ISO text survives",
                        std::get<std::string>(properties.properties[2].value) == "AA");
        }
        expect_true("complete bridged project passes integrity",
                    complete_project.store->verify_integrity().ok());
    }

    ProjectFixture geometry_project{"geometry"};
    expect_true("geometry-only bridge project creates", geometry_project.store != nullptr);
    if (geometry_project.store != nullptr) {
        source::AdapterRegistry registry{};
        expect_true("geometry-only adapter registers",
                    registry.add(std::make_unique<AttributeAdapter>(false)) == source::RegistryError::none);
        const auto request = request_for(
            *snapshot, "world.admin0.geometry-only", "feature.geometry-only.v1");
        const auto bridged = project::record_verified_source_snapshot(
            *geometry_project.store, registry, *snapshot, request);
        expect_true("geometry-only dataset bridge succeeds", bridged.ok());
        const auto properties = storage::list_source_feature_properties_index(
            *geometry_project.store, "world.admin0.geometry-only");
        expect_true("geometry-only bridge leaves property channel absent",
                    properties.status.error == storage::StorageError::record_not_found);
        expect_true("geometry-only project remains valid",
                    geometry_project.store->verify_integrity().ok());
    }

    if (failures != 0) {
        std::cerr << failures << " feature property bridge assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "project_feature_property_bridge: PASS\n";
    return EXIT_SUCCESS;
}
