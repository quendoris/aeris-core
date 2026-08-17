// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#include "aeris/storage/dataset.hpp"

#include "feature_property_detail.hpp"
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
constexpr std::size_t kMaxUriBytes = 4096U;
constexpr std::size_t kMaxResourcesPerSource = 4096U;
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

[[nodiscard]] bool bounded_text(
    const std::string& value,
    const std::size_t max_bytes,
    const bool allow_empty = false) noexcept {
    return (allow_empty || !value.empty()) && value.size() <= max_bytes &&
           value.find('\0') == std::string::npos;
}

[[nodiscard]] double canonical_zero(const double value) noexcept {
    return value == 0.0 ? 0.0 : value;
}

Status validate_resource(const SourceResourceRecord& resource) {
    if (!bounded_text(resource.logical_name, kMaxIdentifierBytes)) {
        return {StorageError::invalid_argument,
                "source resource logical name is empty, contains NUL, or exceeds 255 bytes"};
    }
    if (!is_canonical_sha256(resource.sha256)) {
        return {StorageError::invalid_argument,
                "source resource SHA-256 must be 64 lowercase hexadecimal characters"};
    }
    if (resource.size_bytes &&
        *resource.size_bytes > static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max())) {
        return {StorageError::invalid_argument,
                "source resource byte length exceeds SQLite signed integer range"};
    }
    return Status::success();
}

Status validate_source(const SourceSnapshotRecord& record) {
    if (!bounded_text(record.source_id, kMaxIdentifierBytes) ||
        !bounded_text(record.adapter_id, kMaxIdentifierBytes) ||
        !bounded_text(record.provider, kMaxIdentifierBytes) ||
        !bounded_text(record.dataset, kMaxIdentifierBytes) ||
        !bounded_text(record.snapshot, kMaxIdentifierBytes) ||
        !bounded_text(record.dataset_version, kMaxIdentifierBytes) ||
        !bounded_text(record.license_id, kMaxIdentifierBytes) ||
        !bounded_text(record.source_uri, kMaxUriBytes) ||
        !bounded_text(record.worldview, kMaxIdentifierBytes, true)) {
        return {StorageError::invalid_argument,
                "source snapshot text field violates canonical storage bounds"};
    }
    if (record.capability_bits == 0U) {
        return {StorageError::invalid_argument,
                "source snapshot must declare at least one capability bit"};
    }
    if (!is_canonical_sha256(record.content_sha256)) {
        return {StorageError::invalid_argument,
                "source snapshot content SHA-256 must be 64 lowercase hexadecimal characters"};
    }
    if (!is_canonical_utc_timestamp(record.retrieved_at_utc)) {
        return {StorageError::invalid_argument,
                "source snapshot retrieval timestamp is not canonical Gregorian UTC"};
    }
    if (record.resources.size() > kMaxResourcesPerSource) {
        return {StorageError::invalid_argument,
                "source snapshot exceeds the 4096-resource draft bound"};
    }

    std::set<std::string> logical_names;
    for (const SourceResourceRecord& resource : record.resources) {
        Status status = validate_resource(resource);
        if (!status) return status;
        if (!logical_names.insert(resource.logical_name).second) {
            return {StorageError::invalid_argument,
                    "source snapshot contains a duplicate logical resource name"};
        }
    }
    return Status::success();
}

void canonicalize_source(SourceSnapshotRecord& record) {
    std::sort(record.resources.begin(), record.resources.end(),
              [](const SourceResourceRecord& a, const SourceResourceRecord& b) {
                  return a.logical_name < b.logical_name;
              });
}

[[nodiscard]] bool valid_role(const StoredRingRole role) noexcept {
    return static_cast<std::uint8_t>(role) <=
           static_cast<std::uint8_t>(StoredRingRole::interior);
}

[[nodiscard]] bool valid_interior_side(const StoredInteriorSide side) noexcept {
    return static_cast<std::uint8_t>(side) <=
           static_cast<std::uint8_t>(StoredInteriorSide::right);
}

[[nodiscard]] bool ambiguous_half_turn(const double delta) noexcept {
    return std::abs(std::abs(delta) - kPi) <= kHalfTurnTolerance;
}

