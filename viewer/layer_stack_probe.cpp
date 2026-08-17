// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "layer_stack.hpp"

#include "aeris/storage/layer.hpp"

#include <cstdlib>
#include <iostream>

int main() {
    aeris::viewer::LayerStackState stack(aeris::viewer::MapContent::physical);
    const auto physical = stack.active_layers();
    if (physical.size() != 2U ||
        physical.front().role_id != aeris::storage::kLayerRolePhysicalCoastlineV1 ||
        physical.back().role_id != aeris::storage::kLayerRolePhysicalLandFillV1) {
        std::cerr << "physical layer mapping failed\n";
        return EXIT_FAILURE;
    }

    stack.set_content(aeris::viewer::MapContent::political);
    const auto political = stack.active_layers();
    if (political.size() != 3U ||
        political[0].role_id != aeris::storage::kLayerRoleCountryLabelV1 ||
        political[1].role_id != aeris::storage::kLayerRolePoliticalBoundaryV1 ||
        political[2].role_id != aeris::storage::kLayerRolePoliticalCountryFillV1) {
        std::cerr << "political layer mapping failed\n";
        return EXIT_FAILURE;
    }

    if (!stack.set_visible(aeris::viewer::kLayerIdPoliticalLabels, false) ||
        !stack.set_visible(aeris::viewer::kLayerIdPoliticalCountries, false) ||
        stack.set_visible("unknown", false)) {
        std::cerr << "layer visibility mutation contract failed\n";
        return EXIT_FAILURE;
    }

    const aeris::viewer::LayerRenderState state = stack.render_state();
    if (state.fill_visible || !state.outline_visible || state.labels_visible) {
        std::cerr << "render state mapping failed\n";
        return EXIT_FAILURE;
    }

    stack.set_content(aeris::viewer::MapContent::physical);
    stack.set_content(aeris::viewer::MapContent::political);
    if (stack.visible(aeris::viewer::kLayerIdPoliticalLabels) ||
        stack.visible(aeris::viewer::kLayerIdPoliticalCountries)) {
        std::cerr << "layer state was lost across content switching\n";
        return EXIT_FAILURE;
    }

    std::cout << "viewer_layer_stack_probe: PASS\n";
    return EXIT_SUCCESS;
}
