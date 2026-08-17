// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#include "aeris/storage/style.hpp"

#include "sqlite_detail.hpp"
#include "style_detail.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <utility>

#include <sqlite3.h>

namespace aeris::storage {
namespace {

static_assert(sizeof(double) == 8U, "AERIS style f64 requires 64-bit double");
static_assert(std::numeric_limits<double>::is_iec559,
              "AERIS style f64 requires IEC 60559 / IEEE-754 binary64");

constexpr std::size_t kMaxIdentifierBytes = 255U;
constexpr std::size_t kMaxStyleNameBytes = 1024U;

[[nodiscard]] bool bounded_text(
    const std::string& value,
    const std::size_t max_bytes,
    const bool allow_empty = false) noexcept {
    return (allow_empty || !value.empty()) && value.size() <= max_bytes &&
           value.find('\0') == std::string::npos;
}

[[nodiscard]] std::string text_column(sqlite3_stmt* stmt, const int column) {
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, column));
    const int bytes = sqlite3_column_bytes(stmt, column);
    return std::string(text, static_cast<std::size_t>(bytes));
}

[[nodiscard]] bool valid_utf8(const std::vector<std::uint8_t>& bytes) noexcept {
    std::size_t i = 0U;
    while (i < bytes.size()) {
        const std::uint8_t lead = bytes[i++];
        if (lead == 0U) return false;
        if (lead <= 0x7fU) continue;

        std::uint32_t codepoint = 0U;
        std::size_t continuation = 0U;
        if (lead >= 0xc2U && lead <= 0xdfU) {
            codepoint = static_cast<std::uint32_t>(lead & 0x1fU);
            continuation = 1U;
        } else if (lead >= 0xe0U && lead <= 0xefU) {
            codepoint = static_cast<std::uint32_t>(lead & 0x0fU);
            continuation = 2U;
        } else if (lead >= 0xf0U && lead <= 0xf4U) {
            codepoint = static_cast<std::uint32_t>(lead & 0x07U);
            continuation = 3U;
        } else {
            return false;
        }
        if (i + continuation > bytes.size()) return false;
        for (std::size_t n = 0U; n < continuation; ++n) {
            const std::uint8_t c = bytes[i++];
            if ((c & 0xc0U) != 0x80U) return false;
            codepoint = (codepoint << 6U) | static_cast<std::uint32_t>(c & 0x3fU);
        }

        if ((continuation == 1U && codepoint < 0x80U) ||
            (continuation == 2U && codepoint < 0x800U) ||
            (continuation == 3U && codepoint < 0x10000U) ||
            codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] double decode_f64le(const std::vector<std::uint8_t>& bytes) noexcept {
    std::uint64_t bits = 0U;
    for (unsigned i = 0U; i < 8U; ++i) {
        bits |= static_cast<std::uint64_t>(bytes[i]) << (8U * i);
    }
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

[[nodiscard]] bool canonical_f64_payload(const std::vector<std::uint8_t>& bytes) noexcept {
    if (bytes.size() != 8U) return false;
    const double value = decode_f64le(bytes);
    if (!std::isfinite(value)) return false;
    // AERIS canonical binary64 never persists negative zero.
    return !(value == 0.0 && bytes[7] == 0x80U &&
             std::all_of(bytes.begin(), bytes.begin() + 7,
                         [](const std::uint8_t byte) { return byte == 0U; }));
}

[[nodiscard]] Status validate_property(const StylePropertyRecord& property) {
    if (!bounded_text(property.property_key, kMaxIdentifierBytes) ||
        !bounded_text(property.value_type_id, kMaxIdentifierBytes)) {
        return {StorageError::invalid_argument,
                "style property key/type ID violates canonical identifier bounds"};
    }
    if (property.value.size() > kMaxStylePropertyBytes ||
        property.value.size() > static_cast<std::size_t>(INT_MAX)) {
        return {StorageError::invalid_argument,
                "style property payload exceeds the 1 MiB defensive draft bound"};
    }

    const std::string_view type(property.value_type_id);
    if (type == kStyleValueBoolV1) {
        if (property.value.size() != 1U || property.value.front() > 1U) {
            return {StorageError::invalid_argument,
                    "aeris.style.value.bool.v1 requires exactly one canonical 0/1 byte"};
        }
    } else if (type == kStyleValueI64LeV1) {
        if (property.value.size() != 8U) {
            return {StorageError::invalid_argument,
                    "aeris.style.value.i64le.v1 requires exactly eight bytes"};
        }
    } else if (type == kStyleValueF64LeV1) {
        if (!canonical_f64_payload(property.value)) {
            return {StorageError::invalid_argument,
                    "aeris.style.value.f64le.v1 requires finite canonical little-endian binary64"};
        }
    } else if (type == kStyleValueUtf8V1) {
        if (!valid_utf8(property.value)) {
            return {StorageError::invalid_argument,
                    "aeris.style.value.utf8.v1 requires well-formed NUL-free UTF-8"};
        }
    } else if (type == kStyleValueRgba8V1) {
        if (property.value.size() != 4U) {
            return {StorageError::invalid_argument,
                    "aeris.style.value.rgba8.v1 requires exactly RGBA four bytes"};
        }
    }
    // Unknown bounded type IDs deliberately remain opaque. Their versioned type
    // contract owns canonical byte meaning; storage preserves bytes verbatim.
    return Status::success();
}

[[nodiscard]] Status canonicalize_and_validate_style(ProjectStyleRecord& record) {
    if (!bounded_text(record.style_id, kMaxIdentifierBytes) ||
        !bounded_text(record.model_id, kMaxIdentifierBytes) ||
        !bounded_text(record.name, kMaxStyleNameBytes)) {
        return {StorageError::invalid_argument,
                "style ID/model/name violates canonical storage bounds"};
    }
    if (record.parent_style_id.has_value() &&
        !bounded_text(*record.parent_style_id, kMaxIdentifierBytes)) {
        return {StorageError::invalid_argument,
                "parent style ID violates canonical identifier bounds"};
    }
    if (record.parent_style_id.has_value() && *record.parent_style_id == record.style_id) {
        return {StorageError::invalid_argument, "style cannot inherit from itself"};
    }
    if (record.properties.size() > kMaxStyleProperties ||
        record.resources.size() > kMaxStyleResourceBindings) {
        return {StorageError::invalid_argument,
                "style exceeds property/resource defensive draft bounds"};
    }

    std::set<std::string> property_keys;
    for (const StylePropertyRecord& property : record.properties) {
        Status status = validate_property(property);
        if (!status) return status;
        if (!property_keys.insert(property.property_key).second) {
            return {StorageError::invalid_argument,
                    "style contains duplicate property key"};
        }
    }

    std::set<std::string> resource_slots;
    for (const StyleResourceBinding& binding : record.resources) {
        if (!bounded_text(binding.slot_id, kMaxIdentifierBytes) ||
            !bounded_text(binding.resource_id, kMaxIdentifierBytes)) {
            return {StorageError::invalid_argument,
                    "style resource binding violates canonical identifier bounds"};
        }
        if (!resource_slots.insert(binding.slot_id).second) {
            return {StorageError::invalid_argument,
                    "style contains duplicate resource slot"};
        }
    }

    std::sort(record.properties.begin(), record.properties.end(),
              [](const StylePropertyRecord& a, const StylePropertyRecord& b) {
                  return a.property_key < b.property_key;
              });
    std::sort(record.resources.begin(), record.resources.end(),
              [](const StyleResourceBinding& a, const StyleResourceBinding& b) {
                  return a.slot_id < b.slot_id;
              });
    return Status::success();
}

[[nodiscard]] Status canonicalize_layer_style_bindings(
    std::vector<LayerStyleBinding>& bindings) {
    if (bindings.size() > kMaxLayerStyleBindings) {
        return {StorageError::invalid_argument,
                "layer exceeds the 256 style-binding draft bound"};
    }
    std::set<std::string> slots;
    for (const LayerStyleBinding& binding : bindings) {
        if (!bounded_text(binding.slot_id, kMaxIdentifierBytes) ||
            !bounded_text(binding.style_id, kMaxIdentifierBytes)) {
            return {StorageError::invalid_argument,
                    "layer style binding violates canonical identifier bounds"};
        }
        if (!slots.insert(binding.slot_id).second) {
            return {StorageError::invalid_argument,
                    "layer contains duplicate style slot"};
        }
    }
    std::sort(bindings.begin(), bindings.end(),
              [](const LayerStyleBinding& a, const LayerStyleBinding& b) {
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
        return {StorageError::invalid_application_id, "style target is not an AERIS project"};
    }
    sqlite3_int64 generation = 0;
    if (!(status = detail::query_single_int(db, "PRAGMA user_version;", generation))) return status;
    if (generation != kProjectSchemaGeneration) {
        return {StorageError::unsupported_schema,
                "style target has unsupported draft schema generation"};
    }
    std::string uuid;
    if (!(status = detail::query_single_text(
              db, "SELECT project_uuid FROM aeris_meta WHERE id=1;", uuid))) return status;
    if (uuid != project.metadata().project_uuid) {
        return {StorageError::schema_invalid,
                "style target UUID differs from validated project handle"};
    }
    return Status::success();
}

[[nodiscard]] Status bind_blob(
    sqlite3* db,
    sqlite3_stmt* stmt,
    const int index,
    const std::vector<std::uint8_t>& value) {
    const void* data = value.empty() ? nullptr : value.data();
    if (sqlite3_bind_blob(stmt, index, data, static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
        return {StorageError::sqlite_failure,
                detail::sqlite_message(db, "sqlite style BLOB bind failed")};
    }
    return Status::success();
}

[[nodiscard]] Status load_style_properties(
    sqlite3* db,
    const std::string& style_id,
    std::vector<StylePropertyRecord>& properties) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(
        db,
        "SELECT property_key,value_type_id,value_payload FROM aeris_style_property "
        "WHERE style_id=? ORDER BY property_key;",
        stmt);
    if (!status) return status;
    if (!(status = detail::bind_text(db, stmt.get(), 1, style_id))) return status;

    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW || sqlite3_column_type(stmt.get(), 0) != SQLITE_TEXT ||
            sqlite3_column_type(stmt.get(), 1) != SQLITE_TEXT ||
            sqlite3_column_type(stmt.get(), 2) != SQLITE_BLOB) {
            return {StorageError::schema_invalid,
                    "stored style property has malformed SQLite types"};
        }
        StylePropertyRecord property{};
        property.property_key = text_column(stmt.get(), 0);
        property.value_type_id = text_column(stmt.get(), 1);
        const int bytes = sqlite3_column_bytes(stmt.get(), 2);
        const auto* blob = static_cast<const std::uint8_t*>(sqlite3_column_blob(stmt.get(), 2));
        if (bytes < 0 || static_cast<std::size_t>(bytes) > kMaxStylePropertyBytes ||
            (bytes > 0 && blob == nullptr)) {
            return {StorageError::schema_invalid,
                    "stored style property payload violates canonical bounds"};
        }
        if (bytes > 0) property.value.assign(blob, blob + bytes);
        status = validate_property(property);
        if (!status) {
            return {StorageError::schema_invalid,
                    "stored style property is noncanonical: " + status.diagnostic};
        }
        properties.push_back(std::move(property));
        if (properties.size() > kMaxStyleProperties) {
            return {StorageError::schema_invalid,
                    "stored style exceeds property-count draft bound"};
        }
    }
    return Status::success();
}

[[nodiscard]] Status load_style_resources(
    sqlite3* db,
    const std::string& style_id,
    std::vector<StyleResourceBinding>& resources) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(
        db,
        "SELECT b.slot_id,b.resource_id,r.required_for_reproduction "
        "FROM aeris_style_resource b LEFT JOIN aeris_resource r ON r.resource_id=b.resource_id "
        "WHERE b.style_id=? ORDER BY b.slot_id;",
        stmt);
    if (!status) return status;
    if (!(status = detail::bind_text(db, stmt.get(), 1, style_id))) return status;
    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW || sqlite3_column_type(stmt.get(), 0) != SQLITE_TEXT ||
            sqlite3_column_type(stmt.get(), 1) != SQLITE_TEXT ||
            sqlite3_column_type(stmt.get(), 2) != SQLITE_INTEGER) {
            return {StorageError::schema_invalid,
                    "stored style resource binding is malformed or orphaned"};
        }
        StyleResourceBinding binding{text_column(stmt.get(), 0), text_column(stmt.get(), 1)};
        if (sqlite3_column_int(stmt.get(), 2) != 1 ||
            !bounded_text(binding.slot_id, kMaxIdentifierBytes) ||
            !bounded_text(binding.resource_id, kMaxIdentifierBytes)) {
            return {StorageError::schema_invalid,
                    "style-bound resource is noncanonical or not required-for-reproduction"};
        }
        resources.push_back(std::move(binding));
        if (resources.size() > kMaxStyleResourceBindings) {
            return {StorageError::schema_invalid,
                    "stored style exceeds resource-binding draft bound"};
        }
    }
    return Status::success();
}

