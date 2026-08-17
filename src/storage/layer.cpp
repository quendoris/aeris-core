// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#include "aeris/storage/layer.hpp"

#include "layer_detail.hpp"
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
constexpr sqlite3_int64 kTemporaryOrdinalOffset = 1'000'000;

[[nodiscard]] bool bounded_text(
    const std::string& value,
    const std::size_t max_bytes,
    const bool allow_empty = false) noexcept {
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

[[nodiscard]] Status validate_request(LayerCreateRequest& request) {
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

    std::sort(request.sources.begin(), request.sources.end(),
              [](const LayerSourceBinding& a, const LayerSourceBinding& b) {
                  return a.slot_id < b.slot_id;
              });
    std::sort(request.resources.begin(), request.resources.end(),
              [](const LayerResourceBinding& a, const LayerResourceBinding& b) {
                  return a.slot_id < b.slot_id;
              });
    return Status::success();
}

[[nodiscard]] Status validate_project_connection(sqlite3* db, const ProjectStore& project) {
    Status status = detail::verify_quick_check(db);
    if (!status) return status;

    sqlite3_int64 application_id = 0;
    if (!(status = detail::query_single_int(db, "PRAGMA application_id;", application_id))) return status;
    if (application_id != static_cast<sqlite3_int64>(kProjectApplicationId)) {
        return {StorageError::invalid_application_id,
                "layer target is not an AERIS project"};
    }

    sqlite3_int64 generation = 0;
    if (!(status = detail::query_single_int(db, "PRAGMA user_version;", generation))) return status;
    if (generation != kProjectSchemaGeneration) {
        return {StorageError::unsupported_schema,
                "layer target has unsupported draft schema generation"};
    }

    std::string uuid;
    if (!(status = detail::query_single_text(
              db, "SELECT project_uuid FROM aeris_meta WHERE id=1;", uuid))) {
        return status;
    }
    if (uuid != project.metadata().project_uuid) {
        return {StorageError::schema_invalid,
                "layer target UUID differs from validated project handle"};
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
    bool& exists) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(db, sql, stmt);
    if (!status) return status;
    if (!(status = detail::bind_text(db, stmt.get(), 1, id))) return status;
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) {
        exists = false;
        return Status::success();
    }
    if (rc != SQLITE_ROW || sqlite3_column_type(stmt.get(), 0) != SQLITE_INTEGER) {
        return {StorageError::schema_invalid,
                "layer reference existence probe returned malformed data"};
    }
    exists = true;
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return {StorageError::schema_invalid,
                "layer reference existence probe returned duplicate rows"};
    }
    return Status::success();
}