Status validate_ring(const GeographicRingRecord& ring) {
    if (!valid_role(ring.role) || !valid_interior_side(ring.interior_side)) {
        return {StorageError::invalid_argument,
                "feature ring role/interior-side value is outside the canonical enum domain"};
    }
    if (ring.vertices.size() < 3U || ring.vertices.size() > kMaxVerticesPerRing) {
        return {StorageError::invalid_argument,
                "feature ring vertex count is outside the draft 3..4194304 bound"};
    }
    if (!std::isfinite(ring.closing_longitude_rad)) {
        return {StorageError::invalid_argument,
                "feature ring closing longitude is non-finite"};
    }
    if (ring.longitude_winding != 0 &&
        ring.interior_side == StoredInteriorSide::unspecified) {
        return {StorageError::invalid_argument,
                "winding feature ring requires an explicit interior side"};
    }

    for (const GeographicPointRecord point : ring.vertices) {
        if (!std::isfinite(point.longitude_rad) || !std::isfinite(point.latitude_rad)) {
            return {StorageError::invalid_argument,
                    "feature ring contains a non-finite coordinate"};
        }
        if (point.latitude_rad < -kHalfPi || point.latitude_rad > kHalfPi) {
            return {StorageError::invalid_argument,
                    "feature ring latitude is outside WGS84 geographic range"};
        }
    }

    const double first_longitude = ring.vertices.front().longitude_rad;
    if (!(first_longitude > -kPi && first_longitude <= kPi)) {
        return {StorageError::invalid_argument,
                "canonical feature ring first longitude is outside (-pi,pi]"};
    }

    for (std::size_t index = 1U; index < ring.vertices.size(); ++index) {
        const double delta = ring.vertices[index].longitude_rad -
                             ring.vertices[index - 1U].longitude_rad;
        if (!std::isfinite(delta) || std::abs(delta) > kPi || ambiguous_half_turn(delta)) {
            return {StorageError::invalid_argument,
                    "feature ring contains a noncanonical longitude edge"};
        }
    }

    const double closing_delta = ring.closing_longitude_rad -
                                 ring.vertices.back().longitude_rad;
    if (!std::isfinite(closing_delta) || std::abs(closing_delta) > kPi ||
        ambiguous_half_turn(closing_delta)) {
        return {StorageError::invalid_argument,
                "feature ring contains a noncanonical closing longitude edge"};
    }

    const double turns = (ring.closing_longitude_rad - first_longitude) / kTwoPi;
    if (!std::isfinite(turns) ||
        turns < static_cast<double>(std::numeric_limits<std::int32_t>::min()) - 0.5 ||
        turns > static_cast<double>(std::numeric_limits<std::int32_t>::max()) + 0.5) {
        return {StorageError::invalid_argument,
                "feature ring longitude winding is outside canonical numeric range"};
    }
    const long long rounded = std::llround(turns);
    if (std::abs(turns - static_cast<double>(rounded)) > kWindingTolerance ||
        rounded != static_cast<long long>(ring.longitude_winding)) {
        return {StorageError::invalid_argument,
                "feature ring closing longitude disagrees with longitude winding"};
    }
    return Status::success();
}

Status validate_feature(const FeatureGeometryRecord& feature) {
    if (!bounded_text(feature.stable_id, kMaxIdentifierBytes) ||
        !bounded_text(feature.source_feature_id, kMaxIdentifierBytes)) {
        return {StorageError::invalid_argument,
                "feature stable/source identifier is empty, contains NUL, or exceeds 255 bytes"};
    }
    if (feature.rings.empty() || feature.rings.size() > kMaxRingsPerFeature) {
        return {StorageError::invalid_argument,
                "feature ring count is outside the draft 1..65535 bound"};
    }
    for (const GeographicRingRecord& ring : feature.rings) {
        Status status = validate_ring(ring);
        if (!status) return status;
    }
    return Status::success();
}

Status validate_geometry(const SourceGeometryRecord& record) {
    if (!bounded_text(record.source_id, kMaxIdentifierBytes)) {
        return {StorageError::invalid_argument,
                "geometry source ID is empty, contains NUL, or exceeds 255 bytes"};
    }
    if (record.features.size() > kMaxFeaturesPerSource) {
        return {StorageError::invalid_argument,
                "source geometry exceeds the 1000000-feature draft bound"};
    }

    std::set<std::string> stable_ids;
    std::set<std::string> source_feature_ids;
    for (const FeatureGeometryRecord& feature : record.features) {
        Status status = validate_feature(feature);
        if (!status) return status;
        if (!stable_ids.insert(feature.stable_id).second) {
            return {StorageError::invalid_argument,
                    "source geometry contains a duplicate stable feature ID"};
        }
        if (!source_feature_ids.insert(feature.source_feature_id).second) {
            return {StorageError::invalid_argument,
                    "source geometry contains a duplicate source feature ID"};
        }
    }
    return Status::success();
}

