// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/view/projection_catalog.hpp"
#include "aeris/view/scene.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void expect_true(const std::string_view name, const bool condition) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << name << '\n';
    }
}

}  // namespace

int main() {
    using aeris::view::SurfaceMode;

    const auto& catalog = aeris::view::projection_catalog();
    expect_true("projection catalog has three supported planar surfaces", catalog.size() == 3U);

    const SurfaceMode expected_modes[] = {
        SurfaceMode::sinu_mollweide,
        SurfaceMode::mollweide,
        SurfaceMode::sinusoidal,
    };
    const std::string_view expected_ids[] = {
        "sinu-mollweide",
        "mollweide",
        "sinusoidal",
    };
    const std::string_view expected_names[] = {
        "Sinu-Mollweide",
        "Mollweide",
        "Sinusoidal",
    };

    for (std::size_t index = 0U; index < catalog.size(); ++index) {
        const auto& descriptor = catalog[index];
        expect_true("catalog preserves user-facing projection order", descriptor.mode == expected_modes[index]);
        expect_true("catalog exposes stable projection id", std::string_view(descriptor.stable_id) == expected_ids[index]);
        expect_true("catalog exposes display name", std::string_view(descriptor.display_name) == expected_names[index]);
        expect_true("catalog projection is equal-area", descriptor.capabilities.equal_area);
        expect_true("catalog projection has inverse mapping", descriptor.capabilities.inverse_mapping);
        expect_true("catalog projection has interactive cut", descriptor.capabilities.interactive_cut);

        const auto* found = aeris::view::find_projection_descriptor(descriptor.mode);
        expect_true("catalog lookup returns descriptor", found != nullptr);
        expect_true("catalog lookup returns stable catalog entry", found == &descriptor);
        if (found != nullptr) {
            expect_true(
                "surface mode name agrees with catalog display name",
                std::string_view(aeris::view::surface_mode_name(descriptor.mode)) ==
                    std::string_view(found->display_name)
            );
        }
    }

    expect_true(
        "Globe is not a planar projection catalog entry",
        aeris::view::find_projection_descriptor(SurfaceMode::globe) == nullptr
    );

    if (failures != 0) {
        std::cerr << failures << " projection catalog assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "projection_catalog: PASS\n";
    return EXIT_SUCCESS;
}