[[nodiscard]] Status load_source_bindings(
    sqlite3* db,
    const std::string& layer_id,
    std::vector<LayerSourceBinding>& bindings) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(
        db,
        "SELECT b.slot_id,b.source_id,EXISTS(SELECT 1 FROM aeris_source s WHERE s.source_id=b.source_id) "
        "FROM aeris_layer_source b WHERE b.layer_id=? ORDER BY b.slot_id;",
        stmt);
    if (!status) return status;
    if (!(status = detail::bind_text(db, stmt.get(), 1, layer_id))) return status;

    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW || sqlite3_column_type(stmt.get(), 0) != SQLITE_TEXT ||
            sqlite3_column_type(stmt.get(), 1) != SQLITE_TEXT ||
            sqlite3_column_type(stmt.get(), 2) != SQLITE_INTEGER) {
            return {StorageError::schema_invalid,
                    "stored layer source binding has malformed SQLite types"};
        }
        LayerSourceBinding binding{text_column(stmt.get(), 0), text_column(stmt.get(), 1)};
        if (sqlite3_column_int(stmt.get(), 2) != 1 ||
            !bounded_text(binding.slot_id, kMaxIdentifierBytes) ||
            !bounded_text(binding.source_id, kMaxIdentifierBytes)) {
            return {StorageError::schema_invalid,
                    "stored layer source binding is noncanonical or orphaned"};
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
    std::vector<LayerResourceBinding>& bindings) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(
        db,
        "SELECT b.slot_id,b.resource_id,r.required_for_reproduction "
        "FROM aeris_layer_resource b LEFT JOIN aeris_resource r ON r.resource_id=b.resource_id "
        "WHERE b.layer_id=? ORDER BY b.slot_id;",
        stmt);
    if (!status) return status;
    if (!(status = detail::bind_text(db, stmt.get(), 1, layer_id))) return status;

    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW || sqlite3_column_type(stmt.get(), 0) != SQLITE_TEXT ||
            sqlite3_column_type(stmt.get(), 1) != SQLITE_TEXT ||
            sqlite3_column_type(stmt.get(), 2) != SQLITE_INTEGER) {
            return {StorageError::schema_invalid,
                    "stored layer resource binding is malformed or orphaned"};
        }
        LayerResourceBinding binding{text_column(stmt.get(), 0), text_column(stmt.get(), 1)};
        if (sqlite3_column_int(stmt.get(), 2) != 1 ||
            !bounded_text(binding.slot_id, kMaxIdentifierBytes) ||
            !bounded_text(binding.resource_id, kMaxIdentifierBytes)) {
            return {StorageError::schema_invalid,
                    "layer-bound resource is not marked required-for-reproduction"};
        }
        bindings.push_back(std::move(binding));
        if (bindings.size() > kMaxLayerBindings) {
            return {StorageError::schema_invalid,
                    "stored layer exceeds resource binding bound"};
        }
    }
    return Status::success();
}

[[nodiscard]] Status load_layers(sqlite3* db, std::vector<ProjectLayerRecord>& records) {
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
                    "stored project layer has malformed SQLite types"};
        }

        const sqlite3_int64 ordinal = sqlite3_column_int64(stmt.get(), 3);
        const sqlite3_int64 visible = sqlite3_column_int64(stmt.get(), 4);
        ProjectLayerRecord record{};
        record.layer_id = text_column(stmt.get(), 0);
        record.role_id = text_column(stmt.get(), 1);
        record.name = text_column(stmt.get(), 2);
        if (ordinal < 0 || static_cast<std::uint64_t>(ordinal) != expected_ordinal ||
            expected_ordinal >= kMaxProjectLayers || (visible != 0 && visible != 1) ||
            !bounded_text(record.layer_id, kMaxIdentifierBytes) ||
            !bounded_text(record.role_id, kMaxIdentifierBytes) ||
            !bounded_text(record.name, kMaxLayerNameBytes)) {
            return {StorageError::schema_invalid,
                    "stored project layer violates canonical ordering or field bounds"};
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

[[nodiscard]] bool equal_source_binding(
    const LayerSourceBinding& a,
    const LayerSourceBinding& b) noexcept {
    return a.slot_id == b.slot_id && a.source_id == b.source_id;
}

[[nodiscard]] bool equal_resource_binding(
    const LayerResourceBinding& a,
    const LayerResourceBinding& b) noexcept {
    return a.slot_id == b.slot_id && a.resource_id == b.resource_id;
}

[[nodiscard]] bool equal_existing(
    const ProjectLayerRecord& existing,
    const LayerCreateRequest& request) noexcept {
    if (existing.layer_id != request.layer_id || existing.role_id != request.role_id ||
        existing.name != request.name || existing.visible != request.visible ||
        existing.sources.size() != request.sources.size() ||
        existing.resources.size() != request.resources.size()) {
        return false;
    }
    for (std::size_t i = 0U; i < existing.sources.size(); ++i) {
        if (!equal_source_binding(existing.sources[i], request.sources[i])) return false;
    }
    for (std::size_t i = 0U; i < existing.resources.size(); ++i) {
        if (!equal_resource_binding(existing.resources[i], request.resources[i])) return false;
    }
    return true;
}

[[nodiscard]] Status read_revision_frozen(
    sqlite3* db,
    std::uint64_t& revision,
    bool& frozen) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(db, "SELECT revision,frozen FROM aeris_meta WHERE id=1;", stmt);
    if (!status) return status;
    const int rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_ROW || sqlite3_column_type(stmt.get(), 0) != SQLITE_INTEGER ||
        sqlite3_column_type(stmt.get(), 1) != SQLITE_INTEGER) {
        return {StorageError::schema_invalid,
                "layer mutation could not read canonical project revision/frozen state"};
    }
    const sqlite3_int64 raw_revision = sqlite3_column_int64(stmt.get(), 0);
    const sqlite3_int64 raw_frozen = sqlite3_column_int64(stmt.get(), 1);
    if (raw_revision < 0 || raw_frozen < 0 || raw_frozen > 1) {
        return {StorageError::schema_invalid,
                "layer mutation found invalid project revision/frozen state"};
    }
    revision = static_cast<std::uint64_t>(raw_revision);
    frozen = raw_frozen == 1;
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return {StorageError::schema_invalid,
                "aeris_meta is not a singleton during layer mutation"};
    }
    return Status::success();
}

