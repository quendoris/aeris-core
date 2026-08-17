// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/storage/projection.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

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
        root_ = std::filesystem::temp_directory_path() / "aeris-projection-storage-v0";
        std::error_code ignored{};
        std::filesystem::remove_all(root_, ignored);
        std::filesystem::create_directories(root_);

        aeris::storage::ProjectCreateOptions options{};
        options.timestamp_utc = "2026-08-17T10:30:00Z";
        options.project_uuid = "abcdef12-3456-4abc-8def-1234567890ab";
        auto created = aeris::storage::ProjectStore::create(path(), options);
        if (created.ok()) project_ = std::move(created.store);
    }

    ~Fixture() {
        project_.reset();
        std::error_code ignored{};
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] aeris::storage::ProjectStore* project() noexcept { return project_.get(); }
    [[nodiscard]] std::filesystem::path path() const { return root_ / "world.aeris"; }
    void close() noexcept { project_.reset(); }

private:
    std::filesystem::path root_;
    std::unique_ptr<aeris::storage::ProjectStore> project_;
};

void test_projection_state() {
    using namespace aeris::storage;
    constexpr double pi = 3.141592653589793238462643383279502884;

    Fixture fixture{};
    ProjectStore* project = fixture.project();
    expect_true("projection project creates", project != nullptr);
    if (project == nullptr) return;

    expect_true("projection project uses current draft constants",
                project->metadata().format_major == kDraftFormatMajor &&
                project->metadata().format_minor == kDraftFormatMinor);
    expect_true("projection project starts revision zero", project->metadata().revision == 0U);

    const auto initial = load_project_projection(*project);
    expect_true("default structured projection loads", initial.ok());
    if (initial.ok()) {
        expect_true("default projection model is explicit unspecified",
                    initial.record.model_id == kProjectionModelUnspecified);
        expect_true("default projection central meridian is canonical zero",
                    initial.record.central_meridian_rad == 0.0 &&
                    !std::signbit(initial.record.central_meridian_rad));
        expect_true("default cut model is explicit unspecified",
                    initial.record.cut_model_id == kProjectionCutModelUnspecifiedV1);
    }

    ProjectMetadataUpdate legacy{};
    legacy.modified_utc = "2026-08-17T10:30:01Z";
    legacy.projection_id = std::string(kProjectionModelSinusoidalV1);
    const Status rejected_legacy = project->update_metadata(legacy);
    expect_error("generic metadata projection mutation rejected",
                 rejected_legacy, StorageError::invalid_argument);
    expect_true("rejected legacy projection keeps revision zero",
                project->metadata().revision == 0U);

    ProjectProjectionRecord sinusoidal{};
    sinusoidal.model_id = std::string(kProjectionModelSinusoidalV1);
    sinusoidal.central_meridian_rad = -0.0;
    sinusoidal.cut_model_id = std::string(kProjectionCutModelSingleAntimeridianV1);
    const auto first = set_project_projection(
        *project, sinusoidal, "2026-08-17T10:30:02Z");
    expect_true("sinusoidal structured projection commits",
                first.ok() && first.changed && first.durably_committed);
    expect_true("sinusoidal projection advances one revision",
                project->metadata().revision == 1U);
    expect_true("metadata summary synchronizes to sinusoidal model",
                project->metadata().projection_id == kProjectionModelSinusoidalV1);

    const auto stored_sinusoidal = load_project_projection(*project);
    expect_true("sinusoidal structured projection reloads", stored_sinusoidal.ok());
    if (stored_sinusoidal.ok()) {
        expect_true("stored negative zero canonicalizes to positive zero",
                    stored_sinusoidal.record.central_meridian_rad == 0.0 &&
                    !std::signbit(stored_sinusoidal.record.central_meridian_rad));
    }

    sinusoidal.central_meridian_rad = 0.0;
    const auto retry = set_project_projection(
        *project, sinusoidal, "2026-08-17T10:30:03Z");
    expect_true("canonical-equivalent projection retry is idempotent",
                retry.ok() && !retry.changed && !retry.durably_committed);
    expect_true("projection retry keeps revision one", project->metadata().revision == 1U);

    ProjectProjectionRecord mollweide{};
    mollweide.model_id = std::string(kProjectionModelMollweideV1);
    mollweide.central_meridian_rad = 3.0 * pi;
    mollweide.cut_model_id = std::string(kProjectionCutModelSingleAntimeridianV1);
    const auto second = set_project_projection(
        *project, mollweide, "2026-08-17T10:30:04Z");
    expect_true("mollweide projection commits", second.ok() && second.changed);
    expect_true("mollweide projection is revision two", project->metadata().revision == 2U);
    const auto stored_mollweide = load_project_projection(*project);
    expect_true("mollweide projection loads", stored_mollweide.ok());
    if (stored_mollweide.ok()) {
        expect_true("3pi canonicalizes to positive pi",
                    stored_mollweide.record.central_meridian_rad == pi);
    }

    ProjectProjectionRecord wrong_cut = mollweide;
    wrong_cut.cut_model_id = std::string(kProjectionCutModelUnspecifiedV1);
    const auto rejected_cut = set_project_projection(
        *project, wrong_cut, "2026-08-17T10:30:05Z");
    expect_error("known projection with wrong cut model rejected",
                 rejected_cut.status, StorageError::invalid_argument);
    expect_true("wrong cut rejection keeps revision two", project->metadata().revision == 2U);

    ProjectProjectionRecord nonfinite = mollweide;
    nonfinite.central_meridian_rad = std::numeric_limits<double>::quiet_NaN();
    const auto rejected_nan = set_project_projection(
        *project, nonfinite, "2026-08-17T10:30:06Z");
    expect_error("nonfinite projection meridian rejected",
                 rejected_nan.status, StorageError::invalid_argument);
    expect_true("NaN rejection keeps revision two", project->metadata().revision == 2U);

    ProjectProjectionRecord custom{};
    custom.model_id = "example.projection.interrupted.v1";
    custom.central_meridian_rad = -pi;
    custom.cut_model_id = "example.cut.interrupted.v1";
    const auto custom_write = set_project_projection(
        *project, custom, "2026-08-17T10:30:07Z");
    expect_true("bounded unknown projection semantics preserve explicitly",
                custom_write.ok() && custom_write.changed);
    expect_true("custom projection is revision three", project->metadata().revision == 3U);
    const auto stored_custom = load_project_projection(*project);
    expect_true("custom projection round-trips", stored_custom.ok());
    if (stored_custom.ok()) {
        expect_true("negative pi canonicalizes to positive pi",
                    stored_custom.record.central_meridian_rad == pi);
        expect_true("custom cut identity preserves exactly",
                    stored_custom.record.cut_model_id == custom.cut_model_id);
    }

    ProjectMetadataUpdate worldview{};
    worldview.modified_utc = "2026-08-17T10:30:08Z";
    worldview.worldview_id = "neutral-disputed";
    const Status worldview_status = project->update_metadata(worldview);
    expect_true("ordinary worldview metadata still mutates", worldview_status.ok());
    expect_true("worldview update is revision four", project->metadata().revision == 4U);
    expect_true("worldview update leaves projection summary untouched",
                project->metadata().projection_id == custom.model_id);

    expect_true("valid structured projection passes deep integrity",
                project->verify_integrity().ok());

    const std::filesystem::path path = fixture.path();
    fixture.close();
    project = nullptr;
    auto reopened = ProjectStore::open(path);
    expect_true("structured projection project reopens", reopened.ok());
    if (!reopened.ok()) return;
    const auto reopened_projection = load_project_projection(*reopened.store);
    expect_true("projection survives close/reopen",
                reopened_projection.ok() &&
                reopened_projection.record.model_id == custom.model_id &&
                reopened_projection.record.central_meridian_rad == pi);

    sqlite3* raw = nullptr;
    const std::string path_string = path.string();
    const int open_rc = sqlite3_open_v2(
        path_string.c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr);
    expect_true("hostile projection database opens", open_rc == SQLITE_OK && raw != nullptr);
    if (open_rc == SQLITE_OK && raw != nullptr) {
        char* error = nullptr;
        const int rc = sqlite3_exec(
            raw,
            "UPDATE aeris_projection SET central_meridian_f64le=X'000000000000F87F' WHERE id=1;",
            nullptr, nullptr, &error);
        if (error != nullptr) sqlite3_free(error);
        expect_true("hostile projection NaN writes below API", rc == SQLITE_OK);
        sqlite3_close(raw);
        raw = nullptr;
    }
    const Status hostile = reopened.store->verify_integrity();
    expect_error("deep integrity rejects hostile projection NaN",
                 hostile, StorageError::schema_invalid);
}

}  // namespace

int main() {
    test_projection_state();
    if (failures != 0) {
        std::cerr << failures << " projection assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "project_storage_projection: PASS\n";
    return EXIT_SUCCESS;
}
