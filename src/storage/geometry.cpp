// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#include "aeris/storage/geometry.hpp"

#include "sqlite_detail.hpp"

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

static_assert(sizeof(double) == 8U, "AERIS canonical geometry requires 64-bit double");
static_assert(std::numeric_limits<double>::is_iec559,
              "AERIS canonical geometry requires IEC 60559 / IEEE-754 binary64");

constexpr std::size_t kMaxIdentifierBytes = 255U;
constexpr std::size_t kMaxFeaturesPerSource = 1'000'000U;
constexpr std::size_t kMaxRingsPerFeature = 65'535U;
constexpr std::size_t kMaxVerticesPerRing = 4'194'304U;
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kHalfPi = 0.5 * kPi;
constexpr double kTwoPi = 2.0 * kPi;
constexpr double kHalfTurnTolerance =
    64.0 * std::numeric_limits<double>::epsilon() * kPi;
constexpr double kWindingTolerance =
    256.0 * std::numeric_limits<double>::epsilon();

[[nodiscard]] bool bounded_identifier(const std::string& value) noexcept {
    return !value.empty() && value.size() <= kMaxIdentifierBytes &&
           value.find('\0') == std::string::npos;
}

[[nodiscard]] double canonical_zero(const double value) noexcept {
    return value == 0.0 ? 0.0 : value;
}

[[nodiscard]] bool valid_role(const StoredRingRole role) noexcept {
    const auto value = static_cast<std::uint8_t>(role);
    return value <= static_cast<std::uint8_t>(StoredRingRole::interior);
}

[[nodiscard]] bool valid_interior_side(const StoredInteriorSide side) noexcept {
    const auto value = static_cast<std::uint8_t>(side);
    return value <= static_cast<std::uint8_t>(StoredInteriorSide::right);
}

[[nodiscard]] bool ambiguous_half_turn(const double delta) noexcept {
    return std::abs(std::abs(delta) - kPi) <= kHalfTurnTolerance;
}

Status validate_ring(const GeographicRingRecord& ring) {
    if (!valid_role(ring.role) || !valid_interior_side(ring.interior_side)) {
        return {StorageError::invalid_argument, "feature ring role/interior-side value is outside the canonical enum domain"};
    }
    if (ring.vertices.size() < 3U || ring.vertices.size() > kMaxVerticesPerRing) {
        return {StorageError::invalid_argument, "feature ring vertex count is outside the draft 3..4194304 bound"};
    }
    if (!std::isfinite(ring.closing_longitude_rad)) {
        return {StorageError::invalid_argument, "feature ring closing longitude is non-finite"};
    }
    if (ring.longitude_winding != 0 && ring.interior_side == StoredInteriorSide::unspecified) {
        return {StorageError::invalid_argument, "winding feature ring requires an explicit interior side"};
    }

    for (const GeographicPointRecord point : ring.vertices) {
        if (!std::isfinite(point.longitude_rad) || !std::isfinite(point.latitude_rad)) {
            return {StorageError::invalid_argument, "feature ring contains a non-finite coordinate"};
        }
        if (point.latitude_rad < -kHalfPi || point.latitude_rad > kHalfPi) {
            return {StorageError::invalid_argument, "feature ring latitude is outside WGS84 geographic range"};
        }
    }

    const double first_longitude = ring.vertices.front().longitude_rad;
    if (!(first_longitude > -kPi && first_longitude <= kPi)) {
        return {StorageError::invalid_argument, "canonical feature ring first longitude is outside (-pi,pi]"};
    }

    for (std::size_t index = 1U; index < ring.vertices.size(); ++index) {
        const double delta = ring.vertices[index].longitude_rad -
                             ring.vertices[index - 1U].longitude_rad;
        if (!std::isfinite(delta) || std::abs(delta) > kPi || ambiguous_half_turn(delta)) {
            return {StorageError::invalid_argument, "feature ring contains a noncanonical longitude edge"};
        }
    }

    const double closing_delta = ring.closing_longitude_rad -
                                 ring.vertices.back().longitude_rad;
    if (!std::isfinite(closing_delta) || std::abs(closing_delta) > kPi ||
        ambiguous_half_turn(closing_delta)) {
        return {StorageError::invalid_argument, "feature ring contains a noncanonical closing longitude edge"};
    }

    const double turns = (ring.closing_longitude_rad - first_longitude) / kTwoPi;
    if (!std::isfinite(turns) ||
        turns < static_cast<double>(std::numeric_limits<std::int32_t>::min()) - 0.5 ||
        turns > static_cast<double>(std::numeric_limits<std::int32_t>::max()) + 0.5) {
        return {StorageError::invalid_argument, "feature ring longitude winding is outside canonical numeric range"};
    }
    const long long rounded = std::llround(turns);
    if (std::abs(turns - static_cast<double>(rounded)) > kWindingTolerance ||
        rounded != static_cast<long long>(ring.longitude_winding)) {
        return {StorageError::invalid_argument, "feature ring closing longitude disagrees with longitude winding"};
    }

    return Status::success();
}

