// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/source/acquisition.hpp"
#include "aeris/source/dbf.hpp"
#include "aeris/source/natural_earth.hpp"
#include "aeris/util/sha256.hpp"
#include "aeris/util/text.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

constexpr std::string_view kProvider = "Natural Earth";
constexpr std::string_view kDataset = "ne_110m_admin_0_countries";
constexpr std::string_view kSnapshot = "v5.1.2";
constexpr std::string_view kSourceUri =
    "https://github.com/nvkelso/natural-earth-vector/tree/f1890d9f152c896d250a77557a5751a93d494776/110m_cultural";

int fail(const int code, const std::string& diagnostic) {
    std::cerr << "Natural Earth admin0 proof failed: " << diagnostic << '\n';
    return code;
}

bool add_resource(
    aeris::source::SnapshotManifest& manifest,
    const std::filesystem::path& root,
    std::string logical_name,
    std::filesystem::path relative_path) {
    const std::filesystem::path full = root / relative_path;
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(full, error);
    if (error) return false;
    const aeris::util::Sha256FileResult hash = aeris::util::sha256_file(full);
    if (!hash.ok()) return false;

    aeris::source::ResourceSpec resource{};
    resource.logical_name = std::move(logical_name);
    resource.relative_path = std::move(relative_path);
    resource.sha256 = hash.digest.hex();
    resource.size_bytes = size;
    manifest.resources.push_back(std::move(resource));
    return true;
}

std::string trim_spaces(const std::string& value) {
    const std::size_t first = value.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    const std::size_t last = value.find_last_not_of(' ');
    return value.substr(first, last - first + 1U);
}

std::optional<std::size_t> field_index(
    const aeris::source::DbfTableResult& table,
    const std::string_view name) {
    for (std::size_t index = 0U; index < table.fields.size(); ++index) {
        if (table.fields[index].name.size() == name.size() &&
            std::equal(table.fields[index].name.begin(), table.fields[index].name.end(), name.begin())) {
            return index;
        }
    }
    return std::nullopt;
}

bool diagnose_dbf_semantics(const std::filesystem::path& dbf_path, std::string& diagnostic) {
    const aeris::source::DbfTableResult table = aeris::source::read_dbf_table(dbf_path);
    if (!table.ok()) {
        diagnostic = "strict DBF reader failed before semantic diagnosis: " + table.diagnostic;
        return false;
    }

    constexpr std::array<std::string_view, 13> text_fields{{
        "NAME", "NAME_LONG", "ADMIN", "SOVEREIGNT", "TYPE", "ADM0_A3", "SOV_A3",
        "ISO_A2", "ISO_A3", "UN_A3", "CONTINENT", "REGION_UN", "SUBREGION"
    }};
    std::array<std::size_t, text_fields.size()> text_indices{};
    for (std::size_t index = 0U; index < text_fields.size(); ++index) {
        const auto found = field_index(table, text_fields[index]);
        if (!found.has_value()) {
            diagnostic = "missing DBF field " + std::string(text_fields[index]);
            return false;
        }
        text_indices[index] = *found;
    }
    const auto ne_id_index = field_index(table, "NE_ID");
    if (!ne_id_index.has_value()) {
        diagnostic = "missing DBF field NE_ID";
        return false;
    }

    for (const aeris::source::DbfRecord& row : table.records) {
        for (std::size_t field = 0U; field < text_fields.size(); ++field) {
            const std::string value = trim_spaces(row.values[text_indices[field]]);
            if (!aeris::util::is_valid_utf8_nul_free(value)) {
                diagnostic = "invalid UTF-8/NUL at physical row " +
                    std::to_string(row.record_number) + " field " + std::string(text_fields[field]);
                return false;
            }
        }
        const std::string ne_id_text = trim_spaces(row.values[*ne_id_index]);
        std::int64_t ne_id = 0;
        const auto parsed = std::from_chars(
            ne_id_text.data(), ne_id_text.data() + ne_id_text.size(), ne_id, 10);
        if (ne_id_text.empty() || parsed.ec != std::errc{} ||
            parsed.ptr != ne_id_text.data() + ne_id_text.size() || ne_id <= 0) {
            diagnostic = "invalid positive NE_ID at physical row " +
                std::to_string(row.record_number) + " raw='" + ne_id_text + "'";
            return false;
        }
    }
    return true;
}