[[nodiscard]] Status advance_revision(
    sqlite3* db,
    const ProjectStore& project,
    const std::string_view modified_utc,
    const std::optional<bool> frozen_override = std::nullopt) {
    std::uint64_t revision = 0U;
    bool frozen = false;
    Status status = read_revision_frozen(db, revision, frozen);
    if (!status) return status;
    if (revision >= static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max())) {
        return {StorageError::schema_invalid,
                "project revision exhausted signed SQLite range during layer mutation"};
    }

    detail::StmtPtr stmt;
    status = detail::prepare(
        db,
        "UPDATE aeris_meta SET revision=?,modified_utc=?,frozen=? WHERE id=1 AND project_uuid=?;",
        stmt);
    if (status) status = detail::bind_int64(
        db, stmt.get(), 1, static_cast<sqlite3_int64>(revision + 1U));
    if (status) status = detail::bind_text(db, stmt.get(), 2, std::string(modified_utc));
    if (status) status = detail::bind_int64(
        db, stmt.get(), 3, frozen_override.value_or(frozen) ? 1 : 0);
    if (status) status = detail::bind_text(db, stmt.get(), 4, project.metadata().project_uuid);
    if (status) status = detail::step_done(db, stmt.get());
    if (status && sqlite3_changes(db) != 1) {
        return {StorageError::schema_invalid,
                "layer mutation did not advance exactly one project metadata row"};
    }
    return status;
}

