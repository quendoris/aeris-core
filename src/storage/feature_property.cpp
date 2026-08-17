// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#include "aeris/storage/feature_property.hpp"

#include "feature_property_detail.hpp"
#include "sqlite_detail.hpp"
#include "aeris/util/text.hpp"

#include <algorithm>
#include <array>
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

static_assert(sizeof(double) == 8U, "AERIS feature properties require 64-bit double");
static_assert(std::numeric_limits<double>::is_iec559,
              "AERIS feature properties require IEC 60559 / IEEE-754 binary64");
static_assert(sizeof(std::int64_t) == 8U, "AERIS feature properties require 64-bit int64");

constexpr std::size_t kMaxIdentifierBytes = 255U;
constexpr std::size_t kMaxFeaturesPerSource = 1'000'000U;

[[nodiscard]] bool bounded_identifier(const std::string_view value) noexcept {
    return !value.empty() && value.size() <= kMaxIdentifierBytes &&
           util::is_valid_utf8_nul_free(value);
}

[[nodiscard]] double canonical_zero(const double value) noexcept {
    return value == 0.0 ? 0.0 : value;
}

[[nodiscard]] std::array<unsigned char, 8> encode_u64le(const std::uint64_t value) noexcept {
    std::array<unsigned char, 8> bytes{};
    for (unsigned index = 0U; index < 8U; ++index) {
        bytes[index] = static_cast<unsigned char>((value >> (8U * index)) & 0xffU);
    }
    return bytes;
}

[[nodiscard]] std::uint64_t decode_u64le(const unsigned char* bytes) noexcept {
    std::uint64_t value = 0U;
    for (unsigned index = 0U; index < 8U; ++index) {
        value |= static_cast<std::uint64_t>(bytes[index]) << (8U * index);
    }
    return value;
}

[[nodiscard]] std::array<unsigned char, 8> encode_f64le(const double input) noexcept {
    const double value = canonical_zero(input);
    std::uint64_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    return encode_u64le(bits);
}

[[nodiscard]] double decode_f64le(const unsigned char* bytes) noexcept {
    const std::uint64_t bits = decode_u64le(bytes);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

Status bind_blob(
    sqlite3* db,
    sqlite3_stmt* stmt,
    const int index,
    const void* data,
    const std::size_t bytes) {
    if (bytes > static_cast<std::size_t>(INT_MAX)) {
        return {StorageError::invalid_argument,
                "feature property payload exceeds SQLite bind size"};
    }
    static constexpr unsigned char kEmptyBlobSentinel = 0U;
    const void* pointer = bytes == 0U ? &kEmptyBlobSentinel : data;
    if (sqlite3_bind_blob(stmt, index, pointer, static_cast<int>(bytes), SQLITE_TRANSIENT) != SQLITE_OK) {
        return {StorageError::sqlite_failure,
                detail::sqlite_message(db, "sqlite bind feature property BLOB failed")};
    }
    return Status::success();
}

[[nodiscard]] std::string text_column(sqlite3_stmt* stmt, const int column) {
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, column));
    const int bytes = sqlite3_column_bytes(stmt, column);
    return std::string(text, static_cast<std::size_t>(bytes));
}

Status validate_project_connection(sqlite3* db, const ProjectStore& project) {
    Status status = detail::verify_quick_check(db);
    if (!status) return status;

    sqlite3_int64 application_id = 0;
    if (!(status = detail::query_single_int(db, "PRAGMA application_id;", application_id))) return status;
    if (application_id != static_cast<sqlite3_int64>(kProjectApplicationId)) {
        return {StorageError::invalid_application_id,
                "feature property target is not an AERIS project"};
    }

    sqlite3_int64 generation = 0;
    if (!(status = detail::query_single_int(db, "PRAGMA user_version;", generation))) return status;
    if (generation != kProjectSchemaGeneration) {
        return {StorageError::unsupported_schema,
                "feature property target has unsupported draft schema generation"};
    }

    std::string uuid;
    if (!(status = detail::query_single_text(
              db, "SELECT project_uuid FROM aeris_meta WHERE id=1;", uuid))) return status;
    if (uuid != project.metadata().project_uuid) {
        return {StorageError::schema_invalid,
                "feature property target UUID differs from validated project handle"};
    }
    return Status::success();
}

