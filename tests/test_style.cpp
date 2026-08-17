// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/storage/layer.hpp"
#include "aeris/storage/resource.hpp"
#include "aeris/storage/style.hpp"
#include "aeris/util/sha256.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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

[[nodiscard]] std::vector<std::uint8_t> f64le(const double value) {
    std::uint64_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    std::vector<std::uint8_t> bytes(8U);
    for (unsigned i = 0U; i < 8U; ++i) {
        bytes[i] = static_cast<std::uint8_t>((bits >> (8U * i)) & 0xffU);
    }
    return bytes;
}

class Fixture final {
public:
    Fixture() {
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path() /
                ("aeris-style-v0-" + std::to_string(stamp));
        std::filesystem::create_directories(root_);
        asset_path_ = root_ / "flag-atlas.bin";
        {
            std::ofstream output(asset_path_, std::ios::binary | std::ios::trunc);
            output << "AERIS style graph deterministic flag atlas";
        }

        aeris::storage::ProjectCreateOptions options{};
        options.timestamp_utc = "2026-08-17T10:20:00Z";
        options.project_uuid = "12345678-9abc-4def-8abc-1234567890ab";
        auto created = aeris::storage::ProjectStore::create(project_path(), options);
        if (created.ok()) project_ = std::move(created.store);
    }

    ~Fixture() {
        project_.reset();
        std::error_code ignored{};
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] aeris::storage::ProjectStore* project() noexcept { return project_.get(); }
    [[nodiscard]] const std::filesystem::path& asset_path() const noexcept { return asset_path_; }
    [[nodiscard]] std::filesystem::path project_path() const { return root_ / "world.aeris"; }
    void close() noexcept { project_.reset(); }

private:
    std::filesystem::path root_;
    std::filesystem::path asset_path_;
    std::unique_ptr<aeris::storage::ProjectStore> project_;
};

[[nodiscard]] aeris::storage::ProjectResourceIdentity atlas_identity(
    const std::filesystem::path& path) {
    aeris::storage::ProjectResourceIdentity resource{};
    resource.resource_id = "style.flags.atlas";
    resource.sha256 = aeris::util::sha256_file(path).digest.hex();
    resource.media_type = "application/octet-stream";
    resource.size_bytes = static_cast<std::uint64_t>(std::filesystem::file_size(path));
    resource.retrieval_uri = "https://example.invalid/aeris/style-flags-atlas";
    resource.required_for_reproduction = false;
    return resource;
}

[[nodiscard]] aeris::storage::ProjectStyleRecord base_style() {
    using namespace aeris::storage;
    ProjectStyleRecord style{};
    style.style_id = "style.political.base";
    style.model_id = "aeris.style.political.v1";
    style.name = "Political base";
    style.properties.push_back({"fill.rgba", std::string(kStyleValueRgba8V1), {24U, 96U, 160U, 255U}});
    style.properties.push_back({"outline.enabled", std::string(kStyleValueBoolV1), {1U}});
    style.resources.push_back({"flags.atlas", "style.flags.atlas"});
    return style;
}

[[nodiscard]] aeris::storage::ProjectStyleRecord child_style() {
    using namespace aeris::storage;
    ProjectStyleRecord style{};
    style.style_id = "style.political.flags";
    style.model_id = "aeris.style.flag-satellites.v1";
    style.name = "Flag satellites";
    style.parent_style_id = "style.political.base";
    style.properties.push_back({"symbol.opacity", std::string(kStyleValueF64LeV1), f64le(0.875)});
    style.properties.push_back({"module.payload", "example.style.opaque.v1", {}});
    return style;
}

