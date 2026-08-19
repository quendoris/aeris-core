// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "scene_presentation.hpp"

#include <cstdlib>
#include <iostream>

int main() {
    aeris::viewer::SceneData scene{};
    scene.ok = false;
    scene.diagnostic = "primary scene failure";

    aeris::source::Result source{};
    aeris::source::Feature feature{};
    feature.stable_id = "feature-1";
    source.features.push_back(std::move(feature));

    aeris::viewer::apply_source_presentation(scene, source);

    if (scene.ok || scene.diagnostic != "primary scene failure") {
        std::cerr << "presentation failure preservation: FAIL\n";
        return EXIT_FAILURE;
    }

    std::cout << "presentation failure preservation: PASS\n";
    return EXIT_SUCCESS;
}