Status validate_feature(const FeatureGeometryRecord& feature) {
    if (!bounded_identifier(feature.stable_id) || !bounded_identifier(feature.source_feature_id)) {
        return {StorageError::invalid_argument, "feature stable/source identifier is empty, contains NUL, or exceeds 255 bytes"};
    }
    if (feature.rings.empty() || feature.rings.size() > kMaxRingsPerFeature) {
        return {StorageError::invalid_argument, "feature ring count is outside the draft 1..65535 bound"};
    }
    for (const GeographicRingRecord& ring : feature.rings) {
        Status status = validate_ring(ring);
        if (!status) return status;
    }
    return Status::success();
}

Status validate_source_geometry(const SourceGeometryRecord& record) {
    if (!bounded_identifier(record.source_id)) {
        return {StorageError::invalid_argument, "geometry source ID is empty, contains NUL, or exceeds 255 bytes"};
    }
    if (record.features.size() > kMaxFeaturesPerSource) {
        return {StorageError::invalid_argument, "source geometry exceeds the 1000000-feature draft bound"};
    }

    std::set<std::string> stable_ids;
    std::set<std::string> source_feature_ids;
    for (const FeatureGeometryRecord& feature : record.features) {
        Status status = validate_feature(feature);
        if (!status) return status;
        if (!stable_ids.insert(feature.stable_id).second) {
            return {StorageError::invalid_argument, "source geometry contains a duplicate stable feature ID"};
        }
        if (!source_feature_ids.insert(feature.source_feature_id).second) {
            return {StorageError::invalid_argument, "source geometry contains a duplicate source feature ID"};
        }
    }
    return Status::success();
}

void canonicalize_source_geometry(SourceGeometryRecord& record) {
    for (FeatureGeometryRecord& feature : record.features) {
        for (GeographicRingRecord& ring : feature.rings) {
            ring.closing_longitude_rad = canonical_zero(ring.closing_longitude_rad);
            for (GeographicPointRecord& point : ring.vertices) {
                point.longitude_rad = canonical_zero(point.longitude_rad);
                point.latitude_rad = canonical_zero(point.latitude_rad);
            }
        }
    }
    std::sort(record.features.begin(), record.features.end(),
              [](const FeatureGeometryRecord& a, const FeatureGeometryRecord& b) {
                  return a.stable_id < b.stable_id;
              });
}

[[nodiscard]] std::array<unsigned char, 8> encode_f64le(const double input) noexcept {
    const double value = canonical_zero(input);
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
    return canonical_zero(value);
}

[[nodiscard]] std::vector<unsigned char> encode_vertices(
    const std::vector<GeographicPointRecord>& vertices) {
    std::vector<unsigned char> bytes;
    bytes.reserve(vertices.size() * 16U);
    for (const GeographicPointRecord point : vertices) {
        const auto longitude = encode_f64le(point.longitude_rad);
        const auto latitude = encode_f64le(point.latitude_rad);
        bytes.insert(bytes.end(), longitude.begin(), longitude.end());
        bytes.insert(bytes.end(), latitude.begin(), latitude.end());
    }
    return bytes;
}