Status marker_exists(sqlite3* db, const std::string_view source_id, bool& exists) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(
        db, "SELECT 1 FROM aeris_source_feature_properties WHERE source_id=?;", stmt);
    if (!status) return status;
    if (!(status = detail::bind_text(db, stmt.get(), 1, std::string(source_id)))) return status;
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) {
        exists = false;
        return Status::success();
    }
    if (rc == SQLITE_ROW) {
        exists = true;
        if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
            return {StorageError::schema_invalid,
                    "feature property marker uniqueness probe returned multiple rows"};
        }
        return Status::success();
    }
    return {StorageError::sqlite_failure,
            detail::sqlite_message(db, "feature property marker probe failed")};
}

Status advance_project_revision(
    sqlite3* db,
    const ProjectStore& project,
    const std::string_view modified_utc) {
    sqlite3_int64 revision = 0;
    Status status = detail::query_single_int(
        db, "SELECT revision FROM aeris_meta WHERE id=1;", revision);
    if (!status) return status;
    if (revision < 0 || revision == std::numeric_limits<sqlite3_int64>::max()) {
        return {StorageError::schema_invalid,
                "project revision cannot be incremented for feature property mutation"};
    }

    detail::StmtPtr stmt;
    status = detail::prepare(
        db,
        "UPDATE aeris_meta SET revision=?,modified_utc=? WHERE id=1 AND project_uuid=?;",
        stmt);
    if (status) status = detail::bind_int64(db, stmt.get(), 1, revision + 1);
    if (status) status = detail::bind_text(db, stmt.get(), 2, std::string(modified_utc));
    if (status) status = detail::bind_text(db, stmt.get(), 3, project.metadata().project_uuid);
    if (status) status = detail::step_done(db, stmt.get());
    if (status && sqlite3_changes(db) != 1) {
        return {StorageError::schema_invalid,
                "feature property mutation could not advance exactly one project metadata row"};
    }
    return status;
}

Status read_marker(
    sqlite3* db,
    const std::string_view source_id,
    std::uint64_t& feature_count) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(
        db,
        "SELECT model_id,encoding_id,feature_count FROM aeris_source_feature_properties WHERE source_id=?;",
        stmt);
    if (!status) return status;
    if (!(status = detail::bind_text(db, stmt.get(), 1, std::string(source_id)))) return status;
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) {
        return {StorageError::record_not_found,
                "source has no complete feature property marker"};
    }
    if (rc != SQLITE_ROW || sqlite3_column_type(stmt.get(), 0) != SQLITE_TEXT ||
        sqlite3_column_type(stmt.get(), 1) != SQLITE_TEXT ||
        sqlite3_column_type(stmt.get(), 2) != SQLITE_INTEGER) {
        return {StorageError::schema_invalid,
                "feature property marker contains malformed column types"};
    }
    const std::string model = text_column(stmt.get(), 0);
    const std::string encoding = text_column(stmt.get(), 1);
    const sqlite3_int64 count = sqlite3_column_int64(stmt.get(), 2);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return {StorageError::schema_invalid,
                "feature property source marker is not unique"};
    }
    if (std::string_view(model) != kFeaturePropertiesModelId ||
        std::string_view(encoding) != kFeaturePropertiesEncodingId ||
        count < 0 || count > static_cast<sqlite3_int64>(kMaxFeaturesPerSource)) {
        return {StorageError::schema_invalid,
                "feature property marker violates canonical model/encoding/count"};
    }
    feature_count = static_cast<std::uint64_t>(count);
    return Status::success();
}