[[nodiscard]] Status load_style(
    sqlite3* db,
    const std::string& style_id,
    std::optional<ProjectStyleRecord>& record) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(
        db,
        "SELECT style_id,model_id,name,parent_style_id FROM aeris_style WHERE style_id=?;",
        stmt);
    if (!status) return status;
    if (!(status = detail::bind_text(db, stmt.get(), 1, style_id))) return status;
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) {
        record.reset();
        return Status::success();
    }
    if (rc != SQLITE_ROW || sqlite3_column_type(stmt.get(), 0) != SQLITE_TEXT ||
        sqlite3_column_type(stmt.get(), 1) != SQLITE_TEXT ||
        sqlite3_column_type(stmt.get(), 2) != SQLITE_TEXT ||
        (sqlite3_column_type(stmt.get(), 3) != SQLITE_NULL &&
         sqlite3_column_type(stmt.get(), 3) != SQLITE_TEXT)) {
        return {StorageError::schema_invalid, "stored style has malformed SQLite types"};
    }

    ProjectStyleRecord loaded{};
    loaded.style_id = text_column(stmt.get(), 0);
    loaded.model_id = text_column(stmt.get(), 1);
    loaded.name = text_column(stmt.get(), 2);
    if (sqlite3_column_type(stmt.get(), 3) == SQLITE_TEXT) {
        loaded.parent_style_id = text_column(stmt.get(), 3);
    }
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return {StorageError::schema_invalid, "style ID resolves to more than one row"};
    }
    status = load_style_properties(db, loaded.style_id, loaded.properties);
    if (!status) return status;
    status = load_style_resources(db, loaded.style_id, loaded.resources);
    if (!status) return status;

    ProjectStyleRecord validation = loaded;
    status = canonicalize_and_validate_style(validation);
    if (!status) {
        return {StorageError::schema_invalid,
                "stored style violates canonical bounds: " + status.diagnostic};
    }
    record = std::move(loaded);
    return Status::success();
}