Status bind_blob(
    sqlite3* db,
    sqlite3_stmt* stmt,
    const int index,
    const void* data,
    const std::size_t bytes) {
    if (bytes > static_cast<std::size_t>(INT_MAX)) {
        return {StorageError::invalid_argument, "canonical geometry blob exceeds SQLite bind size"};
    }
    if (sqlite3_bind_blob(stmt, index, data, static_cast<int>(bytes), SQLITE_TRANSIENT) != SQLITE_OK) {
        return {StorageError::sqlite_failure, detail::sqlite_message(db, "sqlite bind BLOB failed")};
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
        return {StorageError::invalid_application_id, "geometry target is not an AERIS project"};
    }

    sqlite3_int64 generation = 0;
    if (!(status = detail::query_single_int(db, "PRAGMA user_version;", generation))) return status;
    if (generation != kProjectSchemaGeneration) {
        return {StorageError::unsupported_schema, "geometry target has unsupported draft schema generation"};
    }

    std::string uuid;
    if (!(status = detail::query_single_text(db, "SELECT project_uuid FROM aeris_meta WHERE id=1;", uuid))) return status;
    if (uuid != project.metadata().project_uuid) {
        return {StorageError::schema_invalid, "geometry target UUID differs from the validated project handle"};
    }
    return Status::success();
}

Status source_exists(sqlite3* db, const std::string& source_id, bool& exists) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(db, "SELECT 1 FROM aeris_source WHERE source_id=?;", stmt);
    if (!status) return status;
    if (!(status = detail::bind_text(db, stmt.get(), 1, source_id))) return status;
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) {
        exists = false;
        return Status::success();
    }
    if (rc != SQLITE_ROW || sqlite3_column_type(stmt.get(), 0) != SQLITE_INTEGER) {
        return {StorageError::schema_invalid, "source geometry parent lookup returned malformed data"};
    }
    exists = true;
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return {StorageError::schema_invalid, "source geometry parent lookup returned duplicate rows"};
    }
    return Status::success();
}

Status load_geometry_marker(
    sqlite3* db,
    const std::string& source_id,
    std::optional<std::size_t>& feature_count) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(
        db,
        "SELECT model_id,encoding_id,feature_count FROM aeris_source_geometry WHERE source_id=?;",
        stmt);
    if (!status) return status;
    if (!(status = detail::bind_text(db, stmt.get(), 1, source_id))) return status;

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) {
        feature_count.reset();
        return Status::success();
    }
    if (rc != SQLITE_ROW || sqlite3_column_type(stmt.get(), 0) != SQLITE_TEXT ||
        sqlite3_column_type(stmt.get(), 1) != SQLITE_TEXT ||
        sqlite3_column_type(stmt.get(), 2) != SQLITE_INTEGER) {
        return {StorageError::schema_invalid, "stored source geometry marker has invalid SQLite types"};
    }

    const std::string model = text_column(stmt.get(), 0);
    const std::string encoding = text_column(stmt.get(), 1);
    const sqlite3_int64 count = sqlite3_column_int64(stmt.get(), 2);
    if (std::string_view(model) != kCanonicalGeometryModelId ||
        std::string_view(encoding) != kCanonicalCoordinateEncodingId ||
        count < 0 || count > static_cast<sqlite3_int64>(kMaxFeaturesPerSource)) {
        return {StorageError::schema_invalid, "stored source geometry marker violates the canonical model/encoding contract"};
    }
    feature_count = static_cast<std::size_t>(count);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return {StorageError::schema_invalid, "source geometry marker is not unique"};
    }
    return Status::success();
}