Status decode_property(
    sqlite3_stmt* stmt,
    const int type_column,
    const int payload_column,
    StoredFeatureProperty& property) {
    if (sqlite3_column_type(stmt, type_column) != SQLITE_TEXT ||
        sqlite3_column_type(stmt, payload_column) != SQLITE_BLOB) {
        return {StorageError::schema_invalid,
                "feature property row has noncanonical type/payload SQLite classes"};
    }
    const std::string type_id = text_column(stmt, type_column);
    const int byte_count = sqlite3_column_bytes(stmt, payload_column);
    if (byte_count < 0) {
        return {StorageError::schema_invalid,
                "feature property payload has negative SQLite byte count"};
    }
    const auto size = static_cast<std::size_t>(byte_count);
    const auto* bytes = static_cast<const unsigned char*>(
        sqlite3_column_blob(stmt, payload_column));
    if (size != 0U && bytes == nullptr) {
        return {StorageError::schema_invalid,
                "feature property payload bytes are unavailable"};
    }

    if (std::string_view(type_id) == kFeaturePropertyBoolTypeId) {
        if (size != 1U || (bytes[0] != 0U && bytes[0] != 1U)) {
            return {StorageError::schema_invalid,
                    "feature bool property is not canonical one-byte 0/1"};
        }
        property.value = bytes[0] == 1U;
        return Status::success();
    }
    if (std::string_view(type_id) == kFeaturePropertyInt64LeTypeId) {
        if (size != 8U) {
            return {StorageError::schema_invalid,
                    "feature int64 property is not canonical eight-byte LE"};
        }
        const std::uint64_t bits = decode_u64le(bytes);
        std::int64_t signed_value = 0;
        std::memcpy(&signed_value, &bits, sizeof(signed_value));
        property.value = signed_value;
        return Status::success();
    }
    if (std::string_view(type_id) == kFeaturePropertyF64LeTypeId) {
        if (size != 8U) {
            return {StorageError::schema_invalid,
                    "feature f64 property is not canonical eight-byte LE"};
        }
        const double real_value = decode_f64le(bytes);
        if (!std::isfinite(real_value) || (real_value == 0.0 && std::signbit(real_value))) {
            return {StorageError::schema_invalid,
                    "feature f64 property is non-finite or negative zero"};
        }
        property.value = real_value;
        return Status::success();
    }
    if (std::string_view(type_id) == kFeaturePropertyUtf8TypeId) {
        if (size > kMaxFeaturePropertyTextBytes) {
            return {StorageError::schema_invalid,
                    "feature UTF-8 property exceeds draft payload bound"};
        }
        const char* chars = size == 0U ? "" : reinterpret_cast<const char*>(bytes);
        std::string text_value(chars, size);
        if (!util::is_valid_utf8_nul_free(text_value)) {
            return {StorageError::schema_invalid,
                    "feature UTF-8 property is malformed or contains NUL"};
        }
        property.value = std::move(text_value);
        return Status::success();
    }
    return {StorageError::schema_invalid,
            "feature property uses an unsupported value type identifier"};
}

Status validate_geometry_match(
    const ProjectStore& project,
    const SourceFeaturePropertiesRecord& record) {
    const SourceGeometryIndexResult geometry =
        list_source_geometry_index(project, record.source_id);
    if (!geometry.ok()) return geometry.status;
    if (geometry.features.size() != record.features.size()) {
        return {StorageError::invalid_argument,
                "complete feature properties must contain exactly every canonical geometry feature"};
    }
    for (std::size_t index = 0U; index < geometry.features.size(); ++index) {
        if (geometry.features[index].stable_id != record.features[index].stable_id) {
            return {StorageError::invalid_argument,
                    "feature property stable-ID set does not exactly match canonical geometry"};
        }
    }
    return Status::success();
}

}  // namespace

