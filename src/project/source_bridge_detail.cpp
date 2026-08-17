// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#include "source_bridge_detail.hpp"

#include <limits>
#include <utility>

namespace aeris::project::detail {
namespace {

[[nodiscard]] bool provenance_matches_verified_manifest(
    const source::Provenance& provenance,
    const source::SnapshotManifest& manifest,
    const source::VerifiedSnapshot& snapshot,
    const source::SourceBinding& binding
) noexcept {
    return binding.snapshot == manifest.snapshot &&
           provenance.provider == manifest.provider &&
           provenance.dataset == manifest.dataset &&
           provenance.snapshot == manifest.snapshot &&
           provenance.source_uri == manifest.source_uri &&
           provenance.retrieved_at_utc == manifest.retrieved_at_utc &&
           provenance.content_sha256 == snapshot.content_sha256();
}

}  // namespace

SourceBridgeResult bridge_failure(
    const SourceBridgeError error,
    std::string diagnostic
) {
    SourceBridgeResult result{};
    result.error = error;
    result.diagnostic = std::move(diagnostic);
    return result;
}

PrepareVerifiedSourceResult prepare_verified_source(
    const source::AdapterRegistry& registry,
    const source::VerifiedSnapshot& snapshot,
    const VerifiedSourceRecordRequest& request
) {
    if (request.source_id.empty() || request.source_id.find('\0') != std::string::npos ||
        !storage::is_canonical_utc_timestamp(request.modified_utc)) {
        return {
            bridge_failure(
                SourceBridgeError::invalid_request,
                "project source ID must be non-empty/NUL-free and mutation time must be canonical Gregorian UTC"),
            std::nullopt};
    }

    source::RegistryLoadResult loaded = registry.load(request.binding, snapshot);
    if (!loaded.ok()) {
        SourceBridgeResult result = bridge_failure(
            SourceBridgeError::registry_rejected,
            loaded.diagnostic.empty() ? "source registry rejected verified snapshot" : loaded.diagnostic);
        result.registry_error = loaded.error;
        result.source_error = loaded.source_error;
        return {std::move(result), std::nullopt};
    }

    const source::Adapter* adapter = registry.find(request.binding.adapter_id);
    if (adapter == nullptr) {
        SourceBridgeResult result = bridge_failure(
            SourceBridgeError::registry_rejected,
            "source adapter disappeared after successful registry load");
        result.registry_error = source::RegistryError::adapter_not_found;
        return {std::move(result), std::nullopt};
    }

    const source::SnapshotManifest& manifest = snapshot.manifest();
    const source::Provenance& provenance = loaded.source.provenance;
    if (!provenance_matches_verified_manifest(provenance, manifest, snapshot, request.binding)) {
        return {
            bridge_failure(
                SourceBridgeError::verified_snapshot_mismatch,
                "adapter provenance does not describe the exact verified acquisition manifest"),
            std::nullopt};
    }

    PreparedVerifiedSource prepared{};
    prepared.provenance.source_id = request.source_id;
    prepared.provenance.adapter_id = std::string(adapter->descriptor().adapter_id);
    prepared.provenance.capability_bits = source::capability_bit(request.binding.capability);
    prepared.provenance.temporal_class = static_cast<std::uint8_t>(adapter->descriptor().temporal_class);
    prepared.provenance.provider = provenance.provider;
    prepared.provenance.dataset = provenance.dataset;
    prepared.provenance.snapshot = provenance.snapshot;
    prepared.provenance.dataset_version = provenance.dataset_version;
    prepared.provenance.source_uri = provenance.source_uri;
    prepared.provenance.license_id = provenance.license_id;
    prepared.provenance.content_sha256 = provenance.content_sha256;
    prepared.provenance.retrieved_at_utc = provenance.retrieved_at_utc;
    prepared.provenance.worldview = provenance.worldview;
    prepared.provenance.resources.reserve(manifest.resources.size());

    for (const source::ResourceSpec& resource : manifest.resources) {
        storage::SourceResourceRecord stored{};
        stored.logical_name = resource.logical_name;
        stored.sha256 = resource.sha256;
        if (resource.size_bytes.has_value()) {
            if (*resource.size_bytes >
                static_cast<std::uintmax_t>(std::numeric_limits<std::uint64_t>::max())) {
                return {
                    bridge_failure(
                        SourceBridgeError::resource_size_overflow,
                        "verified resource size exceeds the project format's uint64 range"),
                    std::nullopt};
            }
            stored.size_bytes = static_cast<std::uint64_t>(*resource.size_bytes);
        }
        prepared.provenance.resources.push_back(std::move(stored));
    }

    prepared.source_result = std::move(loaded.source);
    return {SourceBridgeResult{}, std::move(prepared)};
}

}  // namespace aeris::project::detail