Status read_geometry_index(
    sqlite3* db,
    const std::string& source_id,
    std::vector<FeatureGeometryIndexEntry>& features) {
    std::optional<std::size_t> expected_count;
    Status status = load_geometry_marker(db, source_id, expected_count);
    if (!status) return status;
    if (!expected_count.has_value()) {
        return {StorageError::record_not_found, "source has no canonical geometry set"};
    }

    detail::StmtPtr stmt;
    status = detail::prepare(
        db,
        "SELECT stable_id,source_feature_id,ring_count FROM aeris_feature WHERE source_id=? ORDER BY stable_id;",
        stmt);
    if (!status) return status;
    if (!(status = detail::bind_text(db, stmt.get(), 1, source_id))) return status;

    std::set<std::string> source_feature_ids;
    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW || sqlite3_column_type(stmt.get(), 0) != SQLITE_TEXT ||
            sqlite3_column_type(stmt.get(), 1) != SQLITE_TEXT ||
            sqlite3_column_type(stmt.get(), 2) != SQLITE_INTEGER) {
            return {StorageError::schema_invalid, "stored feature index contains invalid SQLite types"};
        }
        FeatureGeometryIndexEntry entry{};
        entry.stable_id = text_column(stmt.get(), 0);
        entry.source_feature_id = text_column(stmt.get(), 1);
        const sqlite3_int64 ring_count = sqlite3_column_int64(stmt.get(), 2);
        if (!bounded_identifier(entry.stable_id) || !bounded_identifier(entry.source_feature_id) ||
            ring_count < 1 || ring_count > static_cast<sqlite3_int64>(kMaxRingsPerFeature)) {
            return {StorageError::schema_invalid, "stored feature index violates canonical bounds"};
        }
        if (!source_feature_ids.insert(entry.source_feature_id).second) {
            return {StorageError::schema_invalid, "stored geometry contains duplicate source feature identities"};
        }
        entry.ring_count = static_cast<std::uint32_t>(ring_count);
        features.push_back(std::move(entry));
        if (features.size() > kMaxFeaturesPerSource) {
            return {StorageError::schema_invalid, "stored geometry exceeds the feature-count draft bound"};
        }
    }

    if (features.size() != *expected_count) {
        return {StorageError::schema_invalid, "source geometry marker feature_count disagrees with feature rows"};
    }
    return Status::success();
}

Status decode_ring_row(sqlite3_stmt* stmt, GeographicRingRecord& ring) {
    for (const int column : {0, 1, 2, 4}) {
        if (sqlite3_column_type(stmt, column) != SQLITE_INTEGER) {
            return {StorageError::schema_invalid, "stored feature ring contains invalid integer column type"};
        }
    }
    if (sqlite3_column_type(stmt, 3) != SQLITE_BLOB || sqlite3_column_type(stmt, 5) != SQLITE_BLOB) {
        return {StorageError::schema_invalid, "stored feature ring contains invalid coordinate BLOB type"};
    }

    const sqlite3_int64 role = sqlite3_column_int64(stmt, 0);
    const sqlite3_int64 side = sqlite3_column_int64(stmt, 1);
    const sqlite3_int64 winding = sqlite3_column_int64(stmt, 2);
    const sqlite3_int64 vertex_count = sqlite3_column_int64(stmt, 4);
    if (role < 0 || role > 1 || side < 0 || side > 2 ||
        winding < static_cast<sqlite3_int64>(std::numeric_limits<std::int32_t>::min()) ||
        winding > static_cast<sqlite3_int64>(std::numeric_limits<std::int32_t>::max()) ||
        vertex_count < 3 || vertex_count > static_cast<sqlite3_int64>(kMaxVerticesPerRing)) {
        return {StorageError::schema_invalid, "stored feature ring numeric field is outside canonical bounds"};
    }

    const int closing_bytes = sqlite3_column_bytes(stmt, 3);
    const auto* closing_blob = static_cast<const unsigned char*>(sqlite3_column_blob(stmt, 3));
    if (closing_bytes != 8 || closing_blob == nullptr) {
        return {StorageError::schema_invalid, "stored closing longitude BLOB is not one binary64 value"};
    }

    const std::size_t count = static_cast<std::size_t>(vertex_count);
    const std::size_t expected_bytes = count * 16U;
    const int vertex_bytes = sqlite3_column_bytes(stmt, 5);
    const auto* vertex_blob = static_cast<const unsigned char*>(sqlite3_column_blob(stmt, 5));
    if (expected_bytes > static_cast<std::size_t>(INT_MAX) ||
        vertex_bytes != static_cast<int>(expected_bytes) || vertex_blob == nullptr) {
        return {StorageError::schema_invalid, "stored feature vertex BLOB length disagrees with vertex_count"};
    }

    ring.role = static_cast<StoredRingRole>(static_cast<std::uint8_t>(role));
    ring.interior_side = static_cast<StoredInteriorSide>(static_cast<std::uint8_t>(side));
    ring.longitude_winding = static_cast<std::int32_t>(winding);
    ring.closing_longitude_rad = decode_f64le(closing_blob);
    ring.vertices.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        const unsigned char* pair = vertex_blob + index * 16U;
        ring.vertices.push_back({decode_f64le(pair), decode_f64le(pair + 8U)});
    }

    Status status = validate_ring(ring);
    if (!status) {
        return {StorageError::schema_invalid, "stored feature ring violates canonical WGS84 geometry: " + status.diagnostic};
    }
    return Status::success();
}

