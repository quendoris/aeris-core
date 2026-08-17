// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#include "aeris/storage/project.hpp"
#include "aeris/storage/provenance.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

int failures = 0;

void expect(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void expect_error(const aeris::storage::Status& status, const aeris::storage::StorageError expected, const char* message) {
    if (status.error != expected) {
        std::cerr << "FAIL: " << message << " expected=" << static_cast<int>(expected)
                  << " actual=" << static_cast<int>(status.error)
                  << " diagnostic=" << status.diagnostic << '\n';
        ++failures;
    }
}

std::string read_binary(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

aeris::storage::SourceSnapshotRecord natural_earth_record() {
    aeris::storage::SourceSnapshotRecord record;
    record.source_id = "source:land:baseline";
    record.adapter_id = "natural-earth.ne-110m-land.shapefile.v1";
    record.capability_bits = 1U;
    record.temporal_class = 1U;
    record.provider = "Natural Earth";
    record.dataset = "ne_110m_land";
    record.snapshot = "v5.1.2";
    record.dataset_version = "4.1.0";
    record.source_uri = "https://github.com/nvkelso/natural-earth-vector/releases/tag/v5.1.2";
    record.license_id = "LicenseRef-Natural-Earth-Public-Domain";
    record.content_sha256 = "1111111111111111111111111111111111111111111111111111111111111111";
    record.retrieved_at_utc = "2026-08-16T19:45:00Z";
    record.worldview = "";
    record.resources = {
        {"dataset.version", "3333333333333333333333333333333333333333333333333333333333333333", 6U},
        {"geometry.shp", "2222222222222222222222222222222222222222222222222222222222222222", 89504U},
        {"crs.prj", "4444444444444444444444444444444444444444444444444444444444444444", 147U},
    };
    return record;
}

}  // namespace

int main() {
    using namespace aeris::storage;

    expect(is_canonical_sha256("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"),
           "lowercase SHA-256 syntax should be canonical");
    expect(!is_canonical_sha256("0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef"),
           "uppercase SHA-256 syntax should be rejected as noncanonical");
    expect(!is_canonical_sha256("abc"), "short SHA-256 syntax should be rejected");

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "aeris-provenance-v0-test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    if (ec) return 2;
    const std::filesystem::path project_path = root / "world.aeris";

    ProjectCreateOptions options;
    options.timestamp_utc = "2026-08-16T19:44:59Z";
    options.project_uuid = "11111111-2222-4333-8444-555555555555";
    auto project = ProjectStore::create(project_path, options);
    expect(project.ok(), "current draft project should create");
    if (!project.ok()) {
        std::cerr << project.status.diagnostic << '\n';
        return 1;
    }
    expect(project.store->metadata().format_major == kDraftFormatMajor &&
               project.store->metadata().format_minor == kDraftFormatMinor,
           "project metadata should advertise current draft format constants");
    expect(project.store->metadata().revision == 0U, "empty project revision should start at zero");

    auto empty = list_source_snapshots(*project.store);
    expect(empty.ok() && empty.records.empty(), "new project source catalog should be empty");

    SourceSnapshotRecord record = natural_earth_record();
    auto inserted = store_source_snapshot(*project.store, record, "2026-08-16T19:45:01Z");
    expect(inserted.ok() && inserted.inserted && inserted.durably_committed,
           "verified source snapshot record should commit atomically");
    expect(project.store->metadata().revision == 1U, "one source snapshot mutation should advance project revision once");
    expect(project.store->metadata().modified_utc == "2026-08-16T19:45:01Z",
           "source snapshot mutation should advance project modification timestamp");

    const std::string before_read = read_binary(project_path);
    auto listed = list_source_snapshots(*project.store);
    const std::string after_read = read_binary(project_path);
    expect(listed.ok() && listed.records.size() == 1U, "stored source snapshot should enumerate");
    expect(before_read == after_read, "read-only source enumeration must not change project bytes");
    if (listed.ok() && listed.records.size() == 1U) {
        const auto& got = listed.records.front();
        expect(got.source_id == record.source_id && got.adapter_id == record.adapter_id,
               "stored source identity should round-trip");
        expect(got.provider == record.provider && got.dataset == record.dataset && got.snapshot == record.snapshot,
               "stored provenance identity should round-trip");
        expect(got.content_sha256 == record.content_sha256 && got.retrieved_at_utc == record.retrieved_at_utc,
               "stored content identity and acquisition time should round-trip");
        expect(got.resources.size() == 3U, "all source manifest resources should round-trip");
        if (got.resources.size() == 3U) {
            expect(got.resources[0].logical_name == "crs.prj" && got.resources[1].logical_name == "dataset.version" &&
                       got.resources[2].logical_name == "geometry.shp",
                   "resource enumeration should be deterministic by logical name");
        }
    }

    const std::uint64_t revision_after_insert = project.store->metadata().revision;
    auto retry = store_source_snapshot(*project.store, record, "2026-08-16T19:45:02Z");
    expect(retry.ok() && !retry.inserted && !retry.durably_committed,
           "exact source snapshot retry should be idempotent");
    expect(project.store->metadata().revision == revision_after_insert,
           "idempotent source retry must not advance project revision");
    expect(project.store->metadata().modified_utc == "2026-08-16T19:45:01Z",
           "idempotent source retry must not advance project timestamp");

    SourceSnapshotRecord conflict = record;
    conflict.dataset_version = "different-version";
    auto conflicting = store_source_snapshot(*project.store, conflict, "2026-08-16T19:45:03Z");
    expect_error(conflicting.status, StorageError::record_exists,
                 "same source_id with different immutable provenance must fail closed");
    expect(project.store->metadata().revision == revision_after_insert,
           "conflicting source ID must not advance project revision");

    SourceSnapshotRecord bad_hash = record;
    bad_hash.source_id = "source:bad-hash";
    bad_hash.content_sha256[0] = 'A';
    auto rejected_hash = store_source_snapshot(*project.store, bad_hash, "2026-08-16T19:45:04Z");
    expect_error(rejected_hash.status, StorageError::invalid_argument,
                 "noncanonical source content hash should fail at API boundary");

    SourceSnapshotRecord duplicate_resource = record;
    duplicate_resource.source_id = "source:duplicate-resource";
    duplicate_resource.resources.push_back(duplicate_resource.resources.front());
    auto rejected_duplicate = store_source_snapshot(*project.store, duplicate_resource, "2026-08-16T19:45:05Z");
    expect_error(rejected_duplicate.status, StorageError::invalid_argument,
                 "duplicate resource logical names should fail at API boundary");

    SourceSnapshotRecord second = record;
    second.source_id = "source:admin0:snapshot";
    second.adapter_id = "example.admin0.v1";
    second.capability_bits = 2U;
    second.temporal_class = 2U;
    second.provider = "Example Provider";
    second.dataset = "admin0";
    second.snapshot = "2026-08-16";
    second.dataset_version = "2026.08";
    second.source_uri = "https://example.invalid/datasets/admin0/2026-08-16";
    second.license_id = "CC-BY-4.0";
    second.content_sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    second.retrieved_at_utc = "2026-08-16T19:45:06Z";
    second.worldview = "neutral-disputed";
    second.resources = {{"boundaries", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", std::nullopt}};
    auto second_insert = store_source_snapshot(*project.store, second, "2026-08-16T19:45:07Z");
    expect(second_insert.ok() && second_insert.inserted && second_insert.durably_committed,
           "second independent source snapshot should commit");
    expect(project.store->metadata().revision == 2U, "two inserted source snapshots should yield revision two");

    project.store.reset();
    auto reopened = ProjectStore::open(project_path);
    expect(reopened.ok(), "project with provenance catalog should reopen");
    if (!reopened.ok()) {
        std::cerr << reopened.status.diagnostic << '\n';
        return 1;
    }
    auto after_reopen = list_source_snapshots(*reopened.store);
    expect(after_reopen.ok() && after_reopen.records.size() == 2U,
           "all committed source snapshots should survive close/reopen");
    if (after_reopen.ok() && after_reopen.records.size() == 2U) {
        expect(after_reopen.records[0].source_id == "source:admin0:snapshot" &&
                   after_reopen.records[1].source_id == "source:land:baseline",
               "source enumeration should be deterministic by project source ID");
        expect(!after_reopen.records[0].resources[0].size_bytes.has_value(),
               "unknown verified resource byte length should round-trip as NULL");
    }
    expect(reopened.store->verify_integrity().ok(), "project with provenance foreign keys should pass integrity verification");

    std::filesystem::remove_all(root, ec);
    return failures == 0 ? 0 : 1;
}
