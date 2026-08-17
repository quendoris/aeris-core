// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "scene_presentation.hpp"

#include <string>
#include <string_view>

namespace aeris::viewer {
namespace {

const source::FeatureProperty* find_property(
    const source::Feature& feature,
    const std::string_view key
) noexcept {
    for (const source::FeatureProperty& property : feature.properties) {
        if (property.key == key) return &property;
    }
    return nullptr;
}

bool copy_text_property(
    const source::Feature& feature,
    const std::string_view key,
    std::string& output
) {
    const source::FeatureProperty* property = find_property(feature, key);
    if (property == nullptr) return false;
    const auto* text = std::get_if<std::string>(&property->value);
    if (text == nullptr) return false;
    output = *text;
    return true;
}

}  // namespace

void apply_source_presentation(SceneData& scene, const source::Result& source) {
    scene.political = false;
    if (scene.features.size() != source.features.size()) {
        scene.ok = false;
        scene.diagnostic = "scene/source feature cardinality changed before presentation pass";
        return;
    }

    bool complete_political_contract =
        source.feature_properties_complete && !source.features.empty();

    for (std::size_t index = 0U; index < source.features.size(); ++index) {
        const source::Feature& input = source.features[index];
        SceneFeature& output = scene.features[index];
        output.stable_id = input.stable_id;
        output.style_key = input.stable_id;
        output.label.clear();
        output.iso_a2.clear();

        std::string adm0_a3;
        const bool has_name = copy_text_property(input, "name", output.label);
        const bool has_iso = copy_text_property(input, "iso_a2", output.iso_a2);
        const bool has_admin_code = copy_text_property(input, "adm0_a3", adm0_a3);
        if (has_admin_code && !adm0_a3.empty()) output.style_key = std::move(adm0_a3);

        complete_political_contract = complete_political_contract &&
            has_name && has_iso && has_admin_code && !output.style_key.empty();
    }

    scene.political = complete_political_contract;
}

}  // namespace aeris::viewer