[[nodiscard]] Status promote_resource_requirement(
    sqlite3* db,
    const std::string& resource_id,
    bool& external_required) {
    detail::StmtPtr read;
    Status status = detail::prepare(
        db,
        "SELECT storage_mode,required_for_reproduction FROM aeris_resource WHERE resource_id=?;",
        read);
    if (!status) return status;
    if (!(status = detail::bind_text(db, read.get(), 1, resource_id))) return status;
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
    bool& external_required) {
    for (const LayerSourceBinding& binding : request.sources) {
        bool exists = false;
        Status status = row_exists(
            db, "SELECT 1 FROM aeris_source WHERE source_id=?;", binding.source_id, exists);
        if (!status) return status;
        if (!exists) {
            return {StorageError::record_not_found,
                    "layer source binding references missing project source: " + binding.source_id};
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

[[nodiscard]] Status rewrite_order(
    sqlite3* db,
    const std::vector<std::string>& ordered_layer_ids) {
    Status status = detail::exec(
        db, "UPDATE aeris_layer SET ordinal=ordinal+1000000;");
    if (!status) return status;

    detail::StmtPtr stmt;
    status = detail::prepare(
        db, "UPDATE aeris_layer SET ordinal=? WHERE layer_id=?;", stmt);
    if (!status) return status;

    for (std::size_t index = 0U; index < ordered_layer_ids.size(); ++index) {
        if (sqlite3_reset(stmt.get()) != SQLITE_OK || sqlite3_clear_bindings(stmt.get()) != SQLITE_OK) {
            return {StorageError::sqlite_failure,
                    detail::sqlite_message(db, "could not reset layer-order update statement")};
        }
        status = detail::bind_int64(
            db, stmt.get(), 1, static_cast<sqlite3_int64>(index));
        if (status) status = detail::bind_text(db, stmt.get(), 2, ordered_layer_ids[index]);
        if (status) status = detail::step_done(db, stmt.get());
        if (!status) return status;
        if (sqlite3_changes(db) != 1) {
            return {StorageError::schema_invalid,
                    "layer-order rewrite did not update exactly one layer"};
        }
    }
    return Status::success();
}

[[nodiscard]] Status prepare_layer_db(ProjectStore& project, detail::DbPtr& db) {
    Status status = detail::open_database(project.path(), SQLITE_OPEN_READWRITE, db);
    if (!status) return status;
    if (!(status = validate_project_connection(db.get(), project))) return status;
    return detail::configure_durable(db.get());
}

}  // namespace

LayerMutationResult append_layer(
    ProjectStore& project,
    const LayerCreateRequest& input,
    const std::string_view modified_utc) {
    if (!is_canonical_utc_timestamp(modified_utc)) {
        return {{StorageError::invalid_argument,
                 "layer mutation timestamp is not canonical Gregorian UTC"},
                false, false};
    }
    LayerCreateRequest request = input;
    Status status = validate_request(request);
    if (!status) return {std::move(status), false, false};

    detail::DbPtr db;
    status = prepare_layer_db(project, db);
    if (!status) return {std::move(status), false, false};
    if (!(status = detail::begin_immediate(db.get()))) {
        return {std::move(status), false, false};
    }

    std::vector<ProjectLayerRecord> existing_layers;
    status = load_layers(db.get(), existing_layers);
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false};
    }
    for (const ProjectLayerRecord& layer : existing_layers) {
        if (layer.layer_id == request.layer_id) {
            detail::rollback(db.get());
            status = project.refresh_metadata();
            if (!status) return {std::move(status), false, false};
            if (equal_existing(layer, request)) return {Status::success(), false, false};
            return {{StorageError::record_exists,
                     "layer ID already exists with different immutable/configured content"},
                    false, false};
        }
    }
    if (existing_layers.size() >= kMaxProjectLayers) {
        detail::rollback(db.get());
        return {{StorageError::invalid_argument,
                 "project reached the 65535-layer draft bound"},
                false, false};
    }

    detail::StmtPtr layer_stmt;
    status = detail::prepare(
        db.get(),
        "INSERT INTO aeris_layer(layer_id,role_id,name,ordinal,visible) VALUES(?,?,?,?,?);",
        layer_stmt);
    if (status) status = detail::bind_text(db.get(), layer_stmt.get(), 1, request.layer_id);
    if (status) status = detail::bind_text(db.get(), layer_stmt.get(), 2, request.role_id);
    if (status) status = detail::bind_text(db.get(), layer_stmt.get(), 3, request.name);
    if (status) status = detail::bind_int64(
        db.get(), layer_stmt.get(), 4, static_cast<sqlite3_int64>(existing_layers.size()));
    if (status) status = detail::bind_int64(db.get(), layer_stmt.get(), 5, request.visible ? 1 : 0);
    if (status) status = detail::step_done(db.get(), layer_stmt.get());

    bool external_required = false;
    if (status) status = insert_bindings(db.get(), request, external_required);
    if (status) status = advance_revision(
        db.get(), project, modified_utc,
        external_required ? std::optional<bool>(false) : std::nullopt);
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

LayerMutationResult update_layer_state(
    ProjectStore& project,
    const std::string_view layer_id,
    const LayerStateUpdate& update) {
    if (layer_id.empty() || layer_id.size() > kMaxIdentifierBytes ||
        layer_id.find('\0') != std::string_view::npos ||
        !is_canonical_utc_timestamp(update.modified_utc) ||
        (!update.name.has_value() && !update.visible.has_value())) {
        return {{StorageError::invalid_argument,
                 "layer state update requires canonical ID/time and at least one field"},
                false, false};
    }
    if (update.name.has_value() && !bounded_text(*update.name, kMaxLayerNameBytes)) {
        return {{StorageError::invalid_argument,
                 "layer name is empty, contains NUL, or exceeds 1024 bytes"},
                false, false};
    }

    detail::DbPtr db;
    Status status = prepare_layer_db(project, db);
    if (!status) return {std::move(status), false, false};
    if (!(status = detail::begin_immediate(db.get()))) return {std::move(status), false, false};

    detail::StmtPtr read;
    status = detail::prepare(
        db.get(), "SELECT name,visible FROM aeris_layer WHERE layer_id=?;", read);
    if (status) status = detail::bind_text(db.get(), read.get(), 1, std::string(layer_id));
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false};
    }
    const int rc = sqlite3_step(read.get());
    if (rc == SQLITE_DONE) {
        detail::rollback(db.get());
        return {{StorageError::record_not_found, "layer ID was not found"}, false, false};
    }
    if (rc != SQLITE_ROW || sqlite3_column_type(read.get(), 0) != SQLITE_TEXT ||
        sqlite3_column_type(read.get(), 1) != SQLITE_INTEGER) {
        detail::rollback(db.get());
        return {{StorageError::schema_invalid, "stored layer state is malformed"}, false, false};
    }
    const std::string current_name = text_column(read.get(), 0);
    const sqlite3_int64 current_visible = sqlite3_column_int64(read.get(), 1);
    if ((current_visible != 0 && current_visible != 1) || sqlite3_step(read.get()) != SQLITE_DONE) {
        detail::rollback(db.get());
        return {{StorageError::schema_invalid, "stored layer state is noncanonical"}, false, false};
    }

    const std::string next_name = update.name.value_or(current_name);
    const bool next_visible = update.visible.value_or(current_visible == 1);
    if (next_name == current_name && next_visible == (current_visible == 1)) {
        detail::rollback(db.get());
        status = project.refresh_metadata();
        if (!status) return {std::move(status), false, false};
        return {Status::success(), false, false};
    }

    detail::StmtPtr write;
    status = detail::prepare(
        db.get(), "UPDATE aeris_layer SET name=?,visible=? WHERE layer_id=?;", write);
    if (status) status = detail::bind_text(db.get(), write.get(), 1, next_name);
    if (status) status = detail::bind_int64(db.get(), write.get(), 2, next_visible ? 1 : 0);
    if (status) status = detail::bind_text(db.get(), write.get(), 3, std::string(layer_id));
    if (status) status = detail::step_done(db.get(), write.get());
    if (status && sqlite3_changes(db.get()) != 1) {
        status = {StorageError::schema_invalid,
                  "layer state update did not affect exactly one row"};
    }
    if (status) status = advance_revision(db.get(), project, update.modified_utc);
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

