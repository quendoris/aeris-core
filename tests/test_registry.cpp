// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/source/registry.hpp"
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
        root_ = std::filesystem::temp_directory_path() / ("aeris-registry-" + std::to_string(stamp));
        std::filesystem::create_directories(root_);
        const auto path = root_ / "data.bin";
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << "snapshot";
        }
        aeris::source::SnapshotManifest manifest{};
        manifest.provider = "registry-provider";
        manifest.dataset = "registry-dataset";
        manifest.snapshot = "snapshot-1";
        manifest.source_uri = "fixture://registry";
        manifest.retrieved_at_utc = "2026-08-12T00:00:00Z";
        manifest.resources.push_back({
            "data", "data.bin", aeris::util::sha256_file(path).digest.hex(), std::filesystem::file_size(path)
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

class RegistryAdapter final : public aeris::source::Adapter {
public:
    [[nodiscard]] aeris::source::AdapterDescriptor descriptor() const noexcept override {
        return {"registry.adapter.v1", "registry-provider", aeris::source::capability_bit(aeris::source::Capability::land), aeris::source::TemporalClass::slow_change};
    }

    [[nodiscard]] aeris::source::Result load(
        const aeris::source::VerifiedSnapshot& snapshot,
        const aeris::source::Request& request
    ) const override {
        aeris::source::Result result{};
        result.provenance.provider = "registry-provider";
        result.provenance.dataset = "registry-dataset";
        result.provenance.snapshot = request.snapshot;
        result.provenance.source_uri = snapshot.manifest().source_uri;
        result.provenance.license_id = "CC0-1.0";
        result.provenance.content_sha256 = snapshot.content_sha256();
        result.provenance.retrieved_at_utc = snapshot.manifest().retrieved_at_utc;
        aeris::source::Feature feature{};
        feature.stable_id = "feature";
        feature.source_id = "source-feature";
        feature.rings.push_back({aeris::geometry::LinearRing{{{-0.1, -0.1}, {0.1, -0.1}, {0.1, 0.1}, {-0.1, 0.1}}}, aeris::source::RingRole::exterior});
        result.features.push_back(std::move(feature));
        return result;
    }
};

void test_registry_load_and_pin() {
    TempSnapshot fixture{};
    const auto* snapshot = fixture.get();
    expect_true("registry fixture verifies", snapshot != nullptr);
    if (snapshot == nullptr) {
        return;
    }

    aeris::source::AdapterRegistry registry{};
    expect_true("adapter registration succeeds", registry.add(std::make_unique<RegistryAdapter>()) == aeris::source::RegistryError::none);
    expect_true("duplicate adapter id rejected", registry.add(std::make_unique<RegistryAdapter>()) == aeris::source::RegistryError::duplicate_adapter_id);

    aeris::source::SourceBinding binding{};
    binding.adapter_id = "registry.adapter.v1";
    binding.capability = aeris::source::Capability::land;
    binding.snapshot = "snapshot-1";
    binding.expected_content_sha256 = snapshot->content_sha256();

    const auto loaded = registry.load(binding, *snapshot);
    expect_true("pinned binding loads", loaded.ok());
    if (loaded.ok()) {
        expect_true("loaded provenance preserves content identity", loaded.source.provenance.content_sha256 == snapshot->content_sha256());
    }

    binding.expected_content_sha256 = std::string(64U, '0');
    const auto mismatch = registry.load(binding, *snapshot);
    expect_true("wrong pinned content rejected", mismatch.error == aeris::source::RegistryError::snapshot_content_mismatch);
}

void test_missing_adapter_rejected() {
    TempSnapshot fixture{};
    const auto* snapshot = fixture.get();
    if (snapshot == nullptr) {
        ++failures;
        return;
    }
    aeris::source::AdapterRegistry registry{};
    aeris::source::SourceBinding binding{};
    binding.adapter_id = "missing.adapter";
    binding.capability = aeris::source::Capability::land;
    binding.snapshot = "snapshot-1";
    const auto result = registry.load(binding, *snapshot);
    expect_true("missing adapter rejected", result.error == aeris::source::RegistryError::adapter_not_found);
}

}  // namespace

int main() {
    test_registry_load_and_pin();
    test_missing_adapter_rejected();
    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "source_registry: PASS\n";
    return EXIT_SUCCESS;
}
