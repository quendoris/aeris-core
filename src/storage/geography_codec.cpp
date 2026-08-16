// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "geography_codec.hpp"

#include "sqlite_detail.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <set>
#include <utility>
#include <vector>

#include <sqlite3.h>

namespace aeris::storage::detail {
namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kHalfPi = 0.5 * kPi;
constexpr double kTwoPi = 2.0 * kPi;
constexpr std::size_t kMaxIdentifierBytes = 255U;
constexpr std::size_t kEncodedPointBytes = 16U;
constexpr std::size_t kMaxVerticesPerRing =
    static_cast<std::size_t>(std::numeric_limits<int>::max()) / kEncodedPointBytes;

static_assert(sizeof(double) == 8U, "AERIS geographic storage requires binary64 double");
static_assert(std::numeric_limits<double>::is_iec559,
              "AERIS geographic storage requires IEC 60559 / IEEE-754 binary64 double");

[[nodiscard]] bool bounded_identifier(const std::string& value) noexcept {
    return !value.empty() && value.size() <= kMaxIdentifierBytes &&
           value.find('\0') == std::string::npos;
}

[[nodiscard]] double canonical_zero(const double value) noexcept {
    return value == 0.0 ? 0.0 : value;
}

[[nodiscard]] bool is_negative_zero(const double value) noexcept {
    return value == 0.0 && std::signbit(value);
}

[[nodiscard]] bool valid_role(const GeographicRingRole role) noexcept {
    switch (role) {
        case GeographicRingRole::exterior:
        case GeographicRingRole::interior:
            return true;
    }
    return false;
}

[[nodiscard]] bool valid_interior_side(const GeographicInteriorSide side) noexcept {
    switch (side) {
        case GeographicInteriorSide::unspecified:
        case GeographicInteriorSide::left:
        case GeographicInteriorSide::right:
            return true;
    }
    return false;
}

[[nodiscard]] bool ambiguous_half_turn(const double delta) noexcept {
    constexpr double tolerance =
        64.0 * std::numeric_limits<double>::epsilon() * kPi;
    return std::abs(std::abs(delta) - kPi) <= tolerance;
}

[[nodiscard]] bool canonical_edge_delta(const double delta) noexcept {
    return std::isfinite(delta) && delta >= -kPi && delta <= kPi &&
           !ambiguous_half_turn(delta);
}

Status validate_ring(const GeographicRingRecord& ring, const bool require_canonical_zero) {
    if (!valid_role(ring.role) || !valid_interior_side(ring.interior_side)) {
        return {StorageError::invalid_argument, "geographic ring contains an unsupported role/interior-side value"};
    }
    if (ring.vertices.size() < 3U) {
        return {StorageError::invalid_argument, "geographic ring contains fewer than three canonical vertices"};
    }
    if (ring.vertices.size() > kMaxVerticesPerRing) {
        return {StorageError::invalid_argument, "geographic ring exceeds the SQLite binary64 BLOB length domain"};
    }
    if (!std::isfinite(ring.closing_longitude_rad)) {
        return {StorageError::invalid_argument, "geographic ring closing longitude is non-finite"};
    }
    if (require_canonical_zero && is_negative_zero(ring.closing_longitude_rad)) {
        return {StorageError::invalid_argument, "stored geographic ring contains noncanonical negative zero"};
    }
    if (ring.longitude_winding != 0 &&
        ring.interior_side == GeographicInteriorSide::unspecified) {
        return {StorageError::invalid_argument, "winding geographic ring requires an explicit interior side"};
    }

    for (const GeographicPointRecord& point : ring.vertices) {
        if (!std::isfinite(point.longitude_rad) || !std::isfinite(point.latitude_rad)) {
            return {StorageError::invalid_argument, "geographic ring contains a non-finite coordinate"};
        }
        if (point.latitude_rad < -kHalfPi || point.latitude_rad > kHalfPi) {
            return {StorageError::invalid_argument, "geographic ring latitude lies outside the WGS84 domain"};
        }
        if (require_canonical_zero &&
            (is_negative_zero(point.longitude_rad) || is_negative_zero(point.latitude_rad))) {
            return {StorageError::invalid_argument, "stored geographic ring contains noncanonical negative zero"};
        }
    }

    const double first_longitude = ring.vertices.front().longitude_rad;
    if (!(first_longitude > -kPi && first_longitude <= kPi)) {
        return {StorageError::invalid_argument, "geographic ring first longitude is not in canonical (-pi,pi] form"};
    }
    if (ring.vertices.size() >= 4U &&
        ring.vertices.back().latitude_rad == ring.vertices.front().latitude_rad &&
        std::remainder(
            ring.vertices.back().longitude_rad - ring.vertices.front().longitude_rad,
            kTwoPi) == 0.0) {
        return {StorageError::invalid_argument, "geographic ring contains a duplicate terminal closing vertex"};
    }

    for (std::size_t index = 1U; index < ring.vertices.size(); ++index) {
        const double delta =
            ring.vertices[index].longitude_rad - ring.vertices[index - 1U].longitude_rad;
        if (!canonical_edge_delta(delta)) {
            return {StorageError::invalid_argument, "geographic ring contains a noncanonical longitude edge delta"};
        }
    }

    const double closing_delta =
        ring.closing_longitude_rad - ring.vertices.back().longitude_rad;
    if (!canonical_edge_delta(closing_delta)) {
        return {StorageError::invalid_argument, "geographic ring closing edge has a noncanonical longitude delta"};
    }

    const double turns =
        (ring.closing_longitude_rad - first_longitude) / kTwoPi;
    if (!std::isfinite(turns)) {
        return {StorageError::invalid_argument, "geographic ring winding relation is non-finite"};
    }
    const double rounded_turns = std::round(turns);
    constexpr double winding_tolerance = 256.0 * std::numeric_limits<double>::epsilon();
    if (!std::isfinite(rounded_turns) ||
        std::abs(turns - rounded_turns) > winding_tolerance ||
        rounded_turns < static_cast<double>(std::numeric_limits<std::int32_t>::min()) ||
        rounded_turns > static_cast<double>(std::numeric_limits<std::int32_t>::max()) ||
        rounded_turns != static_cast<double>(ring.longitude_winding)) {
        return {StorageError::invalid_argument, "geographic ring closing longitude disagrees with longitude winding"};
    }
    return Status::success();
}

void normalize_ring_zeros(GeographicRingRecord& ring) noexcept {
    ring.closing_longitude_rad = canonical_zero(ring.closing_longitude_rad);
    for (GeographicPointRecord& point : ring.vertices) {
        point.longitude_rad = canonical_zero(point.longitude_rad);
        point.latitude_rad = canonical_zero(point.latitude_rad);
    }
}

[[nodiscard]] bool equal_point(
    const GeographicPointRecord& a,
    const GeographicPointRecord& b) noexcept {
    return a.longitude_rad == b.longitude_rad && a.latitude_rad == b.latitude_rad;
}

[[nodiscard]] bool equal_ring(
    const GeographicRingRecord& a,
    const GeographicRingRecord& b) noexcept {
    if (a.role != b.role || a.interior_side != b.interior_side ||
        a.closing_longitude_rad != b.closing_longitude_rad ||
        a.longitude_winding != b.longitude_winding ||
        a.vertices.size() != b.vertices.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < a.vertices.size(); ++index) {
        if (!equal_point(a.vertices[index], b.vertices[index])) return false;
    }
    return true;
}

[[nodiscard]] bool equal_feature(
    const SourceFeatureRecord& a,
    const SourceFeatureRecord& b) noexcept {
    if (a.stable_id != b.stable_id || a.source_id != b.source_id ||
        a.rings.size() != b.rings.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < a.rings.size(); ++index) {
        if (!equal_ring(a.rings[index], b.rings[index])) return false;
    }
    return true;
}

void append_binary64_le(std::vector<unsigned char>& bytes, const double value) {
    std::uint64_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        bytes.push_back(static_cast<unsigned char>((bits >> shift) & 0xffU));
    }
}

