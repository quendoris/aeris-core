// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/project/world_layers.hpp"

#include "aeris/source/adapter.hpp"
#include "aeris/storage/feature_property.hpp"
#include "aeris/storage/geometry.hpp"
#include "aeris/storage/provenance.hpp"
#include "aeris/surface/classification.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace aeris::project {
namespace {

[[nodiscard]] WorldLayerStackResult storage_failure(
    const storage::Status& status,
    std::string prefix
) {
    if (!status.diagnostic.empty()) {
        prefix += ": ";
        prefix += status.diagnostic;
    }
    return {
        WorldLayerStackError::storage_rejected,
        status.error,
        false,
        false,
        std::move(prefix),
    };
}

[[nodiscard]] WorldLayerStackResult contract_mismatch(std::string diagnostic) {
    return {
        WorldLayerStackError::source_contract_mismatch,
        storage::StorageError::none,
        false,
        false,
        std::move(diagnostic),
    };
}

[[nodiscard]] const storage::SourceSnapshotRecord* find_source(
    const std::vector<storage::SourceSnapshotRecord>& records,
    const std::string& source_id
) noexcept {
    const auto found = std::find_if(
        records.begin(),
        records.end(),
        [&](const storage::SourceSnapshotRecord& record) {
            return record.source_id == source_id;
        }
    );
    return found == records.end() ? nullptr : &*found;
}

[[nodiscard]] storage::LayerCreateRequest layer(
    const std::string_view layer_id,
    const std::string_view role_id,
    std::string name
) {
    storage::LayerCreateRequest request{};
    request.layer_id = std::string(layer_id);
    request.role_id = std::string(role_id);
    request.name = std::move(name);
    request.visible = true;
    return request;
}

[[nodiscard]] std::vector<std::string> stable_ids(
    const std::vector<storage::FeatureGeometryIndexEntry>& features
) {
    std::vector<std::string> output;
    output.reserve(features.size());
    for (const auto& feature : features) output.push_back(feature.stable_id);
    std::sort(output.begin(), output.end());
    return output;
}

[[nodiscard]] std::vector<std::string> stable_ids(
    const std::vector<storage::FeaturePropertiesIndexEntry>& features
) {
    std::vector<std::string> output;
    output.reserve(features.size());
    for (const auto& feature : features) output.push_back(feature.stable_id);
    std::sort(output.begin(), output.end());
    return output;
}

[[nodiscard]] bool exact_surface_layer_wiring(
    const storage::ProjectLayerRecord& existing,
    const std::string& source_id
) noexcept {
    if (existing.role_id != storage::kLayerRolePhysicalSurfaceClassificationV1 ||
        !existing.resources.empty() || existing.sources.size() != 2U) {
        return false;
    }
    bool classification = false;
    bool geometry = false;
    for (const storage::LayerSourceBinding& binding : existing.sources) {
        if (binding.source_id != source_id) return false;
        if (binding.slot_id == "classification") {
            if (classification) return false;
            classification = true;
        } else if (binding.slot_id == "geometry") {
            if (geometry) return false;
            geometry = true;
        } else {
            return false;
        }
    }
    return classification && geometry;
}

[[nodiscard]] WorldLayerStackResult validate_surface_source(
    const storage::ProjectStore& project,
    const storage::SourceSnapshotRecord& source_record,
    const std::string& source_id
) {
    if (!source::has_capability(
            source_record.capability_bits,
            source::Capability::surface_classification
        )) {
        return contract_mismatch(
            "surface source does not advertise the surface_classification capability");
    }

    const auto geometry = storage::list_source_geometry_index(project, source_id);
    if (!geometry.ok()) {
        if (geometry.status.error == storage::StorageError::record_not_found) {
            return contract_mismatch("surface source has no durable feature geometry");
        }
        return storage_failure(
            geometry.status,
            "could not inspect surface-classification geometry index"
        );
    }
    const auto properties = storage::list_source_feature_properties_index(project, source_id);
    if (!properties.ok()) {
        if (properties.status.error == storage::StorageError::record_not_found) {
            return contract_mismatch(
                "surface source does not contain a complete durable property channel");
        }
        return storage_failure(
            properties.status,
            "could not inspect surface-classification property index"
        );
    }
    if (geometry.features.empty() ||
        geometry.features.size() != properties.features.size() ||
        stable_ids(geometry.features) != stable_ids(properties.features)) {
        return contract_mismatch(
            "surface source geometry/property indexes are empty or describe different features");
    }

    for (const storage::FeaturePropertiesIndexEntry& feature : properties.features) {
        const auto loaded = storage::load_feature_properties(
            project,
            source_id,
            feature.stable_id
        );
        if (!loaded.ok()) {
            return storage_failure(
                loaded.status,
                "could not load surface-classification properties for " + feature.stable_id
            );
        }
        bool found_class = false;
        for (const storage::StoredFeatureProperty& property : loaded.properties) {
            if (property.key != surface::kSurfaceClassPropertyKey) continue;
            if (found_class) {
                return contract_mismatch(
                    "surface feature contains duplicate canonical class properties");
            }
            const auto* text = std::get_if<std::string>(&property.value);
            if (text == nullptr || !surface::parse_surface_class_id(*text).has_value()) {
                return contract_mismatch(
                    "surface feature contains an invalid canonical class identifier");
            }
            found_class = true;
        }
        if (!found_class) {
            return contract_mismatch(
                "surface feature is missing the canonical aeris.surface_class.v1 property");
        }
    }

    return {};
}

}  // namespace

