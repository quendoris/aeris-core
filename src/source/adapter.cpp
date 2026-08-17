// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/source/adapter.hpp"

#include "aeris/util/text.hpp"

#include <cmath>
#include <set>

namespace aeris::source {
namespace {

constexpr std::size_t kMaxPropertyKeyBytes = 255U;
constexpr std::size_t kMaxPropertyTextBytes = 1024U * 1024U;

[[nodiscard]] bool valid_property(const FeatureProperty& property) noexcept {
    if (property.key.empty() || property.key.size() > kMaxPropertyKeyBytes ||
        !util::is_valid_utf8_nul_free(property.key)) {
        return false;
    }
    if (const auto* real = std::get_if<double>(&property.value)) {
        return std::isfinite(*real);
    }
    if (const auto* text = std::get_if<std::string>(&property.value)) {
        return text->size() <= kMaxPropertyTextBytes &&
               util::is_valid_utf8_nul_free(*text);
    }
    return true;
}

}  // namespace

bool Provenance::complete() const noexcept {
    return !provider.empty() && !dataset.empty() && !snapshot.empty() &&
           !source_uri.empty() && !license_id.empty() &&
           !content_sha256.empty() && !retrieved_at_utc.empty();
}

SourceError validate_result(
    const Adapter& adapter,
    const Request& request,
    const Result& result
) noexcept {
    if (!result.ok()) {
        return result.error;
    }

    const AdapterDescriptor descriptor = adapter.descriptor();
    if (request.capability == Capability::none) {
        return SourceError::invalid_request;
    }
    if (!has_capability(descriptor.capabilities, request.capability)) {
        return SourceError::unsupported_capability;
    }
    if (!result.provenance.complete()) {
        return SourceError::provenance_incomplete;
    }
    if (result.provenance.provider != descriptor.provider) {
        return SourceError::malformed_source;
    }
    if (!request.snapshot.empty() && result.provenance.snapshot != request.snapshot) {
        return SourceError::unavailable_snapshot;
    }
    if (!request.worldview.empty() && result.provenance.worldview != request.worldview) {
        return SourceError::unsupported_worldview;
    }

    for (const Feature& feature : result.features) {
        if (feature.stable_id.empty() || feature.source_id.empty() || feature.rings.empty()) {
            return SourceError::malformed_source;
        }
        for (const FeatureRing& ring : feature.rings) {
            if (ring.geometry.vertices.size() < 3U) {
                return SourceError::malformed_source;
            }
        }

        if (!result.feature_properties_complete && !feature.properties.empty()) {
            return SourceError::malformed_source;
        }
        std::set<std::string> property_keys;
        for (const FeatureProperty& property : feature.properties) {
            if (!valid_property(property) || !property_keys.insert(property.key).second) {
                return SourceError::malformed_source;
            }
        }
    }

    return SourceError::none;
}

}  // namespace aeris::source