[[nodiscard]] Status load_all_styles(
    sqlite3* db,
    std::vector<ProjectStyleRecord>& records) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(db, "SELECT style_id FROM aeris_style ORDER BY style_id;", stmt);
    if (!status) return status;
    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW || sqlite3_column_type(stmt.get(), 0) != SQLITE_TEXT) {
            return {StorageError::schema_invalid, "style index contains malformed row"};
        }
        const std::string style_id = text_column(stmt.get(), 0);
        std::optional<ProjectStyleRecord> record;
        status = load_style(db, style_id, record);
        if (!status) return status;
        if (!record.has_value()) {
            return {StorageError::schema_invalid, "style index references missing style"};
        }
        records.push_back(std::move(*record));
        if (records.size() > kMaxProjectStyles) {
            return {StorageError::schema_invalid, "project exceeds style-count draft bound"};
        }
    }
    return Status::success();
}

[[nodiscard]] bool equal_property(
    const StylePropertyRecord& a,
    const StylePropertyRecord& b) noexcept {
    return a.property_key == b.property_key && a.value_type_id == b.value_type_id &&
           a.value == b.value;
}

[[nodiscard]] bool equal_resource(
    const StyleResourceBinding& a,
    const StyleResourceBinding& b) noexcept {
    return a.slot_id == b.slot_id && a.resource_id == b.resource_id;
}

