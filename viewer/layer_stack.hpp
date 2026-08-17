// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "world_loader.hpp"

#include <string_view>
#include <vector>

namespace aeris::viewer {

inline constexpr std::string_view kLayerIdPhysicalLand =
    "builtin.physical.land";
inline constexpr std::string_view kLayerIdPhysicalCoastline =
    "builtin.physical.coastline";
inline constexpr std::string_view kLayerIdPoliticalCountries =
    "builtin.political.countries";
inline constexpr std::string_view kLayerIdPoliticalBorders =
    "builtin.political.borders";
inline constexpr std::string_view kLayerIdPoliticalLabels =
    "builtin.political.labels";

struct LayerDescriptor final {
    std::string_view layer_id;
    std::string_view role_id;
    std::string_view name;
    bool visible{true};
};

struct LayerRenderState final {
    bool fill_visible{true};
    bool outline_visible{true};
    bool labels_visible{false};
};

// Runtime presentation state for the demo workbench. Stable layer IDs and role
// IDs intentionally match the durable ProjectLayerRecord contract, while this
// object remains storage-neutral: connecting visibility mutations to .aeris is
// a separate project/writer boundary rather than a hidden Qt-side save path.
class LayerStackState final {
public:
    explicit LayerStackState(MapContent content = MapContent::physical) noexcept;

    void set_content(MapContent content) noexcept;
    [[nodiscard]] MapContent content() const noexcept { return content_; }

    [[nodiscard]] std::vector<LayerDescriptor> active_layers() const;
    [[nodiscard]] bool set_visible(std::string_view layer_id, bool visible) noexcept;
    [[nodiscard]] bool visible(std::string_view layer_id) const noexcept;
    [[nodiscard]] LayerRenderState render_state() const noexcept;

private:
    MapContent content_{MapContent::physical};
    bool physical_land_visible_{true};
    bool physical_coastline_visible_{true};
    bool political_countries_visible_{true};
    bool political_borders_visible_{true};
    bool political_labels_visible_{true};
};

}  // namespace aeris::viewer