const aeris::source::FeatureProperty* property(
    const aeris::source::Feature& feature,
    const std::string_view key) {
    for (const aeris::source::FeatureProperty& candidate : feature.properties) {
        if (candidate.key == key) return &candidate;
    }
    return nullptr;
}

}  // namespace

int main(const int argc, char** argv) {
    if (argc != 3) {
        return fail(2, "usage: <snapshot-root> <retrieved-at-utc>");
    }

    const std::filesystem::path root = argv[1];
    const std::string retrieved_at_utc = argv[2];

    aeris::source::SnapshotManifest manifest{};
    manifest.provider = std::string(kProvider);
    manifest.dataset = std::string(kDataset);
    manifest.snapshot = std::string(kSnapshot);
    manifest.source_uri = std::string(kSourceUri);
    manifest.retrieved_at_utc = retrieved_at_utc;

    if (!add_resource(manifest, root, "geometry.shp", "ne_110m_admin_0_countries.shp") ||
        !add_resource(manifest, root, "attributes.dbf", "ne_110m_admin_0_countries.dbf") ||
        !add_resource(manifest, root, "attributes.cpg", "ne_110m_admin_0_countries.cpg") ||
        !add_resource(manifest, root, "crs.prj", "ne_110m_admin_0_countries.prj") ||
        !add_resource(manifest, root, "dataset.version", "ne_110m_admin_0_countries.VERSION.txt")) {
        return fail(3, "could not hash all pinned admin0 resources");
    }

    aeris::source::SnapshotVerificationResult verified =
        aeris::source::verify_local_snapshot(root, manifest);
    if (!verified.ok() || !verified.snapshot.has_value()) {
        return fail(4, "verified snapshot construction failed: " + verified.diagnostic);
    }

    std::string semantic_diagnostic;
    if (!diagnose_dbf_semantics(root / "ne_110m_admin_0_countries.dbf", semantic_diagnostic)) {
        return fail(5, semantic_diagnostic);
    }

    const aeris::source::NaturalEarthAdmin0Countries110mAdapter adapter{};
    aeris::source::Request request{};
    request.capability = aeris::source::Capability::admin0;
    request.snapshot = std::string(kSnapshot);
    request.worldview = "natural-earth.de-facto";
    const aeris::source::Result result = adapter.load(*verified.snapshot, request);
    if (!result.ok()) {
        return fail(6, "adapter load failed: " + result.diagnostic);
    }
    if (!result.feature_properties_complete || result.features.empty()) {
        return fail(7, "adapter did not produce a nonempty complete feature-property channel");
    }
    if (result.provenance.worldview != request.worldview) {
        return fail(8, "adapter provenance worldview differs from requested pinned worldview");
    }

    std::set<std::string> stable_ids;
    std::set<std::int64_t> ne_ids;
    std::size_t rings = 0U;
    std::size_t vertices = 0U;
    for (const aeris::source::Feature& feature : result.features) {
        if (!stable_ids.insert(feature.stable_id).second || feature.properties.size() != 14U) {
            return fail(9, "feature identity or complete property cardinality is malformed");
        }

        const auto* name = property(feature, "name");
        const auto* iso_a2 = property(feature, "iso_a2");
        const auto* adm0_a3 = property(feature, "adm0_a3");
        const auto* ne_id = property(feature, "ne_id");
        if (name == nullptr || iso_a2 == nullptr || adm0_a3 == nullptr || ne_id == nullptr ||
            !std::holds_alternative<std::string>(name->value) ||
            !std::holds_alternative<std::string>(iso_a2->value) ||
            !std::holds_alternative<std::string>(adm0_a3->value) ||
            !std::holds_alternative<std::int64_t>(ne_id->value)) {
            return fail(10, "required typed political properties are absent or have wrong types");
        }
        if (std::get<std::string>(name->value).empty() ||
            std::get<std::string>(adm0_a3->value).empty()) {
            return fail(11, "required country name/admin code is empty");
        }
        if (!ne_ids.insert(std::get<std::int64_t>(ne_id->value)).second) {
            return fail(12, "NE_ID is not unique across the adapter result");
        }

        rings += feature.rings.size();
        for (const aeris::source::FeatureRing& ring : feature.rings) {
            vertices += ring.geometry.vertices.size();
        }
    }

    std::cout
        << "natural_earth_admin0: PASS"
        << " features=" << result.features.size()
        << " rings=" << rings
        << " vertices=" << vertices
        << " content_sha256=" << verified.snapshot->content_sha256()
        << '\n';
    return EXIT_SUCCESS;
}