[[nodiscard]] bool equal_style(
    const ProjectStyleRecord& a,
    const ProjectStyleRecord& b) noexcept {
    if (a.style_id != b.style_id || a.model_id != b.model_id || a.name != b.name ||
        a.parent_style_id != b.parent_style_id ||
        a.properties.size() != b.properties.size() || a.resources.size() != b.resources.size()) {
        return false;
    }
    for (std::size_t i = 0U; i < a.properties.size(); ++i) {
        if (!equal_property(a.properties[i], b.properties[i])) return false;
    }
    for (std::size_t i = 0U; i < a.resources.size(); ++i) {
        if (!equal_resource(a.resources[i], b.resources[i])) return false;
    }
    return true;
}

[[nodiscard]] Status validate_parent_chain(
    sqlite3* db,
    const std::string& style_id,
    const std::optional<std::string>& parent_style_id) {
    if (!parent_style_id.has_value()) return Status::success();
    std::string current = *parent_style_id;
    std::set<std::string> visited;
    visited.insert(style_id);
    for (std::size_t depth = 0U; depth < kMaxProjectStyles; ++depth) {
        if (!visited.insert(current).second) {
            return {StorageError::invalid_argument, "style inheritance cycle is not allowed"};
        }
        detail::StmtPtr stmt;
        Status status = detail::prepare(
            db, "SELECT parent_style_id FROM aeris_style WHERE style_id=?;", stmt);
        if (!status) return status;
        if (!(status = detail::bind_text(db, stmt.get(), 1, current))) return status;
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) {
            return {StorageError::record_not_found, "parent style does not exist"};
        }
        if (rc != SQLITE_ROW ||
            (sqlite3_column_type(stmt.get(), 0) != SQLITE_NULL &&
             sqlite3_column_type(stmt.get(), 0) != SQLITE_TEXT)) {
            return {StorageError::schema_invalid, "parent style chain contains malformed row"};
        }
        if (sqlite3_column_type(stmt.get(), 0) == SQLITE_NULL) return Status::success();
        current = text_column(stmt.get(), 0);
        if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
            return {StorageError::schema_invalid, "parent style ID is not unique"};
        }
    }
    return {StorageError::schema_invalid, "style inheritance chain exceeds style-count bound"};
}