namespace detail {

Status canonicalize_and_validate_feature_properties(
    SourceFeaturePropertiesRecord& record) {
    if (!bounded_identifier(record.source_id)) {
        return {StorageError::invalid_argument,
                "feature property source ID is empty, malformed UTF-8, contains NUL, or exceeds 255 bytes"};
    }
    if (record.features.size() > kMaxFeaturesPerSource) {
        return {StorageError::invalid_argument,
                "feature property set exceeds the 1000000-feature draft bound"};
    }

    std::set<std::string> stable_ids;
    for (FeaturePropertiesRecord& feature : record.features) {
        if (!bounded_identifier(feature.stable_id) ||
            !stable_ids.insert(feature.stable_id).second) {
            return {StorageError::invalid_argument,
                    "feature property set contains invalid or duplicate stable feature ID"};
        }
        if (feature.properties.size() > kMaxFeaturePropertiesPerFeature) {
            return {StorageError::invalid_argument,
                    "feature exceeds the 4096-property draft bound"};
        }

        std::set<std::string> keys;
        for (StoredFeatureProperty& property : feature.properties) {
            if (!bounded_identifier(property.key) || !keys.insert(property.key).second) {
                return {StorageError::invalid_argument,
                        "feature contains invalid or duplicate property key"};
            }
            if (auto* real = std::get_if<double>(&property.value)) {
                if (!std::isfinite(*real)) {
                    return {StorageError::invalid_argument,
                            "feature property contains non-finite binary64"};
                }
                *real = canonical_zero(*real);
            } else if (const auto* text = std::get_if<std::string>(&property.value)) {
                if (text->size() > kMaxFeaturePropertyTextBytes ||
                    !util::is_valid_utf8_nul_free(*text)) {
                    return {StorageError::invalid_argument,
                            "feature text property is malformed UTF-8, contains NUL, or exceeds 1 MiB"};
                }
            }
        }
        std::sort(feature.properties.begin(), feature.properties.end(),
                  [](const StoredFeatureProperty& a, const StoredFeatureProperty& b) {
                      return a.key < b.key;
                  });
    }
    std::sort(record.features.begin(), record.features.end(),
              [](const FeaturePropertiesRecord& a, const FeaturePropertiesRecord& b) {
                  return a.stable_id < b.stable_id;
              });
    return Status::success();
}

bool equal_feature_properties(
    const SourceFeaturePropertiesRecord& a,
    const SourceFeaturePropertiesRecord& b) noexcept {
    if (a.source_id != b.source_id || a.features.size() != b.features.size()) return false;
    for (std::size_t feature_index = 0U; feature_index < a.features.size(); ++feature_index) {
        const auto& left = a.features[feature_index];
        const auto& right = b.features[feature_index];
        if (left.stable_id != right.stable_id ||
            left.properties.size() != right.properties.size()) return false;
        for (std::size_t property_index = 0U; property_index < left.properties.size(); ++property_index) {
            if (left.properties[property_index].key != right.properties[property_index].key ||
                left.properties[property_index].value != right.properties[property_index].value) {
                return false;
            }
        }
    }
    return true;
}

Status insert_feature_properties(
    sqlite3* db,
    const SourceFeaturePropertiesRecord& record) {
    detail::StmtPtr marker;
    Status status = detail::prepare(
        db,
        "INSERT INTO aeris_source_feature_properties(source_id,model_id,encoding_id,feature_count) VALUES(?,?,?,?);",
        marker);
    if (!status) return status;
    if (!(status = detail::bind_text(db, marker.get(), 1, record.source_id))) return status;
    if (!(status = detail::bind_text(db, marker.get(), 2, std::string(kFeaturePropertiesModelId)))) return status;
    if (!(status = detail::bind_text(db, marker.get(), 3, std::string(kFeaturePropertiesEncodingId)))) return status;
    if (!(status = detail::bind_int64(
              db, marker.get(), 4, static_cast<sqlite3_int64>(record.features.size())))) return status;
    if (!(status = detail::step_done(db, marker.get()))) return status;

    for (const FeaturePropertiesRecord& feature : record.features) {
        for (const StoredFeatureProperty& property : feature.properties) {
            std::string_view type_id;
            std::array<unsigned char, 8> fixed{};
            const void* payload = nullptr;
            std::size_t payload_size = 0U;
            std::string text_payload;
            unsigned char bool_payload = 0U;

            if (const auto* bool_value = std::get_if<bool>(&property.value)) {
                type_id = kFeaturePropertyBoolTypeId;
                bool_payload = *bool_value ? 1U : 0U;
                payload = &bool_payload;
                payload_size = 1U;
            } else if (const auto* integer_value = std::get_if<std::int64_t>(&property.value)) {
                type_id = kFeaturePropertyInt64LeTypeId;
                std::uint64_t bits = 0U;
                std::memcpy(&bits, integer_value, sizeof(bits));
                fixed = encode_u64le(bits);
                payload = fixed.data();
                payload_size = fixed.size();
            } else if (const auto* real_value = std::get_if<double>(&property.value)) {
                type_id = kFeaturePropertyF64LeTypeId;
                fixed = encode_f64le(*real_value);
                payload = fixed.data();
                payload_size = fixed.size();
            } else if (const auto* string_value = std::get_if<std::string>(&property.value)) {
                type_id = kFeaturePropertyUtf8TypeId;
                text_payload = *string_value;
                payload = text_payload.data();
                payload_size = text_payload.size();
            } else {
                return {StorageError::invalid_argument,
                        "feature property variant contains unsupported alternative"};
            }

            detail::StmtPtr stmt;
            status = detail::prepare(
                db,
                "INSERT INTO aeris_feature_property(source_id,stable_id,property_key,value_type_id,value_payload) VALUES(?,?,?,?,?);",
                stmt);
            if (!status) return status;
            if (!(status = detail::bind_text(db, stmt.get(), 1, record.source_id))) return status;
            if (!(status = detail::bind_text(db, stmt.get(), 2, feature.stable_id))) return status;
            if (!(status = detail::bind_text(db, stmt.get(), 3, property.key))) return status;
            if (!(status = detail::bind_text(db, stmt.get(), 4, std::string(type_id)))) return status;
            if (!(status = bind_blob(db, stmt.get(), 5, payload, payload_size))) return status;
            if (!(status = detail::step_done(db, stmt.get()))) return status;
        }
    }
    return Status::success();
}

Status read_existing_feature_properties(
    const ProjectStore& project,
    const std::string_view source_id,
    std::optional<SourceFeaturePropertiesRecord>& record) {
    const SourceFeaturePropertiesIndexResult index =
        list_source_feature_properties_index(project, source_id);
    if (!index.ok()) {
        if (index.status.error == StorageError::record_not_found) {
            record.reset();
            return Status::success();
        }
        return index.status;
    }

    SourceFeaturePropertiesRecord loaded{};
    loaded.source_id = std::string(source_id);
    loaded.features.reserve(index.features.size());
    for (const FeaturePropertiesIndexEntry& entry : index.features) {
        FeaturePropertiesLoadResult properties =
            load_feature_properties(project, source_id, entry.stable_id);
        if (!properties.ok()) return properties.status;
        if (properties.properties.size() != static_cast<std::size_t>(entry.property_count)) {
            return {StorageError::schema_invalid,
                    "feature property index disagrees with property loader"};
        }
        loaded.features.push_back({entry.stable_id, std::move(properties.properties)});
    }
    Status status = canonicalize_and_validate_feature_properties(loaded);
    if (!status) {
        return {StorageError::schema_invalid,
                "persisted feature property set is not canonical: " + status.diagnostic};
    }
    record = std::move(loaded);
    return Status::success();
}

Status verify_feature_property_semantics(const ProjectStore& project) {
    detail::DbPtr db;
    Status status = detail::open_database(project.path(), SQLITE_OPEN_READONLY, db);
    if (!status) return status;
    if (!(status = validate_project_connection(db.get(), project))) return status;

    detail::StmtPtr orphan;
    status = detail::prepare(
        db.get(),
        "SELECT p.source_id FROM aeris_feature_property p "
        "LEFT JOIN aeris_source_feature_properties m ON m.source_id=p.source_id "
        "WHERE m.source_id IS NULL LIMIT 1;",
        orphan);
    if (!status) return status;
    if (sqlite3_step(orphan.get()) == SQLITE_ROW) {
        return {StorageError::schema_invalid,
                "feature property row exists without complete source marker"};
    }

    detail::StmtPtr markers;
    status = detail::prepare(
        db.get(),
        "SELECT source_id FROM aeris_source_feature_properties ORDER BY source_id;",
        markers);
    if (!status) return status;
    std::vector<std::string> source_ids;
    while (true) {
        const int rc = sqlite3_step(markers.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW || sqlite3_column_type(markers.get(), 0) != SQLITE_TEXT) {
            return {StorageError::schema_invalid,
                    "feature property marker catalog is malformed"};
        }
        source_ids.push_back(text_column(markers.get(), 0));
    }
    db.reset();

    for (const std::string& source_id : source_ids) {
        const SourceFeaturePropertiesIndexResult index =
            list_source_feature_properties_index(project, source_id);
        if (!index.ok()) return index.status;
        for (const FeaturePropertiesIndexEntry& feature : index.features) {
            const FeaturePropertiesLoadResult loaded =
                load_feature_properties(project, source_id, feature.stable_id);
            if (!loaded.ok()) return loaded.status;
            if (loaded.properties.size() != static_cast<std::size_t>(feature.property_count)) {
                return {StorageError::schema_invalid,
                        "feature property deep audit count mismatch"};
            }
        }
    }
    return Status::success();
}

}  // namespace detail