[[nodiscard]] std::vector<unsigned char> encode_vertices(
    const std::vector<GeographicPointRecord>& vertices) {
    std::vector<unsigned char> bytes;
    bytes.reserve(vertices.size() * kEncodedPointBytes);
    for (const GeographicPointRecord& point : vertices) {
        append_binary64_le(bytes, point.longitude_rad);
        append_binary64_le(bytes, point.latitude_rad);
    }
    return bytes;
}

[[nodiscard]] double decode_binary64_le(const unsigned char* bytes) noexcept {
    std::uint64_t bits = 0U;
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        bits |= static_cast<std::uint64_t>(bytes[shift / 8U]) << shift;
    }
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

Status reset_statement(sqlite3* db, sqlite3_stmt* stmt) {
    int rc = sqlite3_reset(stmt);
    if (rc != SQLITE_OK) {
        return {StorageError::sqlite_failure, sqlite_message(db, "sqlite statement reset failed")};
    }
    rc = sqlite3_clear_bindings(stmt);
    if (rc != SQLITE_OK) {
        return {StorageError::sqlite_failure, sqlite_message(db, "sqlite clear bindings failed")};
    }
    return Status::success();
}

Status bind_blob(
    sqlite3* db,
    sqlite3_stmt* stmt,
    const int index,
    const std::vector<unsigned char>& bytes) {
    const int rc = sqlite3_bind_blob64(
        stmt,
        index,
        bytes.data(),
        static_cast<sqlite3_uint64>(bytes.size()),
        SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        return {StorageError::sqlite_failure, sqlite_message(db, "sqlite bind geographic BLOB failed")};
    }
    return Status::success();
}