[[nodiscard]] Status promote_style_resource(
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
        return {StorageError::record_not_found, "style resource does not exist"};
    }
    if (rc != SQLITE_ROW || sqlite3_column_type(read.get(), 0) != SQLITE_INTEGER ||
        sqlite3_column_type(read.get(), 1) != SQLITE_INTEGER) {
        return {StorageError::schema_invalid, "style resource state is malformed"};
    }
    const sqlite3_int64 mode = sqlite3_column_int64(read.get(), 0);
    const sqlite3_int64 required = sqlite3_column_int64(read.get(), 1);
    if ((mode != 0 && mode != 1) || (required != 0 && required != 1)) {
        return {StorageError::schema_invalid, "style resource state is outside canonical range"};
    }
    if (sqlite3_step(read.get()) != SQLITE_DONE) {
        return {StorageError::schema_invalid, "style resource ID resolves to duplicate rows"};
    }

    if (required == 0) {
        detail::StmtPtr update;
        status = detail::prepare(
            db,
            "UPDATE aeris_resource SET required_for_reproduction=1 WHERE resource_id=?;",
            update);
        if (status) status = detail::bind_text(db, update.get(), 1, resource_id);
        if (status) status = detail::step_done(db, update.get());
        if (status && sqlite3_changes(db) != 1) {
            return {StorageError::schema_invalid,
                    "style resource requirement promotion did not affect one row"};
        }
        if (!status) return status;
    }
    if (mode == 0) external_required = true;
    return Status::success();
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
        return {StorageError::schema_invalid, "style mutation could not read revision/frozen state"};
    }
    const sqlite3_int64 raw_revision = sqlite3_column_int64(stmt.get(), 0);
    const sqlite3_int64 raw_frozen = sqlite3_column_int64(stmt.get(), 1);
    if (raw_revision < 0 || (raw_frozen != 0 && raw_frozen != 1)) {
        return {StorageError::schema_invalid, "style mutation found invalid revision/frozen state"};
    }
    revision = static_cast<std::uint64_t>(raw_revision);
    frozen = raw_frozen == 1;
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return {StorageError::schema_invalid, "aeris_meta is not singleton during style mutation"};
    }
    return Status::success();
}

[[nodiscard]] Status advance_revision(
    sqlite3* db,
    const ProjectStore& project,
    const std::string_view modified_utc,
    const bool force_unfrozen = false) {
    std::uint64_t revision = 0U;
    bool frozen = false;
    Status status = read_revision_frozen(db, revision, frozen);
    if (!status) return status;
    if (revision >= static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max())) {
        return {StorageError::schema_invalid, "project revision exhausted during style mutation"};
    }
    detail::StmtPtr stmt;
    status = detail::prepare(
        db,
        "UPDATE aeris_meta SET revision=?,modified_utc=?,frozen=? WHERE id=1 AND project_uuid=?;",
        stmt);
    if (status) status = detail::bind_int64(
        db, stmt.get(), 1, static_cast<sqlite3_int64>(revision + 1U));
    if (status) status = detail::bind_text(db, stmt.get(), 2, std::string(modified_utc));
    if (status) status = detail::bind_int64(db, stmt.get(), 3, force_unfrozen ? 0 : (frozen ? 1 : 0));
    if (status) status = detail::bind_text(db, stmt.get(), 4, project.metadata().project_uuid);
    if (status) status = detail::step_done(db, stmt.get());
    if (status && sqlite3_changes(db) != 1) {
        return {StorageError::schema_invalid, "style mutation did not advance one metadata row"};
    }
    return status;
}

[[nodiscard]] Status insert_style_contents(sqlite3* db, const ProjectStyleRecord& record) {
    for (const StylePropertyRecord& property : record.properties) {
        detail::StmtPtr stmt;
        Status status = detail::prepare(
            db,
            "INSERT INTO aeris_style_property(style_id,property_key,value_type_id,value_payload) "
            "VALUES(?,?,?,?);",
            stmt);
        if (!status) return status;
        if (!(status = detail::bind_text(db, stmt.get(), 1, record.style_id))) return status;
        if (!(status = detail::bind_text(db, stmt.get(), 2, property.property_key))) return status;
        if (!(status = detail::bind_text(db, stmt.get(), 3, property.value_type_id))) return status;
        if (!(status = bind_blob(db, stmt.get(), 4, property.value))) return status;
        if (!(status = detail::step_done(db, stmt.get()))) return status;
    }
    for (const StyleResourceBinding& binding : record.resources) {
        detail::StmtPtr stmt;
        Status status = detail::prepare(
            db,
            "INSERT INTO aeris_style_resource(style_id,slot_id,resource_id) VALUES(?,?,?);",
            stmt);
        if (!status) return status;
        if (!(status = detail::bind_text(db, stmt.get(), 1, record.style_id))) return status;
        if (!(status = detail::bind_text(db, stmt.get(), 2, binding.slot_id))) return status;
        if (!(status = detail::bind_text(db, stmt.get(), 3, binding.resource_id))) return status;
        if (!(status = detail::step_done(db, stmt.get()))) return status;
    }
    return Status::success();
}

