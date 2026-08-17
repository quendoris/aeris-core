// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/storage/layer.hpp"

#include "sqlite_detail.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <set>
#include <utility>

#include <sqlite3.h>

namespace aeris::storage {
namespace {

constexpr std::size_t kMaxIdentifierBytes = 255U;
constexpr std::size_t kMaxLayerNameBytes = 1024U;

[[nodiscard]] bool bounded_text(
    const std::string& value,
    const std::size_t max_bytes,
    const bool allow_empty = false
) noexcept {
    return (allow_empty || !value.empty()) && value.size() <= max_bytes &&
           value.find('\0') == std::string::npos;
}

[[nodiscard]] Status validate_slot_id(const std::string& value) {
    if (!bounded_text(value, kMaxIdentifierBytes)) {
        return {StorageError::invalid_argument,
                "layer binding slot ID is empty, contains NUL, or exceeds 255 bytes"};
    }
    return Status::success();
}

[[nodiscard]] Status canonicalize_request(LayerCreateRequest& request) {
    if (!bounded_text(request.layer_id, kMaxIdentifierBytes) ||
        !bounded_text(request.role_id, kMaxIdentifierBytes) ||
        !bounded_text(request.name, kMaxLayerNameBytes)) {
        return {StorageError::invalid_argument,
                "layer ID/role/name violates canonical storage bounds"};
    }
    if (request.sources.size() > kMaxLayerBindings ||
        request.resources.size() > kMaxLayerBindings) {
        return {StorageError::invalid_argument,
                "layer exceeds the 256-binding draft bound"};
    }

    std::set<std::string> source_slots;
    for (const LayerSourceBinding& binding : request.sources) {
        Status status = validate_slot_id(binding.slot_id);
        if (!status) return status;
        if (!bounded_text(binding.source_id, kMaxIdentifierBytes)) {
            return {StorageError::invalid_argument,
                    "layer source binding contains invalid source ID"};
        }
        if (!source_slots.insert(binding.slot_id).second) {
            return {StorageError::invalid_argument,
                    "layer contains duplicate source binding slot"};
        }
    }

    std::set<std::string> resource_slots;
    for (const LayerResourceBinding& binding : request.resources) {
        Status status = validate_slot_id(binding.slot_id);
        if (!status) return status;
        if (!bounded_text(binding.resource_id, kMaxIdentifierBytes)) {
            return {StorageError::invalid_argument,
                    "layer resource binding contains invalid resource ID"};
        }
        if (!resource_slots.insert(binding.slot_id).second) {
            return {StorageError::invalid_argument,
                    "layer contains duplicate resource binding slot"};
        }
    }

    std::sort(
        request.sources.begin(), request.sources.end(),
        [](const LayerSourceBinding& left, const LayerSourceBinding& right) {
            return left.slot_id < right.slot_id;
        }
    );
    std::sort(
        request.resources.begin(), request.resources.end(),
        [](const LayerResourceBinding& left, const LayerResourceBinding& right) {
            return left.slot_id < right.slot_id;
        }
    );
    return Status::success();
}

[[nodiscard]] Status canonicalize_stack(
    const std::vector<LayerCreateRequest>& input,
    std::vector<LayerCreateRequest>& canonical
) {
    if (input.empty() || input.size() > kMaxProjectLayers) {
        return {StorageError::invalid_argument,
                "layer stack initialization requires 1..65535 layers"};
    }

    canonical = input;
    std::set<std::string> layer_ids;
    for (LayerCreateRequest& request : canonical) {
        Status status = canonicalize_request(request);
        if (!status) return status;
        if (!layer_ids.insert(request.layer_id).second) {
            return {StorageError::invalid_argument,
                    "layer stack initialization contains duplicate layer ID"};
        }
    }
    return Status::success();
}

[[nodiscard]] Status validate_project_connection(
    sqlite3* db,
    const ProjectStore& project
) {
    Status status = detail::verify_quick_check(db);
    if (!status) return status;

    sqlite3_int64 application_id = 0;
    status = detail::query_single_int(db, "PRAGMA application_id;", application_id);
    if (!status) return status;
    if (application_id != static_cast<sqlite3_int64>(kProjectApplicationId)) {
        return {StorageError::invalid_application_id,
                "layer stack target is not an AERIS project"};
    }

    sqlite3_int64 generation = 0;
    status = detail::query_single_int(db, "PRAGMA user_version;", generation);
    if (!status) return status;
    if (generation != kProjectSchemaGeneration) {
        return {StorageError::unsupported_schema,
                "layer stack target has unsupported draft schema generation"};
    }

    std::string uuid;
    status = detail::query_single_text(
        db, "SELECT project_uuid FROM aeris_meta WHERE id=1;", uuid);
    if (!status) return status;
    if (uuid != project.metadata().project_uuid) {
        return {StorageError::schema_invalid,
                "layer stack target UUID differs from validated project handle"};
    }
    return Status::success();
}

[[nodiscard]] std::string text_column(sqlite3_stmt* stmt, const int column) {
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, column));
    const int bytes = sqlite3_column_bytes(stmt, column);
    return std::string(text, static_cast<std::size_t>(bytes));
}