Status load_rings(
    sqlite3* db,
    const std::string& source_id,
    SourceFeatureRecord& feature) {
    StmtPtr stmt;
    Status status = prepare(
        db,
        "SELECT ring_index,role,interior_side,closing_longitude_rad,longitude_winding,vertex_count,vertices_le_f64 "
        "FROM aeris_source_ring WHERE source_id=? AND feature_id=? ORDER BY ring_index;",
        stmt);
    if (!status) return status;
    if (!(status = bind_text(db, stmt.get(), 1, source_id))) return status;
    if (!(status = bind_text(db, stmt.get(), 2, feature.stable_id))) return status;

    sqlite3_int64 expected_index = 0;
    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            return {StorageError::sqlite_failure, sqlite_message(db, "could not read stored geographic ring")};
        }
        for (int column : {0, 1, 2, 4, 5}) {
            if (sqlite3_column_type(stmt.get(), column) != SQLITE_INTEGER) {
                return {StorageError::schema_invalid, "stored geographic ring contains an invalid integer column type"};
            }
        }
        const int closing_type = sqlite3_column_type(stmt.get(), 3);
        if (closing_type != SQLITE_FLOAT && closing_type != SQLITE_INTEGER) {
            return {StorageError::schema_invalid, "stored geographic ring closing longitude is not numeric"};
        }
        if (sqlite3_column_type(stmt.get(), 6) != SQLITE_BLOB) {
            return {StorageError::schema_invalid, "stored geographic ring vertex stream is not a BLOB"};
        }

        const sqlite3_int64 ring_index = sqlite3_column_int64(stmt.get(), 0);
        const sqlite3_int64 role = sqlite3_column_int64(stmt.get(), 1);
        const sqlite3_int64 side = sqlite3_column_int64(stmt.get(), 2);
        const double closing = sqlite3_column_double(stmt.get(), 3);
        const sqlite3_int64 winding = sqlite3_column_int64(stmt.get(), 4);
        const sqlite3_int64 vertex_count = sqlite3_column_int64(stmt.get(), 5);
        if (ring_index != expected_index) {
            return {StorageError::schema_invalid, "stored geographic ring indexes are not contiguous from zero"};
        }
        if (role < 0 || role > 1 || side < 0 || side > 2 ||
            winding < static_cast<sqlite3_int64>(std::numeric_limits<std::int32_t>::min()) ||
            winding > static_cast<sqlite3_int64>(std::numeric_limits<std::int32_t>::max()) ||
            vertex_count < 3 ||
            vertex_count > static_cast<sqlite3_int64>(kMaxVerticesPerRing)) {
            return {StorageError::schema_invalid, "stored geographic ring metadata lies outside canonical bounds"};
        }

        const std::size_t count = static_cast<std::size_t>(vertex_count);
        const std::size_t expected_bytes = count * kEncodedPointBytes;
        const int byte_count = sqlite3_column_bytes(stmt.get(), 6);
        if (byte_count < 0 || static_cast<std::size_t>(byte_count) != expected_bytes) {
            return {StorageError::schema_invalid, "stored geographic ring BLOB length disagrees with vertex_count"};
        }
        const auto* blob = static_cast<const unsigned char*>(sqlite3_column_blob(stmt.get(), 6));
        if (blob == nullptr) {
            return {StorageError::schema_invalid, "stored geographic ring BLOB is unexpectedly NULL"};
        }

        GeographicRingRecord ring{};
        ring.role = static_cast<GeographicRingRole>(role);
        ring.interior_side = static_cast<GeographicInteriorSide>(side);
        ring.closing_longitude_rad = closing;
        ring.longitude_winding = static_cast<std::int32_t>(winding);
        ring.vertices.reserve(count);
        for (std::size_t index = 0U; index < count; ++index) {
            const std::size_t offset = index * kEncodedPointBytes;
            const double longitude = decode_binary64_le(blob + offset);
            const double latitude = decode_binary64_le(blob + offset + 8U);
            if (!std::isfinite(longitude) || !std::isfinite(latitude) ||
                is_negative_zero(longitude) || is_negative_zero(latitude)) {
                return {StorageError::schema_invalid, "stored geographic ring BLOB contains a noncanonical binary64 coordinate"};
            }
            ring.vertices.push_back({longitude, latitude});
        }
        if (!std::isfinite(ring.closing_longitude_rad) ||
            is_negative_zero(ring.closing_longitude_rad)) {
            return {StorageError::schema_invalid, "stored geographic ring closing longitude is noncanonical"};
        }
        status = validate_ring(ring, true);
        if (!status) {
            return {StorageError::schema_invalid, "stored geographic ring violates canonical WGS84 semantics: " + status.diagnostic};
        }
        feature.rings.push_back(std::move(ring));
        ++expected_index;
    }
    if (feature.rings.empty()) {
        return {StorageError::schema_invalid, "stored geographic feature contains no rings"};
    }
    return Status::success();
}

}  // namespace

