// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/storage/resource.hpp"
#include "aeris/util/sha256.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
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

class Fixture final {
public:
    Fixture() {
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path() /
                ("aeris-resource-v0-" + std::to_string(stamp));
        std::filesystem::create_directories(root_);

        payload_.resize(aeris::storage::kEmbeddedResourceChunkBytes + 37U);
        for (std::size_t i = 0U; i < payload_.size(); ++i) {
            payload_[i] = static_cast<unsigned char>((i * 131U + 17U) & 0xffU);
        }
        payload_path_ = root_ / "payload.bin";
        {
            std::ofstream output(payload_path_, std::ios::binary | std::ios::trunc);
            output.write(
                reinterpret_cast<const char*>(payload_.data()),
                static_cast<std::streamsize>(payload_.size()));
        }

        optional_path_ = root_ / "optional.bin";
        {
            std::ofstream output(optional_path_, std::ios::binary | std::ios::trunc);
            output << "optional external payload";
        }

        second_required_path_ = root_ / "required-2.bin";
        {
            std::ofstream output(second_required_path_, std::ios::binary | std::ios::trunc);
            output << "second required payload";
        }

        aeris::storage::ProjectCreateOptions options{};
        options.timestamp_utc = "2026-08-17T09:00:00Z";
        options.project_uuid = "12345678-9abc-4def-8abc-123456789abc";
        auto created = aeris::storage::ProjectStore::create(root_ / "world.aeris", options);
        if (created.ok()) project_ = std::move(created.store);
    }

    ~Fixture() {
        project_.reset();
        std::error_code ignored{};
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] aeris::storage::ProjectStore* project() noexcept { return project_.get(); }
    [[nodiscard]] const std::filesystem::path& payload_path() const noexcept { return payload_path_; }
    [[nodiscard]] const std::filesystem::path& optional_path() const noexcept { return optional_path_; }
    [[nodiscard]] const std::filesystem::path& second_required_path() const noexcept { return second_required_path_; }
    [[nodiscard]] const std::vector<unsigned char>& payload() const noexcept { return payload_; }

    [[nodiscard]] std::filesystem::path project_path() const {
        return root_ / "world.aeris";
    }

    void close() noexcept { project_.reset(); }

private:
    std::filesystem::path root_;
    std::filesystem::path payload_path_;
    std::filesystem::path optional_path_;
    std::filesystem::path second_required_path_;
    std::vector<unsigned char> payload_;
    std::unique_ptr<aeris::storage::ProjectStore> project_;
};

[[nodiscard]] aeris::storage::ProjectResourceIdentity identity_for(
    const std::string& id,
    const std::filesystem::path& path,
    const bool required,
    const std::string& uri) {
    aeris::storage::ProjectResourceIdentity identity{};
    identity.resource_id = id;
    identity.sha256 = aeris::util::sha256_file(path).digest.hex();
    identity.media_type = "application/octet-stream";
    identity.size_bytes = static_cast<std::uint64_t>(std::filesystem::file_size(path));
    identity.retrieval_uri = uri;
    identity.required_for_reproduction = required;
    return identity;
}

void test_requirement_promotion_is_monotonic() {
    using namespace aeris::storage;

    Fixture fixture{};
    ProjectStore* project = fixture.project();
    expect_true("promotion project creates", project != nullptr);
    if (project == nullptr) return;

    ProjectResourceIdentity optional = identity_for(
        "asset.promoted", fixture.optional_path(), false,
        "https://example.invalid/aeris/promoted");
    const auto inserted = store_external_resource(
        *project, optional, "2026-08-17T09:10:01Z");
    expect_true("optional promotion fixture stores", inserted.ok() && inserted.inserted);
    expect_true("optional promotion fixture is revision one", project->metadata().revision == 1U);

    const auto frozen = freeze_project(*project, "2026-08-17T09:10:02Z");
    expect_true("optional promotion fixture can freeze", frozen.ok());
    expect_true("optional promotion fixture freeze is revision two", project->metadata().revision == 2U);
    expect_true("optional promotion fixture is frozen", project->metadata().frozen);

    ProjectResourceIdentity required = optional;
    required.required_for_reproduction = true;
    const auto promoted = store_external_resource(
        *project, required, "2026-08-17T09:10:03Z");
    expect_true("same content identity promotes requirement",
                promoted.ok() && !promoted.inserted && !promoted.representation_changed &&
                promoted.durably_committed);
    expect_true("requirement promotion is one revision", project->metadata().revision == 3U);
    expect_true("promoted external requirement atomically thaws project", !project->metadata().frozen);

    auto listed = list_project_resources(*project);
    expect_true("promoted resource lists", listed.ok() && listed.records.size() == 1U);
    if (listed.ok() && listed.records.size() == 1U) {
        expect_true("required bit promoted", listed.records.front().identity.required_for_reproduction);
    }

    const auto no_downgrade = store_external_resource(
        *project, optional, "2026-08-17T09:10:04Z");
    expect_true("optional retry cannot downgrade required resource",
                no_downgrade.ok() && !no_downgrade.durably_committed);
    expect_true("no-downgrade retry keeps revision three", project->metadata().revision == 3U);
    listed = list_project_resources(*project);
    expect_true("required bit remains monotonic",
                listed.ok() && listed.records.size() == 1U &&
                listed.records.front().identity.required_for_reproduction);

    const auto embedded = embed_resource_file(
        *project, optional, fixture.optional_path(), "2026-08-17T09:10:05Z");
    expect_true("embedding accepts original immutable content identity after promotion",
                embedded.ok() && !embedded.inserted && embedded.representation_changed &&
                embedded.durably_committed);
    expect_true("promoted embed is revision four", project->metadata().revision == 4U);
    listed = list_project_resources(*project);
    expect_true("embedding never demotes required bit",
                listed.ok() && listed.records.size() == 1U &&
                listed.records.front().identity.required_for_reproduction &&
                listed.records.front().storage_mode == ResourceStorageMode::embedded);

    const auto refrozen = freeze_project(*project, "2026-08-17T09:10:06Z");
    expect_true("promoted embedded resource permits refreeze", refrozen.ok());
    expect_true("promoted resource refreeze is revision five", project->metadata().revision == 5U);
    expect_true("promotion project returns to frozen", project->metadata().frozen);
}