[[nodiscard]] Status row_exists(
    sqlite3* db,
    const char* sql,
    const std::string& id,
    bool& exists
) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(db, sql, stmt);
    if (!status) return status;
    status = detail::bind_text(db, stmt.get(), 1, id);
    if (!status) return status;

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) {
        exists = false;
        return Status::success();
    }
    if (rc != SQLITE_ROW || sqlite3_column_type(stmt.get(), 0) != SQLITE_INTEGER) {
        return {StorageError::schema_invalid,
                "layer stack reference probe returned malformed data"};
    }
    exists = true;
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return {StorageError::schema_invalid,
                "layer stack reference probe returned duplicate rows"};
    }
    return Status::success();
}

[[nodiscard]] Status load_source_bindings(
    sqlite3* db,
    const std::string& layer_id,
    std::vector<LayerSourceBinding>& bindings
) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(
        db,
        "SELECT slot_id,source_id FROM aeris_layer_source "
        "WHERE layer_id=? ORDER BY slot_id;",
        stmt);
    if (!status) return status;
    status = detail::bind_text(db, stmt.get(), 1, layer_id);
    if (!status) return status;

    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW || sqlite3_column_type(stmt.get(), 0) != SQLITE_TEXT ||
            sqlite3_column_type(stmt.get(), 1) != SQLITE_TEXT) {
            return {StorageError::schema_invalid,
                    "stored layer source binding has malformed SQLite types"};
        }
        LayerSourceBinding binding{text_column(stmt.get(), 0), text_column(stmt.get(), 1)};
        if (!bounded_text(binding.slot_id, kMaxIdentifierBytes) ||
            !bounded_text(binding.source_id, kMaxIdentifierBytes)) {
            return {StorageError::schema_invalid,
                    "stored layer source binding violates canonical bounds"};
        }
        bindings.push_back(std::move(binding));
        if (bindings.size() > kMaxLayerBindings) {
            return {StorageError::schema_invalid,
                    "stored layer exceeds source binding bound"};
        }
    }
    return Status::success();
}

[[nodiscard]] Status load_resource_bindings(
    sqlite3* db,
    const std::string& layer_id,
    std::vector<LayerResourceBinding>& bindings
) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(
        db,
        "SELECT slot_id,resource_id FROM aeris_layer_resource "
        "WHERE layer_id=? ORDER BY slot_id;",
        stmt);
    if (!status) return status;
    status = detail::bind_text(db, stmt.get(), 1, layer_id);
    if (!status) return status;

    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW || sqlite3_column_type(stmt.get(), 0) != SQLITE_TEXT ||
            sqlite3_column_type(stmt.get(), 1) != SQLITE_TEXT) {
            return {StorageError::schema_invalid,
                    "stored layer resource binding has malformed SQLite types"};
        }
        LayerResourceBinding binding{text_column(stmt.get(), 0), text_column(stmt.get(), 1)};
        if (!bounded_text(binding.slot_id, kMaxIdentifierBytes) ||
            !bounded_text(binding.resource_id, kMaxIdentifierBytes)) {
            return {StorageError::schema_invalid,
                    "stored layer resource binding violates canonical bounds"};
        }
        bindings.push_back(std::move(binding));
        if (bindings.size() > kMaxLayerBindings) {
            return {StorageError::schema_invalid,
                    "stored layer exceeds resource binding bound"};
        }
    }
    return Status::success();
}