Status load_feature_from_db(
    sqlite3* db,
    const std::string& source_id,
    const std::string& stable_id,
    std::optional<FeatureGeometryRecord>& feature) {
    detail::StmtPtr feature_stmt;
    Status status = detail::prepare(
        db,
        "SELECT source_feature_id,ring_count FROM aeris_feature WHERE source_id=? AND stable_id=?;",
        feature_stmt);
    if (!status) return status;
    if (!(status = detail::bind_text(db, feature_stmt.get(), 1, source_id))) return status;
    if (!(status = detail::bind_text(db, feature_stmt.get(), 2, stable_id))) return status;

    const int rc = sqlite3_step(feature_stmt.get());
    if (rc == SQLITE_DONE) {
        feature.reset();
        return Status::success();
    }
    if (rc != SQLITE_ROW || sqlite3_column_type(feature_stmt.get(), 0) != SQLITE_TEXT ||
        sqlite3_column_type(feature_stmt.get(), 1) != SQLITE_INTEGER) {
        return {StorageError::schema_invalid, "stored feature row has invalid SQLite types"};
    }

    FeatureGeometryRecord loaded{};
    loaded.stable_id = stable_id;
    loaded.source_feature_id = text_column(feature_stmt.get(), 0);
    const sqlite3_int64 expected_rings = sqlite3_column_int64(feature_stmt.get(), 1);
    if (!bounded_identifier(loaded.stable_id) || !bounded_identifier(loaded.source_feature_id) ||
        expected_rings < 1 || expected_rings > static_cast<sqlite3_int64>(kMaxRingsPerFeature)) {
        return {StorageError::schema_invalid, "stored feature row violates canonical bounds"};
    }
    if (sqlite3_step(feature_stmt.get()) != SQLITE_DONE) {
        return {StorageError::schema_invalid, "feature stable ID resolves to more than one row"};
    }

    detail::StmtPtr ring_stmt;
    status = detail::prepare(
        db,
        "SELECT role,interior_side,longitude_winding,closing_longitude_f64le,vertex_count,vertices_f64le,ring_index "
        "FROM aeris_feature_ring WHERE source_id=? AND stable_id=? ORDER BY ring_index;",
        ring_stmt);
    if (!status) return status;
    if (!(status = detail::bind_text(db, ring_stmt.get(), 1, source_id))) return status;
    if (!(status = detail::bind_text(db, ring_stmt.get(), 2, stable_id))) return status;

    while (true) {
        const int ring_rc = sqlite3_step(ring_stmt.get());
        if (ring_rc == SQLITE_DONE) break;
        if (ring_rc != SQLITE_ROW) {
            return {StorageError::sqlite_failure, detail::sqlite_message(db, "could not read canonical feature ring")};
        }
        if (sqlite3_column_type(ring_stmt.get(), 6) != SQLITE_INTEGER) {
            return {StorageError::schema_invalid, "stored feature ring index has invalid SQLite type"};
        }
        const sqlite3_int64 ring_index = sqlite3_column_int64(ring_stmt.get(), 6);
        if (ring_index < 0 ||
            ring_index != static_cast<sqlite3_int64>(loaded.rings.size())) {
            return {StorageError::schema_invalid, "stored feature ring indices are not contiguous from zero"};
        }

        GeographicRingRecord ring{};
        status = decode_ring_row(ring_stmt.get(), ring);
        if (!status) return status;
        loaded.rings.push_back(std::move(ring));
        if (loaded.rings.size() > kMaxRingsPerFeature) {
            return {StorageError::schema_invalid, "stored feature exceeds the ring-count draft bound"};
        }
    }

    if (loaded.rings.size() != static_cast<std::size_t>(expected_rings)) {
        return {StorageError::schema_invalid, "stored feature ring_count disagrees with ring rows"};
    }
    status = validate_feature(loaded);
    if (!status) {
        return {StorageError::schema_invalid, "stored feature violates canonical geometry bounds: " + status.diagnostic};
    }
    feature = std::move(loaded);
    return Status::success();
}