void test_portable_resource_lifecycle() {
    using namespace aeris::storage;

    Fixture fixture{};
    ProjectStore* project = fixture.project();
    expect_true("resource project creates", project != nullptr);
    if (project == nullptr) return;

    ProjectResourceIdentity primary = identity_for(
        "asset.primary", fixture.payload_path(), true,
        "https://example.invalid/aeris/asset-primary");

    ProjectResourceIdentity local_uri = primary;
    local_uri.resource_id = "asset.local-path";
    local_uri.retrieval_uri = "file:///tmp/machine-local.bin";
    auto invalid_uri = store_external_resource(
        *project, local_uri, "2026-08-17T09:00:01Z");
    expect_error("file URI rejected", invalid_uri.status, StorageError::invalid_argument);
    expect_true("file URI rejection keeps revision zero", project->metadata().revision == 0U);

    auto external = store_external_resource(
        *project, primary, "2026-08-17T09:00:02Z");
    expect_true("required external resource stores", external.ok());
    expect_true("required external resource inserted", external.inserted);
    expect_true("required external resource commits", external.durably_committed);
    expect_true("external resource is one revision", project->metadata().revision == 1U);
    expect_true("required external resource keeps project unfrozen", !project->metadata().frozen);

    auto retry_external = store_external_resource(
        *project, primary, "2026-08-17T09:00:03Z");
    expect_true("external exact retry succeeds", retry_external.ok());
    expect_true("external exact retry is idempotent", !retry_external.durably_committed);
    expect_true("external exact retry keeps revision", project->metadata().revision == 1U);

    auto blocked_freeze = freeze_project(*project, "2026-08-17T09:00:04Z");
    expect_error("required external blocks freeze", blocked_freeze.status, StorageError::invalid_argument);
    expect_true("blocked freeze keeps revision", project->metadata().revision == 1U);
    expect_true("blocked freeze stays unfrozen", !project->metadata().frozen);

    ProjectResourceIdentity wrong_hash = identity_for(
        "asset.bad-hash", fixture.payload_path(), true,
        "https://example.invalid/aeris/bad-hash");
    wrong_hash.sha256 = std::string(64U, '0');
    auto bad_embed = embed_resource_file(
        *project, wrong_hash, fixture.payload_path(), "2026-08-17T09:00:05Z");
    expect_error("wrong aggregate hash rolls embedded mutation back", bad_embed.status, StorageError::integrity_failed);
    expect_true("wrong aggregate hash keeps revision", project->metadata().revision == 1U);

    auto embedded = embed_resource_file(
        *project, primary, fixture.payload_path(), "2026-08-17T09:00:06Z");
    expect_true("external resource upgrades to embedded", embedded.ok());
    expect_true("embedding existing identity changes representation", embedded.representation_changed);
    expect_true("embedding existing identity is not a second identity insert", !embedded.inserted);
    expect_true("embedding commits", embedded.durably_committed);
    expect_true("embedding is one additional revision", project->metadata().revision == 2U);

    const auto resources = list_project_resources(*project);
    expect_true("resource list succeeds", resources.ok());
    expect_true("failed embed left no extra record", resources.records.size() == 1U);
    if (resources.ok() && resources.records.size() == 1U) {
        const auto& record = resources.records.front();
        expect_true("embedded mode persists", record.storage_mode == ResourceStorageMode::embedded);
        expect_true("multi-chunk payload uses two chunks", record.chunk_count == 2U);
        expect_true("resource URI preserved without input path", record.identity.retrieval_uri == primary.retrieval_uri);
    }

    std::vector<unsigned char> streamed;
    const Status streamed_status = stream_embedded_resource(
        *project,
        primary.resource_id,
        [&](const void* data, const std::size_t size) {
            const auto* bytes = static_cast<const unsigned char*>(data);
            streamed.insert(streamed.end(), bytes, bytes + size);
            return Status::success();
        });
    expect_true("embedded resource streams with verification", streamed_status.ok());
    expect_true("streamed embedded bytes are exact", streamed == fixture.payload());
    expect_true("embedded project passes deep integrity", project->verify_integrity().ok());

    auto frozen = freeze_project(*project, "2026-08-17T09:00:07Z");
    expect_true("fully embedded required resources freeze", frozen.ok());
    expect_true("freeze changes portable state", frozen.changed && frozen.durably_committed);
    expect_true("freeze advances one revision", project->metadata().revision == 3U);
    expect_true("freeze marker becomes true", project->metadata().frozen);

    auto frozen_retry = freeze_project(*project, "2026-08-17T09:00:08Z");
    expect_true("exact freeze retry succeeds", frozen_retry.ok());
    expect_true("exact freeze retry is idempotent", !frozen_retry.changed && !frozen_retry.durably_committed);
    expect_true("exact freeze retry keeps revision", project->metadata().revision == 3U);

    ProjectResourceIdentity optional = identity_for(
        "asset.optional", fixture.optional_path(), false,
        "https://example.invalid/aeris/optional");
    auto optional_external = store_external_resource(
        *project, optional, "2026-08-17T09:00:09Z");
    expect_true("optional external resource stores", optional_external.ok());
    expect_true("optional external advances revision", project->metadata().revision == 4U);
    expect_true("optional external does not invalidate frozen state", project->metadata().frozen);

    ProjectResourceIdentity second_required = identity_for(
        "asset.required-2", fixture.second_required_path(), true,
        "urn:aeris:test:required-2");
    auto second_external = store_external_resource(
        *project, second_required, "2026-08-17T09:00:10Z");
    expect_true("new required external resource stores", second_external.ok());
    expect_true("new required external advances revision", project->metadata().revision == 5U);
    expect_true("new required external atomically thaws project", !project->metadata().frozen);

    auto blocked_again = freeze_project(*project, "2026-08-17T09:00:11Z");
    expect_error("second required external blocks refreeze", blocked_again.status, StorageError::invalid_argument);
    expect_true("blocked refreeze keeps revision", project->metadata().revision == 5U);

    auto second_embedded = embed_resource_file(
        *project, second_required, fixture.second_required_path(), "2026-08-17T09:00:12Z");
    expect_true("second required resource embeds", second_embedded.ok());
    expect_true("second embedding advances revision", project->metadata().revision == 6U);
    expect_true("embedding alone does not assert frozen state", !project->metadata().frozen);

    auto refrozen = freeze_project(*project, "2026-08-17T09:00:13Z");
    expect_true("project refreezes after all required assets embed", refrozen.ok());
    expect_true("refreeze advances revision", project->metadata().revision == 7U);
    expect_true("refreeze marker true", project->metadata().frozen);
    expect_true("refrozen project passes deep integrity", project->verify_integrity().ok());

    const std::filesystem::path project_path = fixture.project_path();
    fixture.close();
    project = nullptr;

    auto reopened = ProjectStore::open(project_path);
    expect_true("frozen project reopens", reopened.ok());
    if (!reopened.ok()) return;
    expect_true("frozen marker survives reopen", reopened.store->metadata().frozen);
    expect_true("resource revisions survive reopen", reopened.store->metadata().revision == 7U);
    expect_true("reopened frozen project passes content integrity", reopened.store->verify_integrity().ok());

    std::vector<unsigned char> reopened_bytes;
    const Status reopen_stream = stream_embedded_resource(
        *reopened.store,
        primary.resource_id,
        [&](const void* data, const std::size_t size) {
            const auto* bytes = static_cast<const unsigned char*>(data);
            reopened_bytes.insert(reopened_bytes.end(), bytes, bytes + size);
            return Status::success();
        });
    expect_true("reopened embedded resource streams", reopen_stream.ok());
    expect_true("reopened resource bytes are exact", reopened_bytes == fixture.payload());

    sqlite3* raw = nullptr;
    const std::string sqlite_path = project_path.string();
    const int open_rc = sqlite3_open_v2(sqlite_path.c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr);
    expect_true("hostile resource database opens", open_rc == SQLITE_OK && raw != nullptr);
    if (open_rc == SQLITE_OK && raw != nullptr) {
        const char* sql =
            "UPDATE aeris_resource_chunk SET payload=zeroblob(length(payload)) "
            "WHERE resource_id='asset.primary' AND chunk_index=0;";
        char* error = nullptr;
        const int rc = sqlite3_exec(raw, sql, nullptr, nullptr, &error);
        if (error != nullptr) sqlite3_free(error);
        expect_true("hostile payload corruption writes", rc == SQLITE_OK);
        sqlite3_close(raw);
        raw = nullptr;
    }

    const Status corrupt = reopened.store->verify_integrity();
    expect_error("deep integrity detects same-length payload corruption", corrupt, StorageError::integrity_failed);
}

}  // namespace

int main() {
    test_requirement_promotion_is_monotonic();
    test_portable_resource_lifecycle();

    if (failures != 0) {
        std::cerr << failures << " resource assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "project_storage_resource: PASS\n";
    return EXIT_SUCCESS;
}