[[nodiscard]] Status load_layer_bindings(
    sqlite3* db,
    const std::string& layer_id,
    std::vector<LayerStyleBinding>& bindings) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(
        db,
        "SELECT b.slot_id,b.style_id,EXISTS(SELECT 1 FROM aeris_style s WHERE s.style_id=b.style_id) "
        "FROM aeris_layer_style b WHERE b.layer_id=? ORDER BY b.slot_id;",
        stmt);
    if (!status) return status;
    if (!(status = detail::bind_text(db, stmt.get(), 1, layer_id))) return status;
    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW || sqlite3_column_type(stmt.get(), 0) != SQLITE_TEXT ||
            sqlite3_column_type(stmt.get(), 1) != SQLITE_TEXT ||
            sqlite3_column_type(stmt.get(), 2) != SQLITE_INTEGER) {
            return {StorageError::schema_invalid, "stored layer style binding is malformed"};
        }
        LayerStyleBinding binding{text_column(stmt.get(), 0), text_column(stmt.get(), 1)};
        if (sqlite3_column_int(stmt.get(), 2) != 1 ||
            !bounded_text(binding.slot_id, kMaxIdentifierBytes) ||
            !bounded_text(binding.style_id, kMaxIdentifierBytes)) {
            return {StorageError::schema_invalid, "stored layer style binding is noncanonical/orphaned"};
        }
        bindings.push_back(std::move(binding));
        if (bindings.size() > kMaxLayerStyleBindings) {
            return {StorageError::schema_invalid, "stored layer exceeds style-binding draft bound"};
        }
    }
    return Status::success();
}

[[nodiscard]] bool equal_layer_bindings(
    const std::vector<LayerStyleBinding>& a,
    const std::vector<LayerStyleBinding>& b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0U; i < a.size(); ++i) {
        if (a[i].slot_id != b[i].slot_id || a[i].style_id != b[i].style_id) return false;
    }
    return true;
}

}  // namespace

StyleMutationResult set_style(
    ProjectStore& project,
    const ProjectStyleRecord& input,
    const std::string_view modified_utc) {
    if (!is_canonical_utc_timestamp(modified_utc)) {
        return {{StorageError::invalid_argument, "style mutation time is not canonical Gregorian UTC"}, false, false};
    }
    ProjectStyleRecord record = input;
    Status status = canonicalize_and_validate_style(record);
    if (!status) return {std::move(status), false, false};

    detail::DbPtr db;
    status = detail::open_database(project.path(), SQLITE_OPEN_READWRITE, db);
    if (!status) return {std::move(status), false, false};
    if (!(status = validate_project_connection(db.get(), project))) return {std::move(status), false, false};
    if (!(status = detail::configure_durable(db.get()))) return {std::move(status), false, false};
    if (!(status = detail::begin_immediate(db.get()))) return {std::move(status), false, false};

    status = validate_parent_chain(db.get(), record.style_id, record.parent_style_id);
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false};
    }

    std::optional<ProjectStyleRecord> existing;
    status = load_style(db.get(), record.style_id, existing);
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false};
    }
    if (existing.has_value() && equal_style(*existing, record)) {
        detail::rollback(db.get());
        status = project.refresh_metadata();
        if (!status) return {std::move(status), false, false};
        return {Status::success(), false, false};
    }

    if (!existing.has_value()) {
        sqlite3_int64 count = 0;
        if (!(status = detail::query_single_int(db.get(), "SELECT COUNT(*) FROM aeris_style;", count))) {
            detail::rollback(db.get());
            return {std::move(status), false, false};
        }
        if (count < 0 || count >= static_cast<sqlite3_int64>(kMaxProjectStyles)) {
            detail::rollback(db.get());
            return {{StorageError::invalid_argument, "project style-count draft bound reached"}, false, false};
        }
    }

    bool external_required = false;
    for (const StyleResourceBinding& binding : record.resources) {
        status = promote_style_resource(db.get(), binding.resource_id, external_required);
        if (!status) {
            detail::rollback(db.get());
            return {std::move(status), false, false};
        }
    }

    if (existing.has_value()) {
        detail::StmtPtr update;
        status = detail::prepare(
            db,
            "UPDATE aeris_style SET model_id=?,name=?,parent_style_id=? WHERE style_id=?;",
            update);
        if (status) status = detail::bind_text(db.get(), update.get(), 1, record.model_id);
        if (status) status = detail::bind_text(db.get(), update.get(), 2, record.name);
        if (status) {
            if (record.parent_style_id.has_value()) {
                status = detail::bind_text(db.get(), update.get(), 3, *record.parent_style_id);
            } else if (sqlite3_bind_null(update.get(), 3) != SQLITE_OK) {
                status = {StorageError::sqlite_failure,
                          detail::sqlite_message(db.get(), "sqlite style parent NULL bind failed")};
            }
        }
        if (status) status = detail::bind_text(db.get(), update.get(), 4, record.style_id);
        if (status) status = detail::step_done(db.get(), update.get());
        if (status && sqlite3_changes(db.get()) != 1) {
            status = {StorageError::schema_invalid, "style update did not affect exactly one row"};
        }
        if (status) status = detail::exec(
            db.get(), ("DELETE FROM aeris_style_property WHERE style_id='" + record.style_id + "';").c_str());
        if (status) status = detail::exec(
            db.get(), ("DELETE FROM aeris_style_resource WHERE style_id='" + record.style_id + "';").c_str());
    } else {
        detail::StmtPtr insert;
        status = detail::prepare(
            db,
            "INSERT INTO aeris_style(style_id,model_id,name,parent_style_id) VALUES(?,?,?,?);",
            insert);
        if (status) status = detail::bind_text(db.get(), insert.get(), 1, record.style_id);
        if (status) status = detail::bind_text(db.get(), insert.get(), 2, record.model_id);
        if (status) status = detail::bind_text(db.get(), insert.get(), 3, record.name);
        if (status) {
            if (record.parent_style_id.has_value()) {
                status = detail::bind_text(db.get(), insert.get(), 4, *record.parent_style_id);
            } else if (sqlite3_bind_null(insert.get(), 4) != SQLITE_OK) {
                status = {StorageError::sqlite_failure,
                          detail::sqlite_message(db.get(), "sqlite style parent NULL bind failed")};
            }
        }
        if (status) status = detail::step_done(db.get(), insert.get());
    }

    if (status) status = insert_style_contents(db.get(), record);
    if (status) status = advance_revision(db.get(), project, modified_utc, external_required);
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

