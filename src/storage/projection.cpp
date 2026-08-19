// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#include "aeris/storage/projection.hpp"

#include "projection_detail.hpp"
#include "sqlite_detail.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#include <sqlite3.h>

namespace aeris::storage {
namespace {

static_assert(sizeof(double) == 8U, "AERIS projection storage requires 64-bit double");
static_assert(std::numeric_limits<double>::is_iec559,
              "AERIS projection storage requires IEC 60559 / IEEE-754 binary64");

constexpr std::size_t kMaxIdentifierBytes = 255U;
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kTwoPi = 2.0 * kPi;

[[nodiscard]] bool bounded_id(const std::string& value) noexcept {
    return !value.empty() && value.size() <= kMaxIdentifierBytes &&
           value.find('\0') == std::string::npos;
}

[[nodiscard]] double canonical_zero(const double value) noexcept {
    return value == 0.0 ? 0.0 : value;
}

[[nodiscard]] double canonical_longitude(const double value) noexcept {
    double normalized = std::fmod(value, kTwoPi);
    if (normalized <= -kPi) normalized += kTwoPi;
    if (normalized > kPi) normalized -= kTwoPi;
    return canonical_zero(normalized);
}

[[nodiscard]] Status canonicalize_and_validate(ProjectProjectionRecord& record) {
    if (!bounded_id(record.model_id) || !bounded_id(record.cut_model_id)) {
        return {StorageError::invalid_argument,
                "projection/cut model ID violates canonical identifier bounds"};
    }
    if (!std::isfinite(record.central_meridian_rad)) {
        return {StorageError::invalid_argument,
                "projection central meridian must be finite"};
    }
    record.central_meridian_rad = canonical_longitude(record.central_meridian_rad);

    const std::string_view model(record.model_id);
    const std::string_view cut(record.cut_model_id);
    if (model == kProjectionModelUnspecified) {
        if (record.central_meridian_rad != 0.0 ||
            cut != kProjectionCutModelUnspecifiedV1) {
            return {StorageError::invalid_argument,
                    "unspecified projection requires zero central meridian and unspecified cut model"};
        }
    } else if (model == kProjectionModelSinusoidalV1 ||
               model == kProjectionModelMollweideV1 ||
               model == kProjectionModelLambertCylindricalEqualAreaV1) {
        if (cut != kProjectionCutModelSingleAntimeridianV1) {
            return {StorageError::invalid_argument,
                    "current built-in equal-area projection requires single-antimeridian cut model"};
        }
    }
    return Status::success();
}

[[nodiscard]] std::array<unsigned char, 8> encode_f64le(const double value) noexcept {
    std::uint64_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    std::array<unsigned char, 8> bytes{};
    for (unsigned index = 0U; index < 8U; ++index) {
        bytes[index] = static_cast<unsigned char>((bits >> (index * 8U)) & 0xffU);
    }
    return bytes;
}

[[nodiscard]] double decode_f64le(const unsigned char* bytes) noexcept {
    std::uint64_t bits = 0U;
    for (unsigned index = 0U; index < 8U; ++index) {
        bits |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
    }
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

[[nodiscard]] std::string text_column(sqlite3_stmt* stmt, const int column) {
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, column));
    const int bytes = sqlite3_column_bytes(stmt, column);
    return std::string(text, static_cast<std::size_t>(bytes));
}

[[nodiscard]] Status validate_project_connection(sqlite3* db, const ProjectStore& project) {
    Status status = detail::verify_quick_check(db);
    if (!status) return status;

    sqlite3_int64 application_id = 0;
    if (!(status = detail::query_single_int(db, "PRAGMA application_id;", application_id))) return status;
    if (application_id != static_cast<sqlite3_int64>(kProjectApplicationId)) {
        return {StorageError::invalid_application_id,
                "projection target is not an AERIS project"};
    }

    sqlite3_int64 generation = 0;
    if (!(status = detail::query_single_int(db, "PRAGMA user_version;", generation))) return status;
    if (generation != kProjectSchemaGeneration) {
        return {StorageError::unsupported_schema,
                "projection target has unsupported draft schema generation"};
    }

    std::string uuid;
    if (!(status = detail::query_single_text(
              db, "SELECT project_uuid FROM aeris_meta WHERE id=1;", uuid))) return status;
    if (uuid != project.metadata().project_uuid) {
        return {StorageError::schema_invalid,
                "projection target UUID differs from validated project handle"};
    }
    return Status::success();
}

[[nodiscard]] Status read_projection(
    sqlite3* db,
    ProjectProjectionRecord& record) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(
        db,
        "SELECT model_id,central_meridian_f64le,cut_model_id FROM aeris_projection WHERE id=1;",
        stmt);
    if (!status) return status;
    const int rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_ROW || sqlite3_column_type(stmt.get(), 0) != SQLITE_TEXT ||
        sqlite3_column_type(stmt.get(), 1) != SQLITE_BLOB ||
        sqlite3_column_type(stmt.get(), 2) != SQLITE_TEXT) {
        return {StorageError::schema_invalid,
                "structured projection singleton is missing or has malformed SQLite types"};
    }

    const int central_bytes = sqlite3_column_bytes(stmt.get(), 1);
    const auto* central_blob = static_cast<const unsigned char*>(sqlite3_column_blob(stmt.get(), 1));
    if (central_bytes != 8 || central_blob == nullptr) {
        return {StorageError::schema_invalid,
                "projection central meridian is not one canonical binary64 value"};
    }

    record.model_id = text_column(stmt.get(), 0);
    record.central_meridian_rad = decode_f64le(central_blob);
    record.cut_model_id = text_column(stmt.get(), 2);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return {StorageError::schema_invalid,
                "structured projection contains more than the singleton row"};
    }

    ProjectProjectionRecord canonical = record;
    status = canonicalize_and_validate(canonical);
    if (!status) {
        return {StorageError::schema_invalid,
                "stored projection violates semantic contract: " + status.diagnostic};
    }
    if (canonical.central_meridian_rad != record.central_meridian_rad ||
        (record.central_meridian_rad == 0.0 && std::signbit(record.central_meridian_rad))) {
        return {StorageError::schema_invalid,
                "stored projection central meridian is not in canonical longitude form"};
    }
    return Status::success();
}