[[nodiscard]] bool equal_point(
    const GeographicPointRecord a,
    const GeographicPointRecord b) noexcept {
    return canonical_zero(a.longitude_rad) == canonical_zero(b.longitude_rad) &&
           canonical_zero(a.latitude_rad) == canonical_zero(b.latitude_rad);
}

[[nodiscard]] bool equal_ring(
    const GeographicRingRecord& a,
    const GeographicRingRecord& b) noexcept {
    if (a.role != b.role || a.interior_side != b.interior_side ||
        a.longitude_winding != b.longitude_winding ||
        canonical_zero(a.closing_longitude_rad) != canonical_zero(b.closing_longitude_rad) ||
        a.vertices.size() != b.vertices.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < a.vertices.size(); ++index) {
        if (!equal_point(a.vertices[index], b.vertices[index])) return false;
    }
    return true;
}

[[nodiscard]] bool equal_feature(
    const FeatureGeometryRecord& a,
    const FeatureGeometryRecord& b) noexcept {
    if (a.stable_id != b.stable_id || a.source_feature_id != b.source_feature_id ||
        a.rings.size() != b.rings.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < a.rings.size(); ++index) {
        if (!equal_ring(a.rings[index], b.rings[index])) return false;
    }
    return true;
}

Status existing_geometry_equals(
    sqlite3* db,
    const SourceGeometryRecord& expected,
    bool& identical) {
    std::vector<FeatureGeometryIndexEntry> index;
    Status status = read_geometry_index(db, expected.source_id, index);
    if (!status) return status;
    if (index.size() != expected.features.size()) {
        identical = false;
        return Status::success();
    }

    for (std::size_t position = 0U; position < expected.features.size(); ++position) {
        const FeatureGeometryRecord& wanted = expected.features[position];
        const FeatureGeometryIndexEntry& indexed = index[position];
        if (indexed.stable_id != wanted.stable_id ||
            indexed.source_feature_id != wanted.source_feature_id ||
            static_cast<std::size_t>(indexed.ring_count) != wanted.rings.size()) {
            identical = false;
            return Status::success();
        }
        std::optional<FeatureGeometryRecord> loaded;
        status = load_feature_from_db(db, expected.source_id, wanted.stable_id, loaded);
        if (!status) return status;
        if (!loaded.has_value()) {
            return {StorageError::schema_invalid, "feature index references a missing canonical feature row"};
        }
        if (!equal_feature(*loaded, wanted)) {
            identical = false;
            return Status::success();
        }
    }
    identical = true;
    return Status::success();
}