SourceFeaturePropertiesIndexResult list_source_feature_properties_index(
    const ProjectStore& project,
    const std::string_view source_id) {
    if (!bounded_identifier(source_id)) {
        return {{StorageError::invalid_argument,
                 "feature property source ID is invalid"}, {}};
    }

    detail::DbPtr db;
    Status status = detail::open_database(project.path(), SQLITE_OPEN_READONLY, db);
    if (!status) return {std::move(status), {}};
    if (!(status = validate_project_connection(db.get(), project))) {
        return {std::move(status), {}};
    }

    std::uint64_t marker_count = 0U;
    status = read_marker(db.get(), source_id, marker_count);
    if (!status) return {std::move(status), {}};

    detail::StmtPtr stmt;
    status = detail::prepare(
        db.get(),
        "SELECT f.stable_id,f.source_feature_id,COUNT(p.property_key) "
        "FROM aeris_feature f "
        "LEFT JOIN aeris_feature_property p "
        "ON p.source_id=f.source_id AND p.stable_id=f.stable_id "
        "WHERE f.source_id=? "
        "GROUP BY f.stable_id,f.source_feature_id "
        "ORDER BY f.stable_id;",
        stmt);
    if (!status) return {std::move(status), {}};
    if (!(status = detail::bind_text(db.get(), stmt.get(), 1, std::string(source_id)))) {
        return {std::move(status), {}};
    }

    SourceFeaturePropertiesIndexResult result{};
    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW || sqlite3_column_type(stmt.get(), 0) != SQLITE_TEXT ||
            sqlite3_column_type(stmt.get(), 1) != SQLITE_TEXT ||
            sqlite3_column_type(stmt.get(), 2) != SQLITE_INTEGER) {
            return {{StorageError::schema_invalid,
                     "feature property index row has malformed SQLite classes"}, {}};
        }
        const sqlite3_int64 count = sqlite3_column_int64(stmt.get(), 2);
        if (count < 0 || count > static_cast<sqlite3_int64>(kMaxFeaturePropertiesPerFeature)) {
            return {{StorageError::schema_invalid,
                     "feature property count is outside canonical draft bounds"}, {}};
        }
        result.features.push_back({
            text_column(stmt.get(), 0),
            text_column(stmt.get(), 1),
            static_cast<std::uint32_t>(count)});
    }
    if (result.features.size() != marker_count) {
        return {{StorageError::schema_invalid,
                 "feature property marker count disagrees with canonical geometry feature set"}, {}};
    }
    result.status = Status::success();
    return result;
}

