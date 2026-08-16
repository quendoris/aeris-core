// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#include "aeris/storage/project.hpp"
#include "aeris/storage/session.hpp"

#include <cmath>
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
        std::cerr << "FAIL: " << message << " expected_error=" << static_cast<int>(expected)
                  << " actual_error=" << static_cast<int>(status.error)
                  << " diagnostic=" << status.diagnostic << '\n';
        ++failures;
    }
}

bool has_staging_directory(const std::filesystem::path& root, const std::string& prefix) {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().filename().string().rfind(prefix, 0U) == 0U) return true;
    }
    return false;
}

std::string read_binary(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

}  // namespace

int main() {
    using namespace aeris::storage;

    expect(is_canonical_utc_timestamp("2000-02-29T23:59:59Z"), "Gregorian leap day in year 2000 should be valid");
    expect(!is_canonical_utc_timestamp("2100-02-29T00:00:00Z"), "Gregorian century year 2100 should not be leap");
    expect(!is_canonical_utc_timestamp("2026-02-29T00:00:00Z"), "non-leap February 29 should be rejected");
    expect(!is_canonical_utc_timestamp("2026-04-31T00:00:00Z"), "April 31 should be rejected");
    expect(!is_canonical_utc_timestamp("2026-13-01T00:00:00Z"), "month 13 should be rejected");

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "aeris-storage-v0-contract-test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    expect(!ec, "temporary storage test directory should be creatable");

    const std::filesystem::path project_path = root / "world.aeris";
    const std::string uuid = "01234567-89ab-4cde-8f01-23456789abcd";

    ProjectCreateOptions create_options;
    create_options.timestamp_utc = "2026-08-16T18:10:00Z";
    create_options.project_uuid = uuid;
    create_options.producer = "aeris-test";
    create_options.producer_version = "0.1.0-test";

    auto created = ProjectStore::create(project_path, create_options);
    expect(created.ok(), "project creation should succeed through atomic no-overwrite publication");
    if (!created.ok()) {
        std::cerr << created.status.diagnostic << '\n';
        return 1;
    }
    expect(std::filesystem::exists(project_path), "project file should exist immediately after acknowledged creation");
    expect(!has_staging_directory(root, ".aeris-create-"), "successful project create must remove sibling staging state");
    expect(created.store->metadata().project_uuid == uuid, "project UUID should match explicit create identity");
    expect(created.store->metadata().revision == 0U, "new project revision should be zero");
    expect(created.store->metadata().projection_id == "aeris.projection.unspecified", "new project projection should be explicit unspecified value");
    expect(created.store->verify_integrity().ok(), "new project should pass integrity verification");

    auto duplicate = ProjectStore::create(project_path, create_options);
    expect_error(duplicate.status, StorageError::path_exists, "project create must refuse overwrite at atomic publish step");
    expect(!has_staging_directory(root, ".aeris-create-"), "failed no-overwrite publication must clean sibling staging state");
    expect(created.store->metadata().revision == 0U, "failed duplicate create must not mutate the already-open project");

    ProjectMetadataUpdate update;
    update.modified_utc = "2026-08-16T18:10:01Z";
    update.projection_id = "aeris.projection.mollweide.v1";
    update.worldview_id = "neutral-disputed";
    update.frozen = true;
    Status status = created.store->update_metadata(update);
    expect(status.ok(), "acknowledged project mutation should commit");
    expect(created.store->metadata().revision == 1U, "acknowledged project mutation should increment revision exactly once");
    expect(created.store->metadata().frozen, "project mutation should update frozen marker");

    created.store.reset();
    auto reopened = ProjectStore::open(project_path);
    expect(reopened.ok(), "project should reopen after close");
    if (!reopened.ok()) {
        std::cerr << reopened.status.diagnostic << '\n';
        return 1;
    }
    expect(reopened.store->metadata().revision == 1U, "committed project revision should survive reopen");
    expect(reopened.store->metadata().projection_id == "aeris.projection.mollweide.v1", "committed projection should survive reopen");
    expect(reopened.store->metadata().worldview_id == "neutral-disputed", "committed worldview should survive reopen");
    expect(reopened.store->metadata().frozen, "committed frozen flag should survive reopen");

    auto session = SessionStore::open_or_create(*reopened.store, "2026-08-16T18:10:02Z");
    expect(session.ok(), "adjacent session should create from the validated project identity");
    if (!session.ok()) {
        std::cerr << session.status.diagnostic << '\n';
        return 1;
    }
    expect(session.store->path() == adjacent_session_path(project_path), "session path must be directly adjacent as .aeris.session");
    expect(std::filesystem::exists(session.store->path()), "session sidecar should be durably visible after creation");
    expect(!has_staging_directory(root, ".aeris-session-create-"), "successful session create must remove sibling staging state");
    auto empty_view = session.store->read_view_state();
    expect(empty_view.ok() && !empty_view.value.has_value(), "new session should not invent a view state");

    SessionViewState view;
    view.mode = SessionViewState::Mode::globe;
    view.camera_longitude_rad = -1.205;
    view.camera_latitude_rad = 1.489;
    view.zoom = 1.75;
    status = session.store->write_view_state(view, "2026-08-16T18:10:03Z");
    expect(status.ok(), "acknowledged session mutation should commit");
    expect(session.store->metadata().revision == 1U, "session mutation should increment sidecar revision");
    session.store.reset();

    const std::filesystem::path session_path = adjacent_session_path(project_path);
    const std::string session_bytes_before_wrong_reader = read_binary(session_path);
    auto session_as_project = ProjectStore::open(session_path);
    expect_error(session_as_project.status, StorageError::invalid_application_id, "project reader must reject a session database before configuring it");
    expect(read_binary(session_path) == session_bytes_before_wrong_reader, "rejected project open must not mutate session bytes");

    auto session_reopen = SessionStore::open_or_create(*reopened.store, "2026-08-16T18:10:04Z");
    expect(session_reopen.ok(), "matching session should reopen");
    if (!session_reopen.ok()) {
        std::cerr << session_reopen.status.diagnostic << '\n';
        return 1;
    }
    auto restored = session_reopen.store->read_view_state();
    expect(restored.ok() && restored.value.has_value(), "committed session view should survive reopen");
    if (restored.value) {
        expect(restored.value->mode == SessionViewState::Mode::globe, "restored mode should match");
        expect(std::abs(restored.value->camera_longitude_rad - view.camera_longitude_rad) < 1e-15, "restored longitude should match");
        expect(std::abs(restored.value->camera_latitude_rad - view.camera_latitude_rad) < 1e-15, "restored latitude should match");
        expect(std::abs(restored.value->zoom - view.zoom) < 1e-15, "restored zoom should match");
    }
    session_reopen.store.reset();

    ProjectCreateOptions other_options = create_options;
    other_options.timestamp_utc = "2026-08-16T18:10:05Z";
    other_options.project_uuid = "fedcba98-7654-4321-8abc-def012345678";
    const std::filesystem::path other_project_path = root / "other.aeris";
    auto other_project = ProjectStore::create(other_project_path, other_options);
    expect(other_project.ok(), "second project should create for stale-sidecar mismatch test");
    if (!other_project.ok()) return 1;

    const std::filesystem::path stale_session_path = adjacent_session_path(other_project_path);
    std::filesystem::copy_file(session_path, stale_session_path, std::filesystem::copy_options::overwrite_existing, ec);
    expect(!ec, "valid sidecar should copy beside a different project for UUID mismatch test");
    const std::string stale_bytes_before = read_binary(stale_session_path);
    auto mismatch = SessionStore::open_or_create(*other_project.store, "2026-08-16T18:10:06Z");
    expect_error(mismatch.status, StorageError::session_project_mismatch, "stale sidecar must fail closed on project UUID mismatch");
    expect(read_binary(stale_session_path) == stale_bytes_before, "UUID-mismatched sidecar must not be mutated before rejection");

    auto session_after_mismatch = SessionStore::open_or_create(*reopened.store, "2026-08-16T18:10:07Z");
    expect(session_after_mismatch.ok(), "mismatch beside another project must not affect the original matching sidecar");
    if (session_after_mismatch.ok()) {
        expect(session_after_mismatch.store->metadata().revision == 1U, "mismatch attempt must not mutate original session revision");
    }
    session_after_mismatch.store.reset();

    std::filesystem::remove(session_path, ec);
    expect(!ec, "session sidecar should be independently deletable");
    auto project_without_session = ProjectStore::open(project_path);
    expect(project_without_session.ok(), "deleting session must not change canonical project semantics");
    project_without_session.store.reset();

    ProjectCreateOptions invalid_time = create_options;
    invalid_time.timestamp_utc = "2026-08-16 18:10:00";
    auto invalid = ProjectStore::create(root / "bad.aeris", invalid_time);
    expect_error(invalid.status, StorageError::invalid_argument, "non-canonical timestamp should fail at API boundary");

    ProjectCreateOptions invalid_calendar = create_options;
    invalid_calendar.timestamp_utc = "2026-02-31T18:10:00Z";
    auto invalid_date = ProjectStore::create(root / "bad-date.aeris", invalid_calendar);
    expect_error(invalid_date.status, StorageError::invalid_argument, "impossible Gregorian date should fail at API boundary");

    const std::filesystem::path wrong_id_path = root / "wrong-id.aeris";
    std::filesystem::copy_file(project_path, wrong_id_path, std::filesystem::copy_options::overwrite_existing, ec);
    expect(!ec, "project fixture should copy for identity corruption test");
    {
        std::fstream file(wrong_id_path, std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(68);
        const char zero[4] = {0, 0, 0, 0};
        file.write(zero, 4);
    }
    auto wrong_id = ProjectStore::open(wrong_id_path);
    expect_error(wrong_id.status, StorageError::invalid_application_id, "application_id mismatch should reject a SQLite file even if schema bytes remain valid");

    std::filesystem::remove_all(root, ec);
    return failures == 0 ? 0 : 1;
}