[[nodiscard]] Status load_existing_stack(
    sqlite3* db,
    std::vector<ProjectLayerRecord>& records
) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(
        db,
        "SELECT layer_id,role_id,name,ordinal,visible FROM aeris_layer ORDER BY ordinal;",
        stmt);
    if (!status) return status;

    std::uint64_t expected_ordinal = 0U;
    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW || sqlite3_column_type(stmt.get(), 0) != SQLITE_TEXT ||
            sqlite3_column_type(stmt.get(), 1) != SQLITE_TEXT ||
            sqlite3_column_type(stmt.get(), 2) != SQLITE_TEXT ||
            sqlite3_column_type(stmt.get(), 3) != SQLITE_INTEGER ||
            sqlite3_column_type(stmt.get(), 4) != SQLITE_INTEGER) {
            return {StorageError::schema_invalid,
                    "stored layer stack row has malformed SQLite types"};
        }

        const sqlite3_int64 ordinal = sqlite3_column_int64(stmt.get(), 3);
        const sqlite3_int64 visible = sqlite3_column_int64(stmt.get(), 4);
        ProjectLayerRecord record{};
        record.layer_id = text_column(stmt.get(), 0);
        record.role_id = text_column(stmt.get(), 1);
        record.name = text_column(stmt.get(), 2);
        if (ordinal < 0 || static_cast<std::uint64_t>(ordinal) != expected_ordinal ||
            expected_ordinal >= kMaxProjectLayers ||
            (visible != 0 && visible != 1) ||
            !bounded_text(record.layer_id, kMaxIdentifierBytes) ||
            !bounded_text(record.role_id, kMaxIdentifierBytes) ||
            !bounded_text(record.name, kMaxLayerNameBytes)) {
            return {StorageError::schema_invalid,
                    "stored layer stack violates canonical ordering or bounds"};
        }
        record.ordinal = static_cast<std::uint32_t>(expected_ordinal);
        record.visible = visible == 1;

        status = load_source_bindings(db, record.layer_id, record.sources);
        if (!status) return status;
        status = load_resource_bindings(db, record.layer_id, record.resources);
        if (!status) return status;
        records.push_back(std::move(record));
        ++expected_ordinal;
    }
    return Status::success();
}

[[nodiscard]] bool equal_binding(
    const LayerSourceBinding& left,
    const LayerSourceBinding& right
) noexcept {
    return left.slot_id == right.slot_id && left.source_id == right.source_id;
}

[[nodiscard]] bool equal_binding(
    const LayerResourceBinding& left,
    const LayerResourceBinding& right
) noexcept {
    return left.slot_id == right.slot_id && left.resource_id == right.resource_id;
}

[[nodiscard]] bool equal_layer(
    const ProjectLayerRecord& existing,
    const LayerCreateRequest& requested,
    const std::size_t ordinal
) noexcept {
    if (existing.ordinal != ordinal ||
        existing.layer_id != requested.layer_id ||
        existing.role_id != requested.role_id ||
        existing.name != requested.name ||
        existing.visible != requested.visible ||
        existing.sources.size() != requested.sources.size() ||
        existing.resources.size() != requested.resources.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < existing.sources.size(); ++index) {
        if (!equal_binding(existing.sources[index], requested.sources[index])) return false;
    }
    for (std::size_t index = 0U; index < existing.resources.size(); ++index) {
        if (!equal_binding(existing.resources[index], requested.resources[index])) return false;
    }
    return true;
}

[[nodiscard]] bool equal_stack(
    const std::vector<ProjectLayerRecord>& existing,
    const std::vector<LayerCreateRequest>& requested
) noexcept {
    if (existing.size() != requested.size()) return false;
    for (std::size_t index = 0U; index < existing.size(); ++index) {
        if (!equal_layer(existing[index], requested[index], index)) return false;
    }
    return true;
}