FeaturePropertiesLoadResult load_feature_properties(
    const ProjectStore& project,
    const std::string_view source_id,
    const std::string_view stable_id) {
    if (!bounded_identifier(source_id) || !bounded_identifier(stable_id)) {
        return {{StorageError::invalid_argument,
                 "feature property source/stable ID is invalid"}, {}};
    }

    const SourceFeaturePropertiesIndexResult index =
        list_source_feature_properties_index(project, source_id);
    if (!index.ok()) return {index.status, {}};
    const auto found = std::lower_bound(
        index.features.begin(), index.features.end(), stable_id,
        [](const FeaturePropertiesIndexEntry& entry, const std::string_view key) {
            return entry.stable_id < key;
        });
    if (found == index.features.end() || found->stable_id != stable_id) {
        return {{StorageError::record_not_found,
                 "feature does not exist in complete property-bearing geometry"}, {}};
    }

    detail::DbPtr db;
    Status status = detail::open_database(project.path(), SQLITE_OPEN_READONLY, db);
    if (!status) return {std::move(status), {}};
    if (!(status = validate_project_connection(db.get(), project))) {
        return {std::move(status), {}};
    }

    detail::StmtPtr stmt;
    status = detail::prepare(
        db.get(),
        "SELECT property_key,value_type_id,value_payload "
        "FROM aeris_feature_property WHERE source_id=? AND stable_id=? ORDER BY property_key;",
        stmt);
    if (!status) return {std::move(status), {}};
    if (!(status = detail::bind_text(db.get(), stmt.get(), 1, std::string(source_id)))) {
        return {std::move(status), {}};
    }
    if (!(status = detail::bind_text(db.get(), stmt.get(), 2, std::string(stable_id)))) {
        return {std::move(status), {}};
    }

    FeaturePropertiesLoadResult result{};
    std::string previous_key;
    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW || sqlite3_column_type(stmt.get(), 0) != SQLITE_TEXT) {
            return {{StorageError::schema_invalid,
                     "feature property row has malformed property key"}, {}};
        }
        StoredFeatureProperty property{};
        property.key = text_column(stmt.get(), 0);
        if (!bounded_identifier(property.key) ||
            (!previous_key.empty() && property.key <= previous_key)) {
            return {{StorageError::schema_invalid,
                     "feature property key ordering/identity is noncanonical"}, {}};
        }
        if (!(status = decode_property(stmt.get(), 1, 2, property))) {
            return {std::move(status), {}};
        }
        previous_key = property.key;
        result.properties.push_back(std::move(property));
        if (result.properties.size() > kMaxFeaturePropertiesPerFeature) {
            return {{StorageError::schema_invalid,
                     "feature property row count exceeds draft bound"}, {}};
        }
    }
    if (result.properties.size() != static_cast<std::size_t>(found->property_count)) {
        return {{StorageError::schema_invalid,
                 "feature property loader count disagrees with canonical index"}, {}};
    }
    result.status = Status::success();
    return result;
}