Status insert_geometry(sqlite3* db, const SourceGeometryRecord& record) {
    detail::StmtPtr marker;
    Status status = detail::prepare(
        db,
        "INSERT INTO aeris_source_geometry(source_id,model_id,encoding_id,feature_count) VALUES(?,?,?,?);",
        marker);
    if (!status) return status;
    if (!(status = detail::bind_text(db, marker.get(), 1, record.source_id))) return status;
    if (!(status = detail::bind_text(db, marker.get(), 2, std::string(kCanonicalGeometryModelId)))) return status;
    if (!(status = detail::bind_text(db, marker.get(), 3, std::string(kCanonicalCoordinateEncodingId)))) return status;
    if (!(status = detail::bind_int64(db, marker.get(), 4, static_cast<sqlite3_int64>(record.features.size())))) return status;
    if (!(status = detail::step_done(db, marker.get()))) return status;

    for (const FeatureGeometryRecord& feature : record.features) {
        detail::StmtPtr feature_stmt;
        status = detail::prepare(
            db,
            "INSERT INTO aeris_feature(source_id,stable_id,source_feature_id,ring_count) VALUES(?,?,?,?);",
            feature_stmt);
        if (!status) return status;
        if (!(status = detail::bind_text(db, feature_stmt.get(), 1, record.source_id))) return status;
        if (!(status = detail::bind_text(db, feature_stmt.get(), 2, feature.stable_id))) return status;
        if (!(status = detail::bind_text(db, feature_stmt.get(), 3, feature.source_feature_id))) return status;
        if (!(status = detail::bind_int64(db, feature_stmt.get(), 4, static_cast<sqlite3_int64>(feature.rings.size())))) return status;
        if (!(status = detail::step_done(db, feature_stmt.get()))) return status;

        for (std::size_t ring_index = 0U; ring_index < feature.rings.size(); ++ring_index) {
            const GeographicRingRecord& ring = feature.rings[ring_index];
            const auto closing = encode_f64le(ring.closing_longitude_rad);
            const std::vector<unsigned char> vertices = encode_vertices(ring.vertices);

            detail::StmtPtr ring_stmt;
            status = detail::prepare(
                db,
                "INSERT INTO aeris_feature_ring(source_id,stable_id,ring_index,role,interior_side,longitude_winding,"
                "closing_longitude_f64le,vertex_count,vertices_f64le) VALUES(?,?,?,?,?,?,?,?,?);",
                ring_stmt);
            if (!status) return status;
            if (!(status = detail::bind_text(db, ring_stmt.get(), 1, record.source_id))) return status;
            if (!(status = detail::bind_text(db, ring_stmt.get(), 2, feature.stable_id))) return status;
            if (!(status = detail::bind_int64(db, ring_stmt.get(), 3, static_cast<sqlite3_int64>(ring_index)))) return status;
            if (!(status = detail::bind_int64(db, ring_stmt.get(), 4, static_cast<sqlite3_int64>(static_cast<std::uint8_t>(ring.role))))) return status;
            if (!(status = detail::bind_int64(db, ring_stmt.get(), 5, static_cast<sqlite3_int64>(static_cast<std::uint8_t>(ring.interior_side))))) return status;
            if (!(status = detail::bind_int64(db, ring_stmt.get(), 6, static_cast<sqlite3_int64>(ring.longitude_winding)))) return status;
            if (!(status = bind_blob(db, ring_stmt.get(), 7, closing.data(), closing.size()))) return status;
            if (!(status = detail::bind_int64(db, ring_stmt.get(), 8, static_cast<sqlite3_int64>(ring.vertices.size())))) return status;
            if (!(status = bind_blob(db, ring_stmt.get(), 9, vertices.data(), vertices.size()))) return status;
            if (!(status = detail::step_done(db, ring_stmt.get()))) return status;
        }
    }
    return Status::success();
}

Status advance_project_revision(
    sqlite3* db,
    const ProjectStore& project,
    const std::string_view modified_utc) {
    sqlite3_int64 current_revision = 0;
    Status status = detail::query_single_int(db, "SELECT revision FROM aeris_meta WHERE id=1;", current_revision);
    if (!status) return status;
    if (current_revision < 0 || current_revision == std::numeric_limits<sqlite3_int64>::max()) {
        return {StorageError::schema_invalid, "project revision cannot be incremented for geometry mutation"};
    }

    detail::StmtPtr meta_stmt;
    status = detail::prepare(
        db,
        "UPDATE aeris_meta SET revision=?,modified_utc=? WHERE id=1 AND project_uuid=?;",
        meta_stmt);
    if (status) status = detail::bind_int64(db, meta_stmt.get(), 1, current_revision + 1);
    if (status) status = detail::bind_text(db, meta_stmt.get(), 2, std::string(modified_utc));
    if (status) status = detail::bind_text(db, meta_stmt.get(), 3, project.metadata().project_uuid);
    if (status) status = detail::step_done(db, meta_stmt.get());
    if (status && sqlite3_changes(db) != 1) {
        return {StorageError::schema_invalid, "geometry mutation could not advance exactly one project metadata row"};
    }
    return status;
}

}  // namespace

