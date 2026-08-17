// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "layer_stack.hpp"

#include "aeris/storage/layer.hpp"

namespace aeris::viewer {

LayerStackState::LayerStackState(const MapContent content) noexcept
    : content_(content) {}

void LayerStackState::set_content(const MapContent content) noexcept {
    content_ = content;
}

std::vector<LayerDescriptor> LayerStackState::active_layers() const {
    if (content_ == MapContent::political) {
        return {
            {
                kLayerIdPoliticalLabels,
                storage::kLayerRoleCountryLabelV1,
                "Country labels",
                political_labels_visible_,
            },
            {
                kLayerIdPoliticalBorders,
                storage::kLayerRolePoliticalBoundaryV1,
                "Borders",
                political_borders_visible_,
            },
            {
                kLayerIdPoliticalCountries,
                storage::kLayerRolePoliticalCountryFillV1,
                "Countries",
                political_countries_visible_,
            },
        };
    }

    return {
        {
            kLayerIdPhysicalCoastline,
            storage::kLayerRolePhysicalCoastlineV1,
            "Coastline",
            physical_coastline_visible_,
        },
        {
            kLayerIdPhysicalLand,
            storage::kLayerRolePhysicalLandFillV1,
            "Land",
            physical_land_visible_,
        },
    };
}

bool LayerStackState::set_visible(
    const std::string_view layer_id,
    const bool visible_value
) noexcept {
    if (layer_id == kLayerIdPhysicalLand) {
        physical_land_visible_ = visible_value;
        return true;
    }
    if (layer_id == kLayerIdPhysicalCoastline) {
        physical_coastline_visible_ = visible_value;
        return true;
    }
    if (layer_id == kLayerIdPoliticalCountries) {
        political_countries_visible_ = visible_value;
        return true;
    }
    if (layer_id == kLayerIdPoliticalBorders) {
        political_borders_visible_ = visible_value;
        return true;
    }
    if (layer_id == kLayerIdPoliticalLabels) {
        political_labels_visible_ = visible_value;
        return true;
    }
    return false;
}

bool LayerStackState::visible(const std::string_view layer_id) const noexcept {
    if (layer_id == kLayerIdPhysicalLand) return physical_land_visible_;
    if (layer_id == kLayerIdPhysicalCoastline) return physical_coastline_visible_;
    if (layer_id == kLayerIdPoliticalCountries) return political_countries_visible_;
    if (layer_id == kLayerIdPoliticalBorders) return political_borders_visible_;
    if (layer_id == kLayerIdPoliticalLabels) return political_labels_visible_;
    return false;
}

LayerRenderState LayerStackState::render_state() const noexcept {
    if (content_ == MapContent::political) {
        return {
            political_countries_visible_,
            political_borders_visible_,
            political_labels_visible_,
        };
    }
    return {
        physical_land_visible_,
        physical_coastline_visible_,
        false,
    };
}

}  // namespace aeris::viewer