StyleMutationResult remove_style(
    ProjectStore& project,
    const std::string_view style_id,
    const std::string_view modified_utc) {
    const std::string id(style_id);
    if (!bounded_text(id, kMaxIdentifierBytes) || !is_canonical_utc_timestamp(modified_utc)) {
        return {{StorageError::invalid_argument, "style removal arguments violate canonical bounds"}, false, false};
    }
    detail::DbPtr db;
    Status status = detail::open_database(project.path(), SQLITE_OPEN_READWRITE, db);
    if (!status) return {std::move(status), false, false};
    if (!(status = validate_project_connection(db.get(), project))) return {std::move(status), false, false};
    if (!(status = detail::configure_durable(db.get()))) return {std::move(status), false, false};
    if (!(status = detail::begin_immediate(db.get()))) return {std::move(status), false, false};

    std::optional<ProjectStyleRecord> existing;
    status = load_style(db.get(), id, existing);
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false};
    }
    if (!existing.has_value()) {
        detail::rollback(db.get());
        status = project.refresh_metadata();
        if (!status) return {std::move(status), false, false};
        return {Status::success(), false, false};
    }

    for (const char* sql : {
             "SELECT COUNT(*) FROM aeris_style WHERE parent_style_id=?;",
             "SELECT COUNT(*) FROM aeris_layer_style WHERE style_id=?;"}) {
        detail::StmtPtr stmt;
        status = detail::prepare(db.get(), sql, stmt);
        if (!status) break;
        if (!(status = detail::bind_text(db.get(), stmt.get(), 1, id))) break;
        const int rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_ROW || sqlite3_column_type(stmt.get(), 0) != SQLITE_INTEGER) {
            status = {StorageError::schema_invalid, "style reference count probe failed"};
            break;
        }
        if (sqlite3_column_int64(stmt.get(), 0) > 0) {
            detail::rollback(db.get());
            return {{StorageError::record_exists,
                     "style is still referenced by a child style or layer"}, false, false};
        }
    }
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false};
    }

    detail::StmtPtr erase;
    status = detail::prepare(db.get(), "DELETE FROM aeris_style WHERE style_id=?;", erase);
    if (status) status = detail::bind_text(db.get(), erase.get(), 1, id);
    if (status) status = detail::step_done(db.get(), erase.get());
    if (status && sqlite3_changes(db.get()) != 1) {
        status = {StorageError::schema_invalid, "style removal did not affect exactly one row"};
    }
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

