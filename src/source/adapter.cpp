// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/source/adapter.hpp"

namespace aeris::source {

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
    }

    return SourceError::none;
}

}  // namespace aeris::source