Status canonicalize_feature_records(std::vector<SourceFeatureRecord>& features) {
    if (features.size() > static_cast<std::size_t>(std::numeric_limits<sqlite3_int64>::max())) {
        return {StorageError::invalid_argument, "source dataset feature count exceeds SQLite signed integer range"};
    }

    for (SourceFeatureRecord& feature : features) {
        if (!bounded_identifier(feature.stable_id) || !bounded_identifier(feature.source_id)) {
            return {StorageError::invalid_argument, "source feature identifier is empty, contains NUL, or exceeds 255 bytes"};
        }
        if (feature.rings.empty()) {
            return {StorageError::invalid_argument, "source feature contains no geographic rings"};
        }
        if (feature.rings.size() > static_cast<std::size_t>(std::numeric_limits<sqlite3_int64>::max())) {
            return {StorageError::invalid_argument, "source feature ring count exceeds SQLite signed integer range"};
        }
        for (GeographicRingRecord& ring : feature.rings) {
            normalize_ring_zeros(ring);
            Status status = validate_ring(ring, true);
            if (!status) return status;
        }
    }

    std::sort(features.begin(), features.end(), [](const SourceFeatureRecord& a, const SourceFeatureRecord& b) {
        return a.stable_id < b.stable_id;
    });
    for (std::size_t index = 1U; index < features.size(); ++index) {
        if (features[index - 1U].stable_id == features[index].stable_id) {
            return {StorageError::invalid_argument, "source dataset contains a duplicate stable feature ID"};
        }
    }
    return Status::success();
}

bool equal_feature_records(
    const std::vector<SourceFeatureRecord>& a,
    const std::vector<SourceFeatureRecord>& b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t index = 0U; index < a.size(); ++index) {
        if (!equal_feature(a[index], b[index])) return false;
    }
    return true;
}