[[nodiscard]] Status promote_resource_requirement(
    sqlite3* db,
    const std::string& resource_id,
    bool& external_required
) {
    detail::StmtPtr read;
    Status status = detail::prepare(
        db,
        "SELECT storage_mode,required_for_reproduction FROM aeris_resource WHERE resource_id=?;",
        read);
    if (!status) return status;
    status = detail::bind_text(db, read.get(), 1, resource_id);
    if (!status) return status;

    const int rc = sqlite3_step(read.get());
    if (rc == SQLITE_DONE) {
        return {StorageError::record_not_found,
                "layer resource binding references missing project resource: " + resource_id};
    }
    if (rc != SQLITE_ROW || sqlite3_column_type(read.get(), 0) != SQLITE_INTEGER ||
        sqlite3_column_type(read.get(), 1) != SQLITE_INTEGER) {
        return {StorageError::schema_invalid,
                "layer resource binding target has malformed storage metadata"};
    }
    const sqlite3_int64 mode = sqlite3_column_int64(read.get(), 0);
    const sqlite3_int64 required = sqlite3_column_int64(read.get(), 1);
    if ((mode != 0 && mode != 1) || (required != 0 && required != 1) ||
        sqlite3_step(read.get()) != SQLITE_DONE) {
        return {StorageError::schema_invalid,
                "layer resource binding target violates canonical resource state"};
    }

    if (required == 0) {
        detail::StmtPtr update;
        status = detail::prepare(
            db,
            "UPDATE aeris_resource SET required_for_reproduction=1 "
            "WHERE resource_id=? AND required_for_reproduction=0;",
            update);
        if (status) status = detail::bind_text(db, update.get(), 1, resource_id);
        if (status) status = detail::step_done(db, update.get());
        if (status && sqlite3_changes(db) != 1) {
            return {StorageError::schema_invalid,
                    "layer resource requirement promotion did not affect exactly one row"};
        }
        if (!status) return status;
    }
    if (mode == 0) external_required = true;
    return Status::success();
}

[[nodiscard]] Status insert_bindings(
    sqlite3* db,
    const LayerCreateRequest& request,
    bool& external_required
) {
    for (const LayerSourceBinding& binding : request.sources) {
        bool exists = false;
        Status status = row_exists(
            db,
            "SELECT 1 FROM aeris_source WHERE source_id=?;",
            binding.source_id,
            exists);
        if (!status) return status;
        if (!exists) {
            return {StorageError::record_not_found,
                    "layer source binding references missing project source: " +
                        binding.source_id};
        }

        detail::StmtPtr stmt;
        status = detail::prepare(
            db,
            "INSERT INTO aeris_layer_source(layer_id,slot_id,source_id) VALUES(?,?,?);",
            stmt);
        if (status) status = detail::bind_text(db, stmt.get(), 1, request.layer_id);
        if (status) status = detail::bind_text(db, stmt.get(), 2, binding.slot_id);
        if (status) status = detail::bind_text(db, stmt.get(), 3, binding.source_id);
        if (status) status = detail::step_done(db, stmt.get());
        if (!status) return status;
    }

    for (const LayerResourceBinding& binding : request.resources) {
        Status status = promote_resource_requirement(db, binding.resource_id, external_required);
        if (!status) return status;

        detail::StmtPtr stmt;
        status = detail::prepare(
            db,
            "INSERT INTO aeris_layer_resource(layer_id,slot_id,resource_id) VALUES(?,?,?);",
            stmt);
        if (status) status = detail::bind_text(db, stmt.get(), 1, request.layer_id);
        if (status) status = detail::bind_text(db, stmt.get(), 2, binding.slot_id);
        if (status) status = detail::bind_text(db, stmt.get(), 3, binding.resource_id);
        if (status) status = detail::step_done(db, stmt.get());
        if (!status) return status;
    }
    return Status::success();
}