void test_style_graph_and_portability() {
    using namespace aeris::storage;

    Fixture fixture{};
    ProjectStore* project = fixture.project();
    expect_true("style project creates", project != nullptr);
    if (project == nullptr) return;
    expect_true("generation 6 project advertises draft 0.6",
                project->metadata().format_major == 0 && project->metadata().format_minor == 6);

    const ProjectResourceIdentity atlas = atlas_identity(fixture.asset_path());
    const auto stored_asset = store_external_resource(
        *project, atlas, "2026-08-17T10:20:01Z");
    expect_true("optional style atlas stores", stored_asset.ok());
    expect_true("style asset insert is revision one", project->metadata().revision == 1U);

    const auto frozen = freeze_project(*project, "2026-08-17T10:20:02Z");
    expect_true("optional external style asset does not block freeze", frozen.ok());
    expect_true("initial style freeze is revision two", project->metadata().revision == 2U);
    expect_true("style project starts frozen", project->metadata().frozen);

    ProjectStyleRecord base = base_style();
    const auto base_insert = set_style(*project, base, "2026-08-17T10:20:03Z");
    expect_true("base style stores atomically",
                base_insert.ok() && base_insert.changed && base_insert.durably_committed);
    expect_true("base style is revision three", project->metadata().revision == 3U);
    expect_true("external style resource atomically thaws project", !project->metadata().frozen);

    const auto promoted = list_project_resources(*project);
    expect_true("style atlas still lists", promoted.ok() && promoted.records.size() == 1U);
    if (promoted.ok() && promoted.records.size() == 1U) {
        expect_true("style resource promoted to required",
                    promoted.records.front().identity.required_for_reproduction);
    }

    const auto base_retry = set_style(*project, base, "2026-08-17T10:20:04Z");
    expect_true("exact base style retry is idempotent",
                base_retry.ok() && !base_retry.changed && !base_retry.durably_committed);
    expect_true("base retry keeps revision three", project->metadata().revision == 3U);

    ProjectStyleRecord bad = base;
    bad.name = "Must roll back";
    bad.resources.push_back({"missing", "resource.missing"});
    const auto rollback = set_style(*project, bad, "2026-08-17T10:20:05Z");
    expect_error("missing style resource aborts complete style replacement",
                 rollback.status, StorageError::record_not_found);
    expect_true("failed style replacement keeps revision three", project->metadata().revision == 3U);
    const auto after_rollback = list_project_styles(*project);
    expect_true("failed replacement preserves prior base style",
                after_rollback.ok() && after_rollback.records.size() == 1U &&
                after_rollback.records.front().name == "Political base");

    const auto embedded = embed_resource_file(
        *project, atlas, fixture.asset_path(), "2026-08-17T10:20:06Z");
    expect_true("style atlas embeds", embedded.ok());
    expect_true("style atlas embed is revision four", project->metadata().revision == 4U);
    const auto refrozen = freeze_project(*project, "2026-08-17T10:20:07Z");
    expect_true("style project refreezes after embed", refrozen.ok());
    expect_true("style refreeze is revision five", project->metadata().revision == 5U);
    expect_true("style project is frozen again", project->metadata().frozen);

    ProjectStyleRecord child = child_style();
    const auto child_insert = set_style(*project, child, "2026-08-17T10:20:08Z");
    expect_true("child style with typed and opaque properties stores", child_insert.ok());
    expect_true("child style is revision six", project->metadata().revision == 6U);
    expect_true("inline child style preserves frozen state", project->metadata().frozen);

    LayerCreateRequest layer{};
    layer.layer_id = "layer.flags";
    layer.role_id = "aeris.layer.flags.satellites.v1";
    layer.name = "Flag satellites";
    const auto layer_insert = append_layer(*project, layer, "2026-08-17T10:20:09Z");
    expect_true("style target layer appends", layer_insert.ok());
    expect_true("style target layer is revision seven", project->metadata().revision == 7U);

    const std::vector<LayerStyleBinding> bindings{
        {"paint", child.style_id},
        {"symbol", base.style_id},
    };
    const auto bound = set_layer_style_bindings(
        *project, layer.layer_id, bindings, "2026-08-17T10:20:10Z");
    expect_true("named layer style slots bind atomically", bound.ok() && bound.changed);
    expect_true("layer style binding is revision eight", project->metadata().revision == 8U);
    expect_true("binding styles backed by embedded resources stays frozen", project->metadata().frozen);

    const auto binding_retry = set_layer_style_bindings(
        *project, layer.layer_id, bindings, "2026-08-17T10:20:11Z");
    expect_true("exact layer style binding retry is idempotent",
                binding_retry.ok() && !binding_retry.changed);
    expect_true("binding retry keeps revision eight", project->metadata().revision == 8U);

    ProjectStyleRecord cycle = base;
    cycle.parent_style_id = child.style_id;
    const auto cycle_result = set_style(*project, cycle, "2026-08-17T10:20:12Z");
    expect_error("style inheritance cycle rejected", cycle_result.status, StorageError::invalid_argument);
    expect_true("cycle rejection keeps revision eight", project->metadata().revision == 8U);

    ProjectStyleRecord negative_zero = child;
    negative_zero.properties.clear();
    negative_zero.properties.push_back({
        "symbol.opacity", std::string(kStyleValueF64LeV1),
        {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0x80U}});
    const auto negative_zero_result = set_style(
        *project, negative_zero, "2026-08-17T10:20:13Z");
    expect_error("negative zero f64 style value rejected",
                 negative_zero_result.status, StorageError::invalid_argument);
    expect_true("negative zero rejection keeps revision eight", project->metadata().revision == 8U);

    const auto blocked_remove = remove_style(
        *project, base.style_id, "2026-08-17T10:20:14Z");
    expect_error("referenced parent style cannot be removed",
                 blocked_remove.status, StorageError::record_exists);
    expect_true("blocked style removal keeps revision eight", project->metadata().revision == 8U);

    const auto listed = list_project_styles(*project);
    expect_true("style catalog lists base and child", listed.ok() && listed.records.size() == 2U);
    if (listed.ok() && listed.records.size() == 2U) {
        expect_true("style catalog deterministic by ID",
                    listed.records[0].style_id == base.style_id &&
                    listed.records[1].style_id == child.style_id);
        expect_true("child parent persists",
                    listed.records[1].parent_style_id.has_value() &&
                    *listed.records[1].parent_style_id == base.style_id);
        expect_true("child typed properties persist",
                    listed.records[1].properties.size() == 2U);
    }
    expect_true("valid style graph passes deep integrity", project->verify_integrity().ok());

    const std::filesystem::path path = fixture.project_path();
    fixture.close();
    project = nullptr;
    auto reopened = ProjectStore::open(path);
    expect_true("style project reopens", reopened.ok());
    if (!reopened.ok()) return;
    const auto reopened_bindings = list_layer_style_bindings(*reopened.store, layer.layer_id);
    expect_true("layer style slots survive reopen",
                reopened_bindings.ok() && reopened_bindings.records.size() == 2U &&
                reopened_bindings.records[0].slot_id == "paint" &&
                reopened_bindings.records[1].slot_id == "symbol");
    expect_true("reopened style project passes integrity", reopened.store->verify_integrity().ok());

    sqlite3* raw = nullptr;
    const std::string path_string = path.string();
    const int open_rc = sqlite3_open_v2(path_string.c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr);
    expect_true("hostile style database opens", open_rc == SQLITE_OK && raw != nullptr);
    if (open_rc == SQLITE_OK && raw != nullptr) {
        char* error = nullptr;
        const int rc = sqlite3_exec(
            raw,
            "UPDATE aeris_style_property SET value_payload=X'000000000000F87F' "
            "WHERE style_id='style.political.flags' AND property_key='symbol.opacity';",
            nullptr, nullptr, &error);
        if (error != nullptr) sqlite3_free(error);
        expect_true("hostile NaN style payload writes below API", rc == SQLITE_OK);
        sqlite3_close(raw);
        raw = nullptr;
    }
    const Status hostile = reopened.store->verify_integrity();
    expect_error("deep integrity rejects hostile NaN style payload",
                 hostile, StorageError::schema_invalid);
}

}  // namespace

int main() {
    test_style_graph_and_portability();
    if (failures != 0) {
        std::cerr << failures << " style assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "project_storage_style: PASS\n";
    return EXIT_SUCCESS;
}