Status insert_feature_records(
    sqlite3* db,
    const std::string& source_id,
    const std::vector<SourceFeatureRecord>& features) {
    StmtPtr feature_stmt;
    Status status = prepare(
        db,
        "INSERT INTO aeris_source_feature(source_id,feature_id,source_feature_id) VALUES(?,?,?);",
        feature_stmt);
    if (!status) return status;

    StmtPtr ring_stmt;
    status = prepare(
        db,
        "INSERT INTO aeris_source_ring(source_id,feature_id,ring_index,role,interior_side,closing_longitude_rad,"
        "longitude_winding,vertex_count,vertices_le_f64) VALUES(?,?,?,?,?,?,?,?,?);",
        ring_stmt);
    if (!status) return status;

    for (const SourceFeatureRecord& feature : features) {
        if (!(status = bind_text(db, feature_stmt.get(), 1, source_id))) return status;
        if (!(status = bind_text(db, feature_stmt.get(), 2, feature.stable_id))) return status;
        if (!(status = bind_text(db, feature_stmt.get(), 3, feature.source_id))) return status;
        if (!(status = step_done(db, feature_stmt.get()))) return status;
        if (!(status = reset_statement(db, feature_stmt.get()))) return status;

        for (std::size_t ring_index = 0U; ring_index < feature.rings.size(); ++ring_index) {
            const GeographicRingRecord& ring = feature.rings[ring_index];
            const std::vector<unsigned char> encoded = encode_vertices(ring.vertices);
            if (!(status = bind_text(db, ring_stmt.get(), 1, source_id))) return status;
            if (!(status = bind_text(db, ring_stmt.get(), 2, feature.stable_id))) return status;
            if (!(status = bind_int64(db, ring_stmt.get(), 3, static_cast<sqlite3_int64>(ring_index)))) return status;
            if (!(status = bind_int64(db, ring_stmt.get(), 4, static_cast<sqlite3_int64>(ring.role)))) return status;
            if (!(status = bind_int64(db, ring_stmt.get(), 5, static_cast<sqlite3_int64>(ring.interior_side)))) return status;
            if (!(status = bind_double(db, ring_stmt.get(), 6, ring.closing_longitude_rad))) return status;
            if (!(status = bind_int64(db, ring_stmt.get(), 7, static_cast<sqlite3_int64>(ring.longitude_winding)))) return status;
            if (!(status = bind_int64(db, ring_stmt.get(), 8, static_cast<sqlite3_int64>(ring.vertices.size())))) return status;
            if (!(status = bind_blob(db, ring_stmt.get(), 9, encoded))) return status;
            if (!(status = step_done(db, ring_stmt.get()))) return status;
            if (!(status = reset_statement(db, ring_stmt.get()))) return status;
        }
    }
    return Status::success();
}

Status load_feature_records(
    sqlite3* db,
    const std::string& source_id,
    std::vector<SourceFeatureRecord>& features) {
    StmtPtr stmt;
    Status status = prepare(
        db,
        "SELECT feature_id,source_feature_id FROM aeris_source_feature WHERE source_id=? ORDER BY feature_id;",
        stmt);
    if (!status) return status;
    if (!(status = bind_text(db, stmt.get(), 1, source_id))) return status;

    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            return {StorageError::sqlite_failure, sqlite_message(db, "could not read stored source feature")};
        }
        if (sqlite3_column_type(stmt.get(), 0) != SQLITE_TEXT ||
            sqlite3_column_type(stmt.get(), 1) != SQLITE_TEXT) {
            return {StorageError::schema_invalid, "stored source feature contains an invalid text column type"};
        }
        const auto text_at = [&](const int column) {
            const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), column));
            const int bytes = sqlite3_column_bytes(stmt.get(), column);
            return std::string(text, static_cast<std::size_t>(bytes));
        };

        SourceFeatureRecord feature{};
        feature.stable_id = text_at(0);
        feature.source_id = text_at(1);
        if (!bounded_identifier(feature.stable_id) || !bounded_identifier(feature.source_id)) {
            return {StorageError::schema_invalid, "stored source feature identifier violates canonical bounds"};
        }
        if (!(status = load_rings(db, source_id, feature))) return status;
        features.push_back(std::move(feature));
    }

    for (std::size_t index = 1U; index < features.size(); ++index) {
        if (!(features[index - 1U].stable_id < features[index].stable_id)) {
            return {StorageError::schema_invalid, "stored source feature IDs are not unique under deterministic ordering"};
        }
    }
    return Status::success();
}

}  // namespace aeris::storage::detail