[[nodiscard]] Status advance_revision(
    sqlite3* db,
    const ProjectStore& project,
    const std::string_view modified_utc,
    const bool invalidate_frozen
) {
    detail::StmtPtr read;
    Status status = detail::prepare(
        db,
        "SELECT revision,frozen FROM aeris_meta WHERE id=1;",
        read);
    if (!status) return status;
    const int rc = sqlite3_step(read.get());
    if (rc != SQLITE_ROW || sqlite3_column_type(read.get(), 0) != SQLITE_INTEGER ||
        sqlite3_column_type(read.get(), 1) != SQLITE_INTEGER) {
        return {StorageError::schema_invalid,
                "layer stack mutation could not read project revision/frozen state"};
    }
    const sqlite3_int64 raw_revision = sqlite3_column_int64(read.get(), 0);
    const sqlite3_int64 raw_frozen = sqlite3_column_int64(read.get(), 1);
    if (raw_revision < 0 || (raw_frozen != 0 && raw_frozen != 1) ||
        sqlite3_step(read.get()) != SQLITE_DONE) {
        return {StorageError::schema_invalid,
                "layer stack mutation found noncanonical project metadata"};
    }
    if (raw_revision >= std::numeric_limits<sqlite3_int64>::max()) {
        return {StorageError::schema_invalid,
                "project revision exhausted signed SQLite range during layer stack mutation"};
    }

    detail::StmtPtr write;
    status = detail::prepare(
        db,
        "UPDATE aeris_meta SET revision=?,modified_utc=?,frozen=? "
        "WHERE id=1 AND project_uuid=?;",
        write);
    if (status) status = detail::bind_int64(db, write.get(), 1, raw_revision + 1);
    if (status) status = detail::bind_text(db, write.get(), 2, std::string(modified_utc));
    if (status) status = detail::bind_int64(
        db, write.get(), 3, invalidate_frozen ? 0 : raw_frozen);
    if (status) status = detail::bind_text(
        db, write.get(), 4, project.metadata().project_uuid);
    if (status) status = detail::step_done(db, write.get());
    if (status && sqlite3_changes(db) != 1) {
        return {StorageError::schema_invalid,
                "layer stack mutation did not advance exactly one project metadata row"};
    }
    return status;
}

}  // namespace

LayerMutationResult initialize_layer_stack(
    ProjectStore& project,
    const std::vector<LayerCreateRequest>& input,
    const std::string_view modified_utc
) {
    if (!is_canonical_utc_timestamp(modified_utc)) {
        return {{StorageError::invalid_argument,
                 "layer stack initialization timestamp is not canonical Gregorian UTC"},
                false, false};
    }

    std::vector<LayerCreateRequest> layers;
    Status status = canonicalize_stack(input, layers);
    if (!status) return {std::move(status), false, false};

    detail::DbPtr db;
    status = detail::open_database(project.path(), SQLITE_OPEN_READWRITE, db);
    if (!status) return {std::move(status), false, false};
    status = validate_project_connection(db.get(), project);
    if (!status) return {std::move(status), false, false};
    status = detail::configure_durable(db.get());
    if (!status) return {std::move(status), false, false};
    status = detail::begin_immediate(db.get());
    if (!status) return {std::move(status), false, false};

    std::vector<ProjectLayerRecord> existing;
    status = load_existing_stack(db.get(), existing);
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false};
    }
    if (!existing.empty()) {
        const bool exact_retry = equal_stack(existing, layers);
        detail::rollback(db.get());
        status = project.refresh_metadata();
        if (!status) return {std::move(status), false, false};
        if (exact_retry) return {Status::success(), false, false};
        return {{StorageError::record_exists,
                 "project already contains a different non-empty layer stack"},
                false, false};
    }

    bool external_required = false;
    for (std::size_t index = 0U; index < layers.size(); ++index) {
        const LayerCreateRequest& request = layers[index];
        detail::StmtPtr layer_stmt;
        status = detail::prepare(
            db.get(),
            "INSERT INTO aeris_layer(layer_id,role_id,name,ordinal,visible) "
            "VALUES(?,?,?,?,?);",
            layer_stmt);
        if (status) status = detail::bind_text(
            db.get(), layer_stmt.get(), 1, request.layer_id);
        if (status) status = detail::bind_text(
            db.get(), layer_stmt.get(), 2, request.role_id);
        if (status) status = detail::bind_text(
            db.get(), layer_stmt.get(), 3, request.name);
        if (status) status = detail::bind_int64(
            db.get(), layer_stmt.get(), 4, static_cast<sqlite3_int64>(index));
        if (status) status = detail::bind_int64(
            db.get(), layer_stmt.get(), 5, request.visible ? 1 : 0);
        if (status) status = detail::step_done(db.get(), layer_stmt.get());
        if (status) status = insert_bindings(db.get(), request, external_required);
        if (!status) {
            detail::rollback(db.get());
            return {std::move(status), false, false};
        }
    }

    status = advance_revision(db.get(), project, modified_utc, external_required);
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false};
    }
    status = detail::commit(db.get());
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false};
    }

    status = project.refresh_metadata();
    if (!status) return {std::move(status), true, true};
    return {Status::success(), true, true};
}

}  // namespace aeris::storage