[[nodiscard]] Status validate_metadata_sync(
    sqlite3* db,
    const ProjectProjectionRecord& record) {
    std::string projection_id;
    Status status = detail::query_single_text(
        db, "SELECT projection_id FROM aeris_meta WHERE id=1;", projection_id);
    if (!status) return status;
    if (projection_id != record.model_id) {
        return {StorageError::schema_invalid,
                "aeris_meta projection_id disagrees with structured projection model"};
    }
    return Status::success();
}

[[nodiscard]] bool equal_projection(
    const ProjectProjectionRecord& a,
    const ProjectProjectionRecord& b) noexcept {
    return a.model_id == b.model_id &&
           a.central_meridian_rad == b.central_meridian_rad &&
           a.cut_model_id == b.cut_model_id;
}

[[nodiscard]] Status read_revision(sqlite3* db, std::uint64_t& revision) {
    sqlite3_int64 raw = 0;
    Status status = detail::query_single_int(
        db, "SELECT revision FROM aeris_meta WHERE id=1;", raw);
    if (!status) return status;
    if (raw < 0) {
        return {StorageError::schema_invalid,
                "project revision is negative during projection mutation"};
    }
    revision = static_cast<std::uint64_t>(raw);
    return Status::success();
}

}  // namespace

ProjectionMutationResult set_project_projection(
    ProjectStore& project,
    const ProjectProjectionRecord& input,
    const std::string_view modified_utc) {
    if (!is_canonical_utc_timestamp(modified_utc)) {
        return {{StorageError::invalid_argument,
                 "projection mutation time is not canonical Gregorian UTC"},
                false, false};
    }

    ProjectProjectionRecord record = input;
    Status status = canonicalize_and_validate(record);
    if (!status) return {std::move(status), false, false};

    detail::DbPtr db;
    status = detail::open_database(project.path(), SQLITE_OPEN_READWRITE, db);
    if (!status) return {std::move(status), false, false};
    if (!(status = validate_project_connection(db.get(), project))) {
        return {std::move(status), false, false};
    }
    if (!(status = detail::configure_durable(db.get()))) {
        return {std::move(status), false, false};
    }
    if (!(status = detail::begin_immediate(db.get()))) {
        return {std::move(status), false, false};
    }

    ProjectProjectionRecord existing{};
    status = read_projection(db.get(), existing);
    if (status) status = validate_metadata_sync(db.get(), existing);
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false};
    }
    if (equal_projection(existing, record)) {
        detail::rollback(db.get());
        status = project.refresh_metadata();
        if (!status) return {std::move(status), false, false};
        return {Status::success(), false, false};
    }

    const auto central = encode_f64le(record.central_meridian_rad);
    detail::StmtPtr projection_stmt;
    status = detail::prepare(
        db.get(),
        "UPDATE aeris_projection SET model_id=?,central_meridian_f64le=?,cut_model_id=? WHERE id=1;",
        projection_stmt);
    if (status) status = detail::bind_text(db.get(), projection_stmt.get(), 1, record.model_id);
    if (status && sqlite3_bind_blob(
            projection_stmt.get(), 2, central.data(), static_cast<int>(central.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
        status = {StorageError::sqlite_failure,
                  detail::sqlite_message(db.get(), "sqlite projection central-meridian bind failed")};
    }
    if (status) status = detail::bind_text(db.get(), projection_stmt.get(), 3, record.cut_model_id);
    if (status) status = detail::step_done(db.get(), projection_stmt.get());
    if (status && sqlite3_changes(db.get()) != 1) {
        status = {StorageError::schema_invalid,
                  "projection update did not affect exactly one singleton row"};
    }

    std::uint64_t revision = 0U;
    if (status) status = read_revision(db.get(), revision);
    if (status && revision >= static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max())) {
        status = {StorageError::schema_invalid,
                  "project revision exhausted during projection mutation"};
    }

    detail::StmtPtr meta_stmt;
    if (status) status = detail::prepare(
        db.get(),
        "UPDATE aeris_meta SET revision=?,modified_utc=?,projection_id=? "
        "WHERE id=1 AND project_uuid=?;",
        meta_stmt);
    if (status) status = detail::bind_int64(
        db.get(), meta_stmt.get(), 1, static_cast<sqlite3_int64>(revision + 1U));
    if (status) status = detail::bind_text(db.get(), meta_stmt.get(), 2, std::string(modified_utc));
    if (status) status = detail::bind_text(db.get(), meta_stmt.get(), 3, record.model_id);
    if (status) status = detail::bind_text(db.get(), meta_stmt.get(), 4, project.metadata().project_uuid);
    if (status) status = detail::step_done(db.get(), meta_stmt.get());
    if (status && sqlite3_changes(db.get()) != 1) {
        status = {StorageError::schema_invalid,
                  "projection mutation did not advance exactly one metadata row"};
    }

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

ProjectProjectionResult load_project_projection(const ProjectStore& project) {
    detail::DbPtr db;
    Status status = detail::open_database(project.path(), SQLITE_OPEN_READONLY, db);
    if (!status) return {std::move(status), {}};
    if (!(status = validate_project_connection(db.get(), project))) {
        return {std::move(status), {}};
    }

    ProjectProjectionResult result{};
    status = read_projection(db.get(), result.record);
    if (status) status = validate_metadata_sync(db.get(), result.record);
    result.status = std::move(status);
    if (!result.status) result.record = {};
    return result;
}

namespace detail {

Status verify_projection_semantics(const ProjectStore& project) {
    const ProjectProjectionResult result = load_project_projection(project);
    return result.status;
}

}  // namespace detail
}  // namespace aeris::storage
