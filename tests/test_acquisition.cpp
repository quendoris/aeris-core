// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/source/acquisition.hpp"
#include "aeris/util/sha256.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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

class TempDirectory final {
public:
    TempDirectory() {
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() / ("aeris-acquisition-" + std::to_string(stamp));
        std::filesystem::create_directories(path_ / "nested");
        write("geometry.bin", "geometry-bytes");
        write("nested/meta.txt", "metadata\n");
    }

    ~TempDirectory() {
        std::error_code ignored{};
        std::filesystem::remove_all(path_, ignored);
    }

    void write(const std::filesystem::path& relative, const std::string& content) const {
        std::ofstream output(path_ / relative, std::ios::binary | std::ios::trunc);
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    [[nodiscard]] std::string hash(const std::filesystem::path& relative) const {
        return aeris::util::sha256_file(path_ / relative).digest.hex();
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

aeris::source::SnapshotManifest valid_manifest(const TempDirectory& directory) {
    aeris::source::SnapshotManifest manifest{};
    manifest.provider = "fixture-provider";
    manifest.dataset = "fixture-dataset";
    manifest.snapshot = "2026-08-12";
    manifest.source_uri = "fixture://dataset/2026-08-12";
    manifest.retrieved_at_utc = "2026-08-12T14:00:00Z";
    manifest.resources.push_back({"geometry", "geometry.bin", directory.hash("geometry.bin"), 14U, ""});
    manifest.resources.push_back({"metadata", "nested/meta.txt", directory.hash("nested/meta.txt"), 9U, ""});
    return manifest;
}

void test_verified_snapshot_and_aggregate_identity() {
    const TempDirectory directory{};
    const auto manifest = valid_manifest(directory);
    const auto first = aeris::source::verify_local_snapshot(directory.path(), manifest);
    const auto second = aeris::source::verify_local_snapshot(directory.path(), manifest);
    expect_true("valid snapshot verifies", first.ok());
    expect_true("repeat verification succeeds", second.ok());
    if (!first.ok() || !second.ok()) {
        return;
    }

    expect_true(
        "aggregate content identity is deterministic",
        first.snapshot->content_sha256() == second.snapshot->content_sha256()
    );
    expect_true("aggregate SHA-256 is canonical hex", first.snapshot->content_sha256().size() == 64U);
    const auto geometry = first.snapshot->resource_path("geometry");
    expect_true("logical resource resolves", geometry.has_value());
    if (geometry.has_value()) {
        expect_true("resolved resource is inside snapshot", geometry->filename() == "geometry.bin");
    }
}

void test_hash_mismatch_rejected() {
    const TempDirectory directory{};
    auto manifest = valid_manifest(directory);
    manifest.resources.front().sha256 = std::string(64U, '0');
    const auto result = aeris::source::verify_local_snapshot(directory.path(), manifest);
    expect_true("resource hash mismatch rejected", result.error == aeris::source::AcquisitionError::hash_mismatch);
}

void test_size_mismatch_rejected() {
    const TempDirectory directory{};
    auto manifest = valid_manifest(directory);
    manifest.resources.front().size_bytes = 1U;
    const auto result = aeris::source::verify_local_snapshot(directory.path(), manifest);
    expect_true("resource size mismatch rejected", result.error == aeris::source::AcquisitionError::size_mismatch);
}

void test_traversal_rejected() {
    const TempDirectory directory{};
    auto manifest = valid_manifest(directory);
    manifest.resources.front().relative_path = "../escape.bin";
    const auto result = aeris::source::verify_local_snapshot(directory.path(), manifest);
    expect_true("path traversal rejected", result.error == aeris::source::AcquisitionError::unsafe_resource_path);
}

void test_duplicate_logical_name_rejected() {
    const TempDirectory directory{};
    auto manifest = valid_manifest(directory);
    manifest.resources[1].logical_name = manifest.resources[0].logical_name;
    const auto result = aeris::source::verify_local_snapshot(directory.path(), manifest);
    expect_true("duplicate logical name rejected", result.error == aeris::source::AcquisitionError::duplicate_resource);
}

void test_retrieval_metadata_does_not_change_content_identity() {
    const TempDirectory directory{};
    auto first_manifest = valid_manifest(directory);
    auto second_manifest = first_manifest;
    second_manifest.retrieved_at_utc = "2030-01-01T00:00:00Z";
    second_manifest.source_uri = "mirror://same-bytes";

    const auto first = aeris::source::verify_local_snapshot(directory.path(), first_manifest);
    const auto second = aeris::source::verify_local_snapshot(directory.path(), second_manifest);
    expect_true("first content snapshot verifies", first.ok());
    expect_true("second content snapshot verifies", second.ok());
    if (first.ok() && second.ok()) {
        expect_true(
            "transport metadata does not change content hash",
            first.snapshot->content_sha256() == second.snapshot->content_sha256()
        );
    }
}

}  // namespace

int main() {
    test_verified_snapshot_and_aggregate_identity();
    test_hash_mismatch_rejected();
    test_size_mismatch_rejected();
    test_traversal_rejected();
    test_duplicate_logical_name_rejected();
    test_retrieval_metadata_does_not_change_content_identity();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "source_acquisition: PASS\n";
    return EXIT_SUCCESS;
}