LayerMutationResult set_layer_order(
    ProjectStore& project,
    const std::vector<std::string>& ordered_layer_ids,
    const std::string_view modified_utc) {
    if (!is_canonical_utc_timestamp(modified_utc) ||
        ordered_layer_ids.size() > kMaxProjectLayers) {
        return {{StorageError::invalid_argument,
                 "layer reorder has invalid timestamp or exceeds layer bound"},
                false, false};
    }
    std::set<std::string> requested;
    for (const std::string& id : ordered_layer_ids) {
        if (!bounded_text(id, kMaxIdentifierBytes) || !requested.insert(id).second) {
            return {{StorageError::invalid_argument,
                     "layer reorder IDs must be unique canonical identifiers"},
                    false, false};
        }
    }

    detail::DbPtr db;
    Status status = prepare_layer_db(project, db);
    if (!status) return {std::move(status), false, false};
    if (!(status = detail::begin_immediate(db.get()))) return {std::move(status), false, false};

    std::vector<ProjectLayerRecord> layers;
    status = load_layers(db.get(), layers);
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false};
    }
    if (layers.size() != ordered_layer_ids.size()) {
        detail::rollback(db.get());
        return {{StorageError::invalid_argument,
                 "layer reorder must contain every current layer exactly once"},
                false, false};
    }

    std::vector<std::string> current;
    current.reserve(layers.size());
    for (const ProjectLayerRecord& layer : layers) {
        current.push_back(layer.layer_id);
        if (requested.find(layer.layer_id) == requested.end()) {
            detail::rollback(db.get());
            return {{StorageError::invalid_argument,
                     "layer reorder contains an unknown or omits an existing layer"},
                    false, false};
        }
    }
    if (current == ordered_layer_ids) {
        detail::rollback(db.get());
        status = project.refresh_metadata();
        if (!status) return {std::move(status), false, false};
        return {Status::success(), false, false};
    }

    status = rewrite_order(db.get(), ordered_layer_ids);
    if (status) status = advance_revision(db.get(), project, modified_utc);
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

