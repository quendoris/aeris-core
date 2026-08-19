// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/project/source_reader.hpp"

#include "aeris/source/adapter.hpp"
#include "aeris/storage/feature_property.hpp"
#include "aeris/storage/geometry.hpp"
#include "aeris/storage/provenance.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect_true(const std::string_view name, const bool condition) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << name << '\n';
    }
}

class Fixture final {
public:
    Fixture() {
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path() /
                ("aeris-source-reader-" + std::to_string(stamp));
        std::filesystem::create_directories(root_);

        aeris::storage::ProjectCreateOptions options{};
        options.timestamp_utc = "2026-08-17T18:20:00Z";
        options.project_uuid = "3e207821-980a-44de-87af-0e8166917e7c";
        auto created = aeris::storage::ProjectStore::create(root_ / "world.aeris", options);
        if (created.ok()) project_ = std::move(created.store);
    }

    ~Fixture() {
        project_.reset();
        std::error_code ignored{};
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] aeris::storage::ProjectStore* project() noexcept {
        return project_.get();
    }

private:
    std::filesystem::path root_;
    std::unique_ptr<aeris::storage::ProjectStore> project_;
};

[[nodiscard]] aeris::storage::SourceSnapshotRecord source_record(
    std::string source_id,
    std::string content_hash,
    std::string worldview = {}
) {
    aeris::storage::SourceSnapshotRecord source{};
    source.source_id = std::move(source_id);
    source.adapter_id = "fixture.reader.v1";
    source.capability_bits = aeris::source::capability_bit(aeris::source::Capability::admin0);
    source.temporal_class = static_cast<std::uint8_t>(aeris::source::TemporalClass::slow_change);
    source.provider = "fixture-provider";
    source.dataset = "fixture-dataset";
    source.snapshot = "snapshot-1";
    source.dataset_version = "v1";
    source.source_uri = "fixture://durable-source";
    source.license_id = "CC0-1.0";
    source.content_sha256 = std::move(content_hash);
    source.retrieved_at_utc = "2026-08-17T18:20:01Z";
    source.worldview = std::move(worldview);
    return source;
}

[[nodiscard]] aeris::storage::GeographicRingRecord ring(
    const double offset,
    const aeris::storage::StoredRingRole role = aeris::storage::StoredRingRole::exterior
) {
    aeris::storage::GeographicRingRecord value{};
    value.role = role;
    value.interior_side = aeris::storage::StoredInteriorSide::right;
    value.longitude_winding = 0;
    value.closing_longitude_rad = offset;
    value.vertices = {
        {offset, 0.00},
        {offset + 0.10, 0.00},
        {offset + 0.10, 0.10},
        {offset, 0.10},
    };
    return value;
}

[[nodiscard]] aeris::storage::SourceGeometryRecord geometry_record(
    const std::string& source_id
) {
    aeris::storage::SourceGeometryRecord geometry{};
    geometry.source_id = source_id;

    aeris::storage::FeatureGeometryRecord alpha{};
    alpha.stable_id = "feature.alpha";
    alpha.source_feature_id = "record:11";
    alpha.rings.push_back(ring(0.00));

    aeris::storage::FeatureGeometryRecord beta{};
    beta.stable_id = "feature.beta";
    beta.source_feature_id = "record:12";
    beta.rings.push_back(ring(0.30));

    geometry.features.push_back(std::move(beta));
    geometry.features.push_back(std::move(alpha));
    return geometry;
}

[[nodiscard]] aeris::storage::SourceFeaturePropertiesRecord property_record(
    const std::string& source_id
) {
    aeris::storage::SourceFeaturePropertiesRecord properties{};
    properties.source_id = source_id;

    aeris::storage::FeaturePropertiesRecord alpha{};
    alpha.stable_id = "feature.alpha";
    alpha.properties.push_back({"ACTIVE", true});
    alpha.properties.push_back({"COUNT", std::int64_t{42}});
    alpha.properties.push_back({"RATIO", 0.625});
    alpha.properties.push_back({"NAME", std::string("Alpha")});

    aeris::storage::FeaturePropertiesRecord beta{};
    beta.stable_id = "feature.beta";
    // Verified-empty property list must survive as an explicit complete channel.

    properties.features.push_back(std::move(beta));
    properties.features.push_back(std::move(alpha));
    return properties;
}

[[nodiscard]] bool seed(
    aeris::storage::ProjectStore& project,
    const std::string& source_id,
    const std::string& hash,
    const bool with_properties,
    const std::string& worldview = {}
) {
    const auto provenance = aeris::storage::store_source_snapshot(
        project,
        source_record(source_id, hash, worldview),
        "2026-08-17T18:20:02Z");
    if (!provenance.ok()) return false;

    const auto geometry = aeris::storage::store_source_geometry(
        project,
        geometry_record(source_id),
        "2026-08-17T18:20:03Z");
    if (!geometry.ok()) return false;

    if (with_properties) {
        const auto properties = aeris::storage::store_source_feature_properties(
            project,
            property_record(source_id),
            "2026-08-17T18:20:04Z");
        if (!properties.ok()) return false;
    }
    return true;
}