void canonicalize_geometry(SourceGeometryRecord& record) {
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

[[nodiscard]] bool equal_resource(
    const SourceResourceRecord& a,
    const SourceResourceRecord& b) noexcept {
    return a.logical_name == b.logical_name && a.sha256 == b.sha256 &&
           a.size_bytes == b.size_bytes;
}

[[nodiscard]] bool equal_source(
    const SourceSnapshotRecord& a,
    const SourceSnapshotRecord& b) noexcept {
    if (a.source_id != b.source_id || a.adapter_id != b.adapter_id ||
        a.capability_bits != b.capability_bits || a.temporal_class != b.temporal_class ||
        a.provider != b.provider || a.dataset != b.dataset || a.snapshot != b.snapshot ||
        a.dataset_version != b.dataset_version || a.source_uri != b.source_uri ||
        a.license_id != b.license_id || a.content_sha256 != b.content_sha256 ||
        a.retrieved_at_utc != b.retrieved_at_utc || a.worldview != b.worldview ||
        a.resources.size() != b.resources.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < a.resources.size(); ++index) {
        if (!equal_resource(a.resources[index], b.resources[index])) return false;
    }
    return true;
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

[[nodiscard]] bool equal_geometry(
    const SourceGeometryRecord& a,
    const SourceGeometryRecord& b) noexcept {
    if (a.source_id != b.source_id || a.features.size() != b.features.size()) return false;
    for (std::size_t index = 0U; index < a.features.size(); ++index) {
        if (!equal_feature(a.features[index], b.features[index])) return false;
    }
    return true;
}

Status read_existing_source(
    const ProjectStore& project,
    const std::string& source_id,
    std::optional<SourceSnapshotRecord>& record) {
    const SourceSnapshotListResult listed = list_source_snapshots(project);
    if (!listed.ok()) return listed.status;
    for (SourceSnapshotRecord item : listed.records) {
        if (item.source_id == source_id) {
            canonicalize_source(item);
            record = std::move(item);
            return Status::success();
        }
    }
    record.reset();
    return Status::success();
}

Status read_existing_geometry(
    const ProjectStore& project,
    const std::string& source_id,
    std::optional<SourceGeometryRecord>& record) {
    const SourceGeometryIndexResult index = list_source_geometry_index(project, source_id);
    if (!index.ok()) {
        if (index.status.error == StorageError::record_not_found) {
            record.reset();
            return Status::success();
        }
        return index.status;
    }

    SourceGeometryRecord loaded{};
    loaded.source_id = source_id;
    loaded.features.reserve(index.features.size());
    for (const FeatureGeometryIndexEntry& entry : index.features) {
        const FeatureGeometryLoadResult feature =
            load_feature_geometry(project, source_id, entry.stable_id);
        if (!feature.ok()) return feature.status;
        if (feature.feature->source_feature_id != entry.source_feature_id ||
            feature.feature->rings.size() != static_cast<std::size_t>(entry.ring_count)) {
            return {StorageError::schema_invalid,
                    "canonical geometry index disagrees with loaded feature"};
        }
        loaded.features.push_back(*feature.feature);
    }
    canonicalize_geometry(loaded);
    record = std::move(loaded);
    return Status::success();
}

Status validate_project_connection(sqlite3* db, const ProjectStore& project) {
    Status status = detail::verify_quick_check(db);
    if (!status) return status;

    sqlite3_int64 application_id = 0;
    if (!(status = detail::query_single_int(db, "PRAGMA application_id;", application_id))) return status;
    if (application_id != static_cast<sqlite3_int64>(kProjectApplicationId)) {
        return {StorageError::invalid_application_id,
                "source dataset target is not an AERIS project"};
    }

    sqlite3_int64 generation = 0;
    if (!(status = detail::query_single_int(db, "PRAGMA user_version;", generation))) return status;
    if (generation != kProjectSchemaGeneration) {
        return {StorageError::unsupported_schema,
                "source dataset target has unsupported draft schema generation"};
    }

    std::string uuid;
    if (!(status = detail::query_single_text(
              db, "SELECT project_uuid FROM aeris_meta WHERE id=1;", uuid))) {
        return status;
    }
    if (uuid != project.metadata().project_uuid) {
        return {StorageError::schema_invalid,
                "source dataset project UUID differs from the validated project handle"};
    }
    return Status::success();
}

Status row_exists(
    sqlite3* db,
    const char* sql,
    const std::string& source_id,
    bool& exists) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(db, sql, stmt);
    if (!status) return status;
    if (!(status = detail::bind_text(db, stmt.get(), 1, source_id))) return status;
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) {
        exists = false;
        return Status::success();
    }
    if (rc != SQLITE_ROW || sqlite3_column_type(stmt.get(), 0) != SQLITE_INTEGER) {
        return {StorageError::schema_invalid,
                "source dataset existence probe returned malformed data"};
    }
    exists = true;
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return {StorageError::schema_invalid,
                "source dataset existence probe returned duplicate rows"};
    }
    return Status::success();
}