SourceGeometryMutationResult store_source_geometry(
    ProjectStore& project,
    const SourceGeometryRecord& input,
    const std::string_view modified_utc) {
    if (!is_canonical_utc_timestamp(modified_utc)) {
        return {{StorageError::invalid_argument, "geometry mutation timestamp is not canonical Gregorian UTC"}, false, false};
    }

    SourceGeometryRecord record = input;
    canonicalize_source_geometry(record);
    Status status = validate_source_geometry(record);
    if (!status) return {std::move(status), false, false};

    detail::DbPtr db;
    status = detail::open_database(project.path(), SQLITE_OPEN_READWRITE, db);
    if (!status) return {std::move(status), false, false};
    if (!(status = validate_project_connection(db.get(), project))) return {std::move(status), false, false};
    if (!(status = detail::configure_durable(db.get()))) return {std::move(status), false, false};
    if (!(status = detail::begin_immediate(db.get()))) return {std::move(status), false, false};

    bool parent_exists = false;
    status = source_exists(db.get(), record.source_id, parent_exists);
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false};
    }
    if (!parent_exists) {
        detail::rollback(db.get());
        return {{StorageError::record_not_found, "canonical geometry requires an already-persisted source provenance row"}, false, false};
    }

    std::optional<std::size_t> existing_count;
    status = load_geometry_marker(db.get(), record.source_id, existing_count);
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false};
    }
    if (existing_count.has_value()) {
        bool identical = false;
        status = existing_geometry_equals(db.get(), record, identical);
        detail::rollback(db.get());
        if (!status) return {std::move(status), false, false};
        status = project.refresh_metadata();
        if (!status) return {std::move(status), false, false};
        if (identical) return {Status::success(), false, false};
        return {{StorageError::record_exists, "source already has a different immutable canonical geometry set"}, false, false};
    }

    status = insert_geometry(db.get(), record);
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false};
    }
    status = advance_project_revision(db.get(), project, modified_utc);
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

SourceGeometryIndexResult list_source_geometry_index(
    const ProjectStore& project,
    const std::string_view source_id) {
    if (source_id.empty() || source_id.size() > kMaxIdentifierBytes || source_id.find('\0') != std::string_view::npos) {
        return {{StorageError::invalid_argument, "geometry source ID violates canonical identifier bounds"}, {}};
    }

    detail::DbPtr db;
    Status status = detail::open_database(project.path(), SQLITE_OPEN_READONLY, db);
    if (!status) return {std::move(status), {}};
    if (!(status = validate_project_connection(db.get(), project))) return {std::move(status), {}};

    SourceGeometryIndexResult result{};
    status = read_geometry_index(db.get(), std::string(source_id), result.features);
    result.status = std::move(status);
    if (!result.status) result.features.clear();
    return result;
}

FeatureGeometryLoadResult load_feature_geometry(
    const ProjectStore& project,
    const std::string_view source_id,
    const std::string_view stable_id) {
    if (source_id.empty() || source_id.size() > kMaxIdentifierBytes || source_id.find('\0') != std::string_view::npos ||
        stable_id.empty() || stable_id.size() > kMaxIdentifierBytes || stable_id.find('\0') != std::string_view::npos) {
        return {{StorageError::invalid_argument, "geometry source/feature identifier violates canonical bounds"}, std::nullopt};
    }

    detail::DbPtr db;
    Status status = detail::open_database(project.path(), SQLITE_OPEN_READONLY, db);
    if (!status) return {std::move(status), std::nullopt};
    if (!(status = validate_project_connection(db.get(), project))) return {std::move(status), std::nullopt};

    std::optional<std::size_t> marker;
    status = load_geometry_marker(db.get(), std::string(source_id), marker);
    if (!status) return {std::move(status), std::nullopt};
    if (!marker.has_value()) {
        return {{StorageError::record_not_found, "source has no canonical geometry set"}, std::nullopt};
    }

    FeatureGeometryLoadResult result{};
    status = load_feature_from_db(db.get(), std::string(source_id), std::string(stable_id), result.feature);
    if (!status) return {std::move(status), std::nullopt};
    if (!result.feature.has_value()) {
        return {{StorageError::record_not_found, "canonical feature stable ID was not found"}, std::nullopt};
    }
    result.status = Status::success();
    return result;
}

}  // namespace aeris::storage
