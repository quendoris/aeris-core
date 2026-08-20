// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/storage/projection.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

namespace {

[[nodiscard]] bool expect(const char* name, const bool condition) {
    if (!condition) std::cerr << "FAIL " << name << '\n';
    return condition;
}

}  // namespace

int main() {
    using namespace aeris::storage;

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "aeris-projection-adapter-storage-v0";
    const std::filesystem::path path = root / "world.aeris";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root);

    ProjectCreateOptions create{};
    create.timestamp_utc = "2026-08-19T07:50:00Z";
    create.project_uuid = "6a8ad940-bbed-4a14-9cf1-9bb16cf50fc3";
    auto created = ProjectStore::create(path, create);
    if (!expect("projection adapter storage project creates", created.ok())) return EXIT_FAILURE;

    ProjectProjectionRecord lambert{};
    lambert.model_id = std::string(kProjectionModelLambertCylindricalEqualAreaV1);
    lambert.central_meridian_rad = 0.25;
    lambert.cut_model_id = std::string(kProjectionCutModelSingleAntimeridianV1);

    const auto committed = set_project_projection(
        *created.store, lambert, "2026-08-19T07:50:01Z");
    bool ok = true;
    ok = expect(
        "Lambert CEA projection commits with the verified cut",
        committed.ok() && committed.changed && committed.durably_committed) && ok;
    ok = expect(
        "Lambert CEA projection advances exactly one revision",
        created.store->metadata().revision == 1U) && ok;
    ok = expect(
        "Lambert CEA metadata summary synchronizes",
        created.store->metadata().projection_id == kProjectionModelLambertCylindricalEqualAreaV1) && ok;

    ProjectProjectionRecord wrong_cut = lambert;
    wrong_cut.cut_model_id = std::string(kProjectionCutModelUnspecifiedV1);
    const auto rejected = set_project_projection(
        *created.store, wrong_cut, "2026-08-19T07:50:02Z");
    ok = expect(
        "Lambert CEA rejects an incompatible cut before mutation",
        rejected.status.error == StorageError::invalid_argument &&
        !rejected.changed && !rejected.durably_committed) && ok;
    ok = expect(
        "rejected Lambert cut keeps the committed revision",
        created.store->metadata().revision == 1U) && ok;

    created.store.reset();
    auto reopened = ProjectStore::open(path);
    ok = expect("Lambert CEA project reopens", reopened.ok()) && ok;
    if (reopened.ok()) {
        const auto loaded = load_project_projection(*reopened.store);
        ok = expect(
            "Lambert CEA model/cut survive reopen exactly",
            loaded.ok() &&
            loaded.record.model_id == kProjectionModelLambertCylindricalEqualAreaV1 &&
            loaded.record.cut_model_id == kProjectionCutModelSingleAntimeridianV1 &&
            loaded.record.central_meridian_rad == 0.25) && ok;
        ok = expect(
            "Lambert CEA survives deep project integrity",
            reopened.store->verify_integrity().ok()) && ok;
    }

    reopened.store.reset();
    std::filesystem::remove_all(root, ignored);
    if (!ok) return EXIT_FAILURE;
    std::cout << "project_storage_projection_adapter: PASS\n";
    return EXIT_SUCCESS;
}
