// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/project/world_layers.hpp"

#include "aeris/source/adapter.hpp"
#include "aeris/storage/feature_property.hpp"
#include "aeris/storage/geometry.hpp"
#include "aeris/storage/provenance.hpp"

#include <algorithm>
#include <string>
#include <utility>
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

}  // namespace aeris::project