LayerMutationResult remove_layer(
    ProjectStore& project,
    const std::string_view layer_id,
    const std::string_view modified_utc) {
    if (layer_id.empty() || layer_id.size() > kMaxIdentifierBytes ||
        layer_id.find('\0') != std::string_view::npos ||
        !is_canonical_utc_timestamp(modified_utc)) {
        return {{StorageError::invalid_argument,
                 "layer removal requires canonical ID and mutation timestamp"},
                false, false};
    }

    detail::DbPtr db;
    Status status = prepare_layer_db(project, db);
    if (!status) return {std::move(status), false, false};
    if (!(status = detail::begin_immediate(db.get()))) return {std::move(status), false, false};

    std::vector<ProjectLayerRecord> layers;
    status = load_layers(db.get(), layers);
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false};
    }

    const auto found = std::find_if(
        layers.begin(), layers.end(), [&](const ProjectLayerRecord& layer) {
            return layer.layer_id == layer_id;
        });
    if (found == layers.end()) {
        detail::rollback(db.get());
        return {{StorageError::record_not_found, "layer ID was not found"}, false, false};
    }

    detail::StmtPtr erase;
    status = detail::prepare(db.get(), "DELETE FROM aeris_layer WHERE layer_id=?;", erase);
    if (status) status = detail::bind_text(db.get(), erase.get(), 1, std::string(layer_id));
    if (status) status = detail::step_done(db.get(), erase.get());
    if (status && sqlite3_changes(db.get()) != 1) {
        status = {StorageError::schema_invalid,
                  "layer removal did not delete exactly one row"};
    }
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false};
    }

    std::vector<std::string> remaining;
    remaining.reserve(layers.size() - 1U);
    for (const ProjectLayerRecord& layer : layers) {
        if (layer.layer_id != layer_id) remaining.push_back(layer.layer_id);
    }
    status = rewrite_order(db.get(), remaining);
    if (status) status = advance_revision(db.get(), project, modified_utc);
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

ProjectLayerListResult list_project_layers(const ProjectStore& project) {
    detail::DbPtr db;
    Status status = detail::open_database(project.path(), SQLITE_OPEN_READONLY, db);
    if (!status) return {std::move(status), {}};
    if (!(status = validate_project_connection(db.get(), project))) {
        return {std::move(status), {}};
    }

    ProjectLayerListResult result{};
    status = load_layers(db.get(), result.records);
    result.status = std::move(status);
    if (!result.status) result.records.clear();
    return result;
}

namespace detail {

Status verify_layer_semantics(const ProjectStore& project) {
    const ProjectLayerListResult result = list_project_layers(project);
    return result.status;
}

}  // namespace detail
}  // namespace aeris::storage