StyleMutationResult set_layer_style_bindings(
    ProjectStore& project,
    const std::string_view layer_id,
    const std::vector<LayerStyleBinding>& input,
    const std::string_view modified_utc) {
    const std::string id(layer_id);
    if (!bounded_text(id, kMaxIdentifierBytes) || !is_canonical_utc_timestamp(modified_utc)) {
        return {{StorageError::invalid_argument, "layer style mutation arguments violate canonical bounds"}, false, false};
    }
    std::vector<LayerStyleBinding> bindings = input;
    Status status = canonicalize_layer_style_bindings(bindings);
    if (!status) return {std::move(status), false, false};

    detail::DbPtr db;
    status = detail::open_database(project.path(), SQLITE_OPEN_READWRITE, db);
    if (!status) return {std::move(status), false, false};
    if (!(status = validate_project_connection(db.get(), project))) return {std::move(status), false, false};
    if (!(status = detail::configure_durable(db.get()))) return {std::move(status), false, false};
    if (!(status = detail::begin_immediate(db.get()))) return {std::move(status), false, false};

    detail::StmtPtr layer_probe;
    status = detail::prepare(db.get(), "SELECT 1 FROM aeris_layer WHERE layer_id=?;", layer_probe);
    if (status) status = detail::bind_text(db.get(), layer_probe.get(), 1, id);
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false};
    }
    if (sqlite3_step(layer_probe.get()) != SQLITE_ROW) {
        detail::rollback(db.get());
        return {{StorageError::record_not_found, "layer style target does not exist"}, false, false};
    }

    for (const LayerStyleBinding& binding : bindings) {
        std::optional<ProjectStyleRecord> style;
        status = load_style(db.get(), binding.style_id, style);
        if (!status || !style.has_value()) {
            detail::rollback(db.get());
            if (!status) return {std::move(status), false, false};
            return {{StorageError::record_not_found, "layer style binding references missing style"}, false, false};
        }
    }

    std::vector<LayerStyleBinding> existing;
    status = load_layer_bindings(db.get(), id, existing);
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false};
    }
    if (equal_layer_bindings(existing, bindings)) {
        detail::rollback(db.get());
        status = project.refresh_metadata();
        if (!status) return {std::move(status), false, false};
        return {Status::success(), false, false};
    }

    detail::StmtPtr erase;
    status = detail::prepare(db.get(), "DELETE FROM aeris_layer_style WHERE layer_id=?;", erase);
    if (status) status = detail::bind_text(db.get(), erase.get(), 1, id);
    if (status) status = detail::step_done(db.get(), erase.get());
    for (const LayerStyleBinding& binding : bindings) {
        if (!status) break;
        detail::StmtPtr insert;
        status = detail::prepare(
            db.get(), "INSERT INTO aeris_layer_style(layer_id,slot_id,style_id) VALUES(?,?,?);", insert);
        if (status) status = detail::bind_text(db.get(), insert.get(), 1, id);
        if (status) status = detail::bind_text(db.get(), insert.get(), 2, binding.slot_id);
        if (status) status = detail::bind_text(db.get(), insert.get(), 3, binding.style_id);
        if (status) status = detail::step_done(db.get(), insert.get());
    }
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

ProjectStyleListResult list_project_styles(const ProjectStore& project) {
    detail::DbPtr db;
    Status status = detail::open_database(project.path(), SQLITE_OPEN_READONLY, db);
    if (!status) return {std::move(status), {}};
    if (!(status = validate_project_connection(db.get(), project))) return {std::move(status), {}};
    ProjectStyleListResult result{};
    result.status = load_all_styles(db.get(), result.records);
    if (!result.status) result.records.clear();
    return result;
}

LayerStyleBindingListResult list_layer_style_bindings(
    const ProjectStore& project,
    const std::string_view layer_id) {
    const std::string id(layer_id);
    if (!bounded_text(id, kMaxIdentifierBytes)) {
        return {{StorageError::invalid_argument, "layer ID violates canonical bounds"}, {}};
    }
    detail::DbPtr db;
    Status status = detail::open_database(project.path(), SQLITE_OPEN_READONLY, db);
    if (!status) return {std::move(status), {}};
    if (!(status = validate_project_connection(db.get(), project))) return {std::move(status), {}};
    LayerStyleBindingListResult result{};
    result.status = load_layer_bindings(db.get(), id, result.records);
    if (!result.status) result.records.clear();
    return result;
}

namespace detail {

Status verify_style_semantics(const ProjectStore& project) {
    DbPtr db;
    Status status = open_database(project.path(), SQLITE_OPEN_READONLY, db);
    if (!status) return status;
    if (!(status = validate_project_connection(db.get(), project))) return status;

    std::vector<ProjectStyleRecord> styles;
    if (!(status = load_all_styles(db.get(), styles))) return status;
    std::set<std::string> ids;
    for (const ProjectStyleRecord& style : styles) ids.insert(style.style_id);
    for (const ProjectStyleRecord& style : styles) {
        if (style.parent_style_id.has_value() && ids.count(*style.parent_style_id) != 1U) {
            return {StorageError::schema_invalid, "style inheritance references missing parent"};
        }
        status = validate_parent_chain(db.get(), style.style_id, style.parent_style_id);
        if (!status) {
            return {StorageError::schema_invalid,
                    "style inheritance graph is invalid: " + status.diagnostic};
        }
    }

    StmtPtr layer_stmt;
    status = prepare(db.get(), "SELECT layer_id FROM aeris_layer ORDER BY layer_id;", layer_stmt);
    if (!status) return status;
    while (true) {
        const int rc = sqlite3_step(layer_stmt.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW || sqlite3_column_type(layer_stmt.get(), 0) != SQLITE_TEXT) {
            return {StorageError::schema_invalid, "style integrity layer index is malformed"};
        }
        const std::string layer_id = text_column(layer_stmt.get(), 0);
        std::vector<LayerStyleBinding> bindings;
        if (!(status = load_layer_bindings(db.get(), layer_id, bindings))) return status;
    }
    return Status::success();
}

}  // namespace detail

}  // namespace aeris::storage