Status insert_source(sqlite3* db, const SourceSnapshotRecord& record) {
    detail::StmtPtr source_stmt;
    Status status = detail::prepare(
        db,
        "INSERT INTO aeris_source(source_id,adapter_id,capability_bits,temporal_class,provider,dataset,snapshot,dataset_version,source_uri,license_id,content_sha256,retrieved_at_utc,worldview) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?);",
        source_stmt);
    if (!status) return status;

    int index = 1;
    if (!(status = detail::bind_text(db, source_stmt.get(), index++, record.source_id))) return status;
    if (!(status = detail::bind_text(db, source_stmt.get(), index++, record.adapter_id))) return status;
    if (!(status = detail::bind_int64(db, source_stmt.get(), index++, static_cast<sqlite3_int64>(record.capability_bits)))) return status;
    if (!(status = detail::bind_int64(db, source_stmt.get(), index++, static_cast<sqlite3_int64>(record.temporal_class)))) return status;
    if (!(status = detail::bind_text(db, source_stmt.get(), index++, record.provider))) return status;
    if (!(status = detail::bind_text(db, source_stmt.get(), index++, record.dataset))) return status;
    if (!(status = detail::bind_text(db, source_stmt.get(), index++, record.snapshot))) return status;
    if (!(status = detail::bind_text(db, source_stmt.get(), index++, record.dataset_version))) return status;
    if (!(status = detail::bind_text(db, source_stmt.get(), index++, record.source_uri))) return status;
    if (!(status = detail::bind_text(db, source_stmt.get(), index++, record.license_id))) return status;
    if (!(status = detail::bind_text(db, source_stmt.get(), index++, record.content_sha256))) return status;
    if (!(status = detail::bind_text(db, source_stmt.get(), index++, record.retrieved_at_utc))) return status;
    if (!(status = detail::bind_text(db, source_stmt.get(), index, record.worldview))) return status;
    if (!(status = detail::step_done(db, source_stmt.get()))) return status;

    for (const SourceResourceRecord& resource : record.resources) {
        detail::StmtPtr resource_stmt;
        status = detail::prepare(
            db,
            "INSERT INTO aeris_source_resource(source_id,logical_name,sha256,size_bytes) VALUES(?,?,?,?);",
            resource_stmt);
        if (!status) return status;
        if (!(status = detail::bind_text(db, resource_stmt.get(), 1, record.source_id))) return status;
        if (!(status = detail::bind_text(db, resource_stmt.get(), 2, resource.logical_name))) return status;
        if (!(status = detail::bind_text(db, resource_stmt.get(), 3, resource.sha256))) return status;
        if (resource.size_bytes) {
            if (!(status = detail::bind_int64(
                      db, resource_stmt.get(), 4,
                      static_cast<sqlite3_int64>(*resource.size_bytes)))) {
                return status;
            }
        } else if (sqlite3_bind_null(resource_stmt.get(), 4) != SQLITE_OK) {
            return {StorageError::sqlite_failure,
                    detail::sqlite_message(db, "sqlite bind NULL failed")};
        }
        if (!(status = detail::step_done(db, resource_stmt.get()))) return status;
    }
    return Status::success();
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
        return {StorageError::invalid_argument,
                "canonical geometry blob exceeds SQLite bind size"};
    }
    if (sqlite3_bind_blob(stmt, index, data, static_cast<int>(bytes), SQLITE_TRANSIENT) != SQLITE_OK) {
        return {StorageError::sqlite_failure,
                detail::sqlite_message(db, "sqlite bind BLOB failed")};
    }
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
    if (!(status = detail::bind_text(
              db, marker.get(), 2, std::string(kCanonicalGeometryModelId)))) return status;
    if (!(status = detail::bind_text(
              db, marker.get(), 3, std::string(kCanonicalCoordinateEncodingId)))) return status;
    if (!(status = detail::bind_int64(
              db, marker.get(), 4,
              static_cast<sqlite3_int64>(record.features.size())))) return status;
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
        if (!(status = detail::bind_int64(
                  db, feature_stmt.get(), 4,
                  static_cast<sqlite3_int64>(feature.rings.size())))) return status;
        if (!(status = detail::step_done(db, feature_stmt.get()))) return status;

        for (std::size_t ring_index = 0U; ring_index < feature.rings.size(); ++ring_index) {
            const GeographicRingRecord& ring = feature.rings[ring_index];
            const auto closing = encode_f64le(ring.closing_longitude_rad);
            const std::vector<unsigned char> vertices = encode_vertices(ring.vertices);

            detail::StmtPtr ring_stmt;
            status = detail::prepare(
                db,
                "INSERT INTO aeris_feature_ring(source_id,stable_id,ring_index,role,interior_side,longitude_winding,closing_longitude_f64le,vertex_count,vertices_f64le) VALUES(?,?,?,?,?,?,?,?,?);",
                ring_stmt);
            if (!status) return status;
            if (!(status = detail::bind_text(db, ring_stmt.get(), 1, record.source_id))) return status;
            if (!(status = detail::bind_text(db, ring_stmt.get(), 2, feature.stable_id))) return status;
            if (!(status = detail::bind_int64(
                      db, ring_stmt.get(), 3,
                      static_cast<sqlite3_int64>(ring_index)))) return status;
            if (!(status = detail::bind_int64(
                      db, ring_stmt.get(), 4,
                      static_cast<sqlite3_int64>(static_cast<std::uint8_t>(ring.role))))) return status;
            if (!(status = detail::bind_int64(
                      db, ring_stmt.get(), 5,
                      static_cast<sqlite3_int64>(static_cast<std::uint8_t>(ring.interior_side))))) return status;
            if (!(status = detail::bind_int64(
                      db, ring_stmt.get(), 6,
                      static_cast<sqlite3_int64>(ring.longitude_winding)))) return status;
            if (!(status = bind_blob(db, ring_stmt.get(), 7, closing.data(), closing.size()))) return status;
            if (!(status = detail::bind_int64(
                      db, ring_stmt.get(), 8,
                      static_cast<sqlite3_int64>(ring.vertices.size())))) return status;
            if (!(status = bind_blob(
                      db, ring_stmt.get(), 9, vertices.data(), vertices.size()))) return status;
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
    Status status = detail::query_single_int(
        db, "SELECT revision FROM aeris_meta WHERE id=1;", current_revision);
    if (!status) return status;
    if (current_revision < 0 ||
        current_revision == std::numeric_limits<sqlite3_int64>::max()) {
        return {StorageError::schema_invalid,
                "project revision cannot be incremented for source dataset mutation"};
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
        return {StorageError::schema_invalid,
                "source dataset mutation could not advance exactly one project metadata row"};
    }
    return status;
}