WorldLayerStackResult initialize_builtin_world_layer_stack(
    storage::ProjectStore& project,
    const BuiltinWorldLayerSources& sources,
    const std::string_view modified_utc
) {
    if (sources.physical_source_id.empty() ||
        sources.political_source_id.empty() ||
        sources.physical_source_id == sources.political_source_id) {
        return {
            WorldLayerStackError::invalid_request,
            storage::StorageError::none,
            false,
            false,
            "built-in world layers require distinct physical and political source IDs",
        };
    }

    const auto snapshots = storage::list_source_snapshots(project);
    if (!snapshots.ok()) {
        return storage_failure(snapshots.status, "could not inspect project source provenance");
    }

    const storage::SourceSnapshotRecord* physical =
        find_source(snapshots.records, sources.physical_source_id);
    const storage::SourceSnapshotRecord* political =
        find_source(snapshots.records, sources.political_source_id);
    if (physical == nullptr || political == nullptr) {
        return contract_mismatch(
            "built-in world layer source ID is not present in project provenance");
    }
    if (!source::has_capability(physical->capability_bits, source::Capability::land)) {
        return contract_mismatch(
            "physical source does not advertise the land capability");
    }
    if (!source::has_capability(political->capability_bits, source::Capability::admin0) ||
        political->worldview.empty()) {
        return contract_mismatch(
            "political source requires admin0 capability and explicit worldview provenance");
    }

    const auto physical_geometry =
        storage::list_source_geometry_index(project, sources.physical_source_id);
    if (!physical_geometry.ok()) {
        if (physical_geometry.status.error == storage::StorageError::record_not_found) {
            return contract_mismatch("physical source has no durable feature geometry");
        }
        return storage_failure(
            physical_geometry.status,
            "could not inspect physical source geometry index"
        );
    }
    const auto political_geometry =
        storage::list_source_geometry_index(project, sources.political_source_id);
    if (!political_geometry.ok()) {
        if (political_geometry.status.error == storage::StorageError::record_not_found) {
            return contract_mismatch("political source has no durable feature geometry");
        }
        return storage_failure(
            political_geometry.status,
            "could not inspect political source geometry index"
        );
    }
    const auto political_properties =
        storage::list_source_feature_properties_index(project, sources.political_source_id);
    if (!political_properties.ok()) {
        if (political_properties.status.error == storage::StorageError::record_not_found) {
            return contract_mismatch(
                "political source does not contain a complete durable property channel");
        }
        return storage_failure(
            political_properties.status,
            "could not inspect political source property index"
        );
    }

    if (physical_geometry.features.empty()) {
        return contract_mismatch("physical source has no durable feature geometry");
    }
    if (political_geometry.features.empty() ||
        political_properties.features.size() != political_geometry.features.size()) {
        return contract_mismatch(
            "political source geometry/property indexes are empty or cardinality-mismatched");
    }
    if (stable_ids(political_geometry.features) != stable_ids(political_properties.features)) {
        return contract_mismatch(
            "political source geometry/property indexes do not describe the same stable features");
    }

    std::vector<storage::LayerCreateRequest> stack;
    stack.reserve(5U);

    auto labels = layer(
        kBuiltinPoliticalLabelsLayerId,
        storage::kLayerRoleCountryLabelV1,
        "Country labels");
    labels.sources.push_back({"properties", sources.political_source_id});
    stack.push_back(std::move(labels));

    auto borders = layer(
        kBuiltinPoliticalBordersLayerId,
        storage::kLayerRolePoliticalBoundaryV1,
        "Borders");
    borders.sources.push_back({"geometry", sources.political_source_id});
    stack.push_back(std::move(borders));

    auto countries = layer(
        kBuiltinPoliticalCountriesLayerId,
        storage::kLayerRolePoliticalCountryFillV1,
        "Countries");
    countries.sources.push_back({"geometry", sources.political_source_id});
    countries.sources.push_back({"properties", sources.political_source_id});
    stack.push_back(std::move(countries));

    auto coastline = layer(
        kBuiltinPhysicalCoastlineLayerId,
        storage::kLayerRolePhysicalCoastlineV1,
        "Coastline");
    coastline.sources.push_back({"geometry", sources.physical_source_id});
    stack.push_back(std::move(coastline));

    auto land = layer(
        kBuiltinPhysicalLandLayerId,
        storage::kLayerRolePhysicalLandFillV1,
        "Land");
    land.sources.push_back({"geometry", sources.physical_source_id});
    stack.push_back(std::move(land));

    const storage::LayerMutationResult initialized =
        storage::initialize_layer_stack(project, stack, modified_utc);
    if (!initialized.ok()) {
        return {
            WorldLayerStackError::storage_rejected,
            initialized.status.error,
            initialized.changed,
            initialized.durably_committed,
            initialized.status.diagnostic,
        };
    }
    return {
        WorldLayerStackError::none,
        storage::StorageError::none,
        initialized.changed,
        initialized.durably_committed,
        {},
    };
}