FeaturePropertiesMutationResult store_source_feature_properties(
    ProjectStore& project,
    const SourceFeaturePropertiesRecord& input,
    const std::string_view modified_utc) {
    if (!is_canonical_utc_timestamp(modified_utc)) {
        return {{StorageError::invalid_argument,
                 "feature property mutation timestamp is not canonical Gregorian UTC"},
                false, false};
    }

    SourceFeaturePropertiesRecord record = input;
    Status status = detail::canonicalize_and_validate_feature_properties(record);
    if (!status) return {std::move(status), false, false};
    if (!(status = validate_geometry_match(project, record))) {
        return {std::move(status), false, false};
    }

    std::optional<SourceFeaturePropertiesRecord> existing;
    if (!(status = detail::read_existing_feature_properties(project, record.source_id, existing))) {
        return {std::move(status), false, false};
    }
    if (existing.has_value()) {
        if (detail::equal_feature_properties(*existing, record)) {
            return {Status::success(), false, false};
        }
        return {{StorageError::record_exists,
                 "source already has a different immutable complete feature property set"},
                false, false};
    }

    detail::DbPtr db;
    if (!(status = detail::open_database(project.path(), SQLITE_OPEN_READWRITE, db))) {
        return {std::move(status), false, false};
    }
    if (!(status = validate_project_connection(db.get(), project)) ||
        !(status = detail::configure_durable(db.get())) ||
        !(status = detail::begin_immediate(db.get()))) {
        return {std::move(status), false, false};
    }

    bool present = false;
    status = marker_exists(db.get(), record.source_id, present);
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false};
    }
    if (present) {
        detail::rollback(db.get());
        std::optional<SourceFeaturePropertiesRecord> raced;
        status = detail::read_existing_feature_properties(project, record.source_id, raced);
        if (!status) return {std::move(status), false, false};
        if (raced.has_value() && detail::equal_feature_properties(*raced, record)) {
            status = project.refresh_metadata();
            if (!status) return {std::move(status), false, false};
            return {Status::success(), false, false};
        }
        return {{StorageError::record_exists,
                 "concurrent feature property writer committed different immutable data"},
                false, false};
    }

    if (!(status = detail::insert_feature_properties(db.get(), record)) ||
        !(status = advance_project_revision(db.get(), project, modified_utc))) {
        detail::rollback(db.get());
        return {std::move(status), false, false};
    }
    if (!(status = detail::commit(db.get()))) {
        detail::rollback(db.get());
        return {std::move(status), false, false};
    }
    status = project.refresh_metadata();
    if (!status) return {std::move(status), true, true};
    return {Status::success(), true, true};
}

}  // namespace aeris::storage