Status validate_property_geometry_match(const SourceDatasetRecord& record) {
    if (!record.feature_properties.has_value()) return Status::success();
    const SourceFeaturePropertiesRecord& properties = *record.feature_properties;
    if (properties.source_id != record.source.source_id ||
        properties.source_id != record.geometry.source_id) {
        return {StorageError::invalid_argument,
                "source dataset feature-property source ID must match provenance and geometry"};
    }
    if (properties.features.size() != record.geometry.features.size()) {
        return {StorageError::invalid_argument,
                "complete feature properties must contain exactly every dataset geometry feature"};
    }
    for (std::size_t index = 0U; index < properties.features.size(); ++index) {
        if (properties.features[index].stable_id != record.geometry.features[index].stable_id) {
            return {StorageError::invalid_argument,
                    "feature property stable-ID set must exactly match dataset canonical geometry"};
        }
    }
    return Status::success();
}

}  // namespace

SourceDatasetMutationResult store_source_dataset(
    ProjectStore& project,
    const SourceDatasetRecord& input,
    const std::string_view modified_utc) {
    if (!is_canonical_utc_timestamp(modified_utc)) {
        return {{StorageError::invalid_argument,
                 "source dataset mutation timestamp is not canonical Gregorian UTC"},
                false, false};
    }

    SourceDatasetRecord record = input;
    canonicalize_source(record.source);
    canonicalize_geometry(record.geometry);
    if (record.feature_properties.has_value()) {
        Status property_status =
            detail::canonicalize_and_validate_feature_properties(*record.feature_properties);
        if (!property_status) return {std::move(property_status), false, false};
    }

    if (record.source.source_id != record.geometry.source_id) {
        return {{StorageError::invalid_argument,
                 "source dataset provenance and geometry source IDs must match"},
                false, false};
    }

    Status status = validate_source(record.source);
    if (!status) return {std::move(status), false, false};
    status = validate_geometry(record.geometry);
    if (!status) return {std::move(status), false, false};
    status = validate_property_geometry_match(record);
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

    bool source_present = false;
    bool geometry_present = false;
    bool properties_present = false;
    status = row_exists(
        db.get(), "SELECT 1 FROM aeris_source WHERE source_id=?;",
        record.source.source_id, source_present);
    if (status) {
        status = row_exists(
            db.get(), "SELECT 1 FROM aeris_source_geometry WHERE source_id=?;",
            record.source.source_id, geometry_present);
    }
    if (status) {
        status = row_exists(
            db.get(), "SELECT 1 FROM aeris_source_feature_properties WHERE source_id=?;",
            record.source.source_id, properties_present);
    }
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false};
    }
    if (!source_present && geometry_present) {
        detail::rollback(db.get());
        return {{StorageError::schema_invalid,
                 "source geometry marker exists without its provenance parent"},
                false, false};
    }
    if (!geometry_present && properties_present) {
        detail::rollback(db.get());
        return {{StorageError::schema_invalid,
                 "feature property marker exists without canonical source geometry"},
                false, false};
    }

    if (source_present) {
        std::optional<SourceSnapshotRecord> existing_source;
        status = read_existing_source(project, record.source.source_id, existing_source);
        if (!status || !existing_source.has_value()) {
            detail::rollback(db.get());
            if (!status) return {std::move(status), false, false};
            return {{StorageError::schema_invalid,
                     "source existence probe disagrees with canonical provenance reader"},
                    false, false};
        }
        if (!equal_source(*existing_source, record.source)) {
            detail::rollback(db.get());
            return {{StorageError::record_exists,
                     "source ID already exists with different immutable provenance"},
                    false, false};
        }
    }

    if (geometry_present) {
        std::optional<SourceGeometryRecord> existing_geometry;
        status = read_existing_geometry(project, record.geometry.source_id, existing_geometry);
        if (!status || !existing_geometry.has_value()) {
            detail::rollback(db.get());
            if (!status) return {std::move(status), false, false};
            return {{StorageError::schema_invalid,
                     "geometry existence probe disagrees with canonical geometry reader"},
                    false, false};
        }
        if (!equal_geometry(*existing_geometry, record.geometry)) {
            detail::rollback(db.get());
            return {{StorageError::record_exists,
                     "source ID already exists with different immutable canonical geometry"},
                    false, false};
        }
    }

    if (record.feature_properties.has_value() && properties_present) {
        std::optional<SourceFeaturePropertiesRecord> existing_properties;
        status = detail::read_existing_feature_properties(
            project, record.source.source_id, existing_properties);
        if (!status || !existing_properties.has_value()) {
            detail::rollback(db.get());
            if (!status) return {std::move(status), false, false};
            return {{StorageError::schema_invalid,
                     "feature property existence probe disagrees with canonical reader"},
                    false, false};
        }
        if (!detail::equal_feature_properties(
                *existing_properties, *record.feature_properties)) {
            detail::rollback(db.get());
            return {{StorageError::record_exists,
                     "source already has different immutable complete feature properties"},
                    false, false};
        }
    }

    const bool insert_source_needed = !source_present;
    const bool insert_geometry_needed = !geometry_present;
    const bool insert_properties_needed =
        record.feature_properties.has_value() && !properties_present;

    if (!insert_source_needed && !insert_geometry_needed && !insert_properties_needed) {
        detail::rollback(db.get());
        status = project.refresh_metadata();
        if (!status) return {std::move(status), false, false};
        return {Status::success(), false, false};
    }

    if (insert_source_needed) status = insert_source(db.get(), record.source);
    if (status && insert_geometry_needed) status = insert_geometry(db.get(), record.geometry);
    if (status && insert_properties_needed) {
        status = detail::insert_feature_properties(db.get(), *record.feature_properties);
    }
    if (status) status = advance_project_revision(db.get(), project, modified_utc);
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