WorldLayerStackResult ensure_builtin_surface_classification_layer(
    storage::ProjectStore& project,
    const std::string_view surface_source_id,
    const std::string_view modified_utc
) {
    if (surface_source_id.empty() ||
        surface_source_id.find('\0') != std::string_view::npos ||
        modified_utc.empty()) {
        return {
            WorldLayerStackError::invalid_request,
            storage::StorageError::none,
            false,
            false,
            "surface-classification layer requires a canonical source ID and timestamp",
        };
    }

    const std::string source_id(surface_source_id);
    const auto snapshots = storage::list_source_snapshots(project);
    if (!snapshots.ok()) {
        return storage_failure(
            snapshots.status,
            "could not inspect project source provenance for surface classification"
        );
    }
    const storage::SourceSnapshotRecord* source_record =
        find_source(snapshots.records, source_id);
    if (source_record == nullptr) {
        return contract_mismatch(
            "surface-classification source ID is not present in project provenance");
    }

    const WorldLayerStackResult validated =
        validate_surface_source(project, *source_record, source_id);
    if (!validated.ok()) return validated;

    const auto layers = storage::list_project_layers(project);
    if (!layers.ok()) {
        return storage_failure(
            layers.status,
            "could not inspect project layers before surface-classification composition"
        );
    }
    const auto existing = std::find_if(
        layers.records.begin(),
        layers.records.end(),
        [](const storage::ProjectLayerRecord& item) {
            return item.layer_id == kBuiltinSurfaceClassificationLayerId;
        }
    );
    if (existing != layers.records.end()) {
        if (!exact_surface_layer_wiring(*existing, source_id)) {
            return contract_mismatch(
                "existing built-in surface-classification layer has conflicting immutable wiring");
        }
        // Name and visibility are mutable user state. A bootstrap/update must
        // never rewrite those values just to make an exact structural retry.
        return {};
    }

    storage::LayerCreateRequest request = layer(
        kBuiltinSurfaceClassificationLayerId,
        storage::kLayerRolePhysicalSurfaceClassificationV1,
        "Surface classification");
    request.sources.push_back({"classification", source_id});
    request.sources.push_back({"geometry", source_id});

    const storage::LayerMutationResult appended =
        storage::append_layer(project, request, modified_utc);
    if (!appended.ok()) {
        return {
            WorldLayerStackError::storage_rejected,
            appended.status.error,
            appended.changed,
            appended.durably_committed,
            appended.status.diagnostic,
        };
    }
    return {
        WorldLayerStackError::none,
        storage::StorageError::none,
        appended.changed,
        appended.durably_committed,
        {},
    };
}

}  // namespace aeris::project