[[nodiscard]] const aeris::source::Feature* find_feature(
    const aeris::source::Result& source,
    const std::string_view stable_id
) noexcept {
    for (const auto& feature : source.features) {
        if (feature.stable_id == stable_id) return &feature;
    }
    return nullptr;
}

void test_complete_properties_rehydrate() {
    Fixture fixture{};
    auto* project = fixture.project();
    expect_true("complete fixture creates", project != nullptr);
    if (project == nullptr) return;

    expect_true(
        "complete source seeds",
        seed(*project, "source.complete", std::string(64U, 'a'), true, "fixture.de-facto")
    );

    const auto loaded = aeris::project::load_durable_source_result(
        *project, "source.complete");
    expect_true("complete source rehydrates", loaded.ok());
    if (!loaded.ok()) {
        std::cerr << loaded.diagnostic << '\n';
        return;
    }

    expect_true("provenance provider survives",
                loaded.source.provenance.provider == "fixture-provider");
    expect_true("provenance worldview survives",
                loaded.source.provenance.worldview == "fixture.de-facto");
    expect_true("property completeness survives",
                loaded.source.feature_properties_complete);
    expect_true("two features survive", loaded.source.features.size() == 2U);

    const auto* alpha = find_feature(loaded.source, "feature.alpha");
    const auto* beta = find_feature(loaded.source, "feature.beta");
    expect_true("alpha and beta found", alpha != nullptr && beta != nullptr);
    if (alpha != nullptr) {
        expect_true("alpha source feature id survives", alpha->source_id == "record:11");
        expect_true("alpha ring survives", alpha->rings.size() == 1U);
        if (alpha->rings.size() == 1U) {
            expect_true("alpha ring role survives",
                        alpha->rings[0].role == aeris::source::RingRole::exterior);
            expect_true("alpha side survives",
                        alpha->rings[0].geometry.interior_side ==
                            aeris::geometry::RingInteriorSide::right);
            expect_true("alpha vertices survive",
                        alpha->rings[0].geometry.vertices.size() == 4U &&
                        alpha->rings[0].geometry.vertices[1].longitude_rad == 0.10);
        }
        expect_true("all typed properties survive", alpha->properties.size() == 4U);
        if (alpha->properties.size() == 4U) {
            expect_true("property order is canonical", alpha->properties[0].key == "ACTIVE" &&
                        alpha->properties[1].key == "COUNT" &&
                        alpha->properties[2].key == "NAME" &&
                        alpha->properties[3].key == "RATIO");
            expect_true("bool property survives",
                        std::get<bool>(alpha->properties[0].value));
            expect_true("int64 property survives",
                        std::get<std::int64_t>(alpha->properties[1].value) == 42);
            expect_true("string property survives",
                        std::get<std::string>(alpha->properties[2].value) == "Alpha");
            expect_true("double property survives",
                        std::get<double>(alpha->properties[3].value) == 0.625);
        }
    }
    if (beta != nullptr) {
        expect_true("verified-empty feature remains empty", beta->properties.empty());
    }
}

void test_geometry_only_rehydrate() {
    Fixture fixture{};
    auto* project = fixture.project();
    expect_true("geometry-only fixture creates", project != nullptr);
    if (project == nullptr) return;

    expect_true(
        "geometry-only source seeds",
        seed(*project, "source.geometry", std::string(64U, 'b'), false)
    );
    const auto loaded = aeris::project::load_durable_source_result(
        *project, "source.geometry");
    expect_true("geometry-only source rehydrates", loaded.ok());
    if (!loaded.ok()) return;
    expect_true("missing property marker remains incomplete",
                !loaded.source.feature_properties_complete);
    bool all_empty = true;
    for (const auto& feature : loaded.source.features) {
        all_empty = all_empty && feature.properties.empty();
    }
    expect_true("incomplete property channel never fabricates values", all_empty);
}

void test_missing_source_and_geometry() {
    Fixture fixture{};
    auto* project = fixture.project();
    expect_true("missing fixture creates", project != nullptr);
    if (project == nullptr) return;

    const auto missing = aeris::project::load_durable_source_result(
        *project, "source.missing");
    expect_true("missing source classified",
                missing.error == aeris::project::DurableSourceLoadError::source_not_found);

    const auto provenance = aeris::storage::store_source_snapshot(
        *project,
        source_record("source.provenance-only", std::string(64U, 'c')),
        "2026-08-17T18:20:02Z");
    expect_true("provenance-only source seeds", provenance.ok());
    const auto no_geometry = aeris::project::load_durable_source_result(
        *project, "source.provenance-only");
    expect_true("missing geometry classified",
                no_geometry.error == aeris::project::DurableSourceLoadError::geometry_unavailable);
}

}  // namespace

int main() {
    test_complete_properties_rehydrate();
    test_geometry_only_rehydrate();
    test_missing_source_and_geometry();

    if (failures != 0) {
        std::cerr << failures << " durable source reader assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "project_source_reader: PASS\n";
    return EXIT_SUCCESS;
}
