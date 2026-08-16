// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/project/source_bridge.hpp"

#include "aeris/storage/provenance.hpp"

#include <limits>
#include <utility>

namespace aeris::project {
namespace {

[[nodiscard]] SourceBridgeResult failure(
    const SourceBridgeError error,
    std::string diagnostic
) {
    SourceBridgeResult result{};
    result.error = error;
    result.diagnostic = std::move(diagnostic);
    return result;
}

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

SourceBridgeResult record_verified_source_snapshot(
    storage::ProjectStore& project,
    const source::AdapterRegistry& registry,
    const source::VerifiedSnapshot& snapshot,
    const VerifiedSourceRecordRequest& request
) {
    if (request.source_id.empty() || request.source_id.find('\0') != std::string::npos ||
        !storage::is_canonical_utc_timestamp(request.modified_utc)) {
        return failure(
            SourceBridgeError::invalid_request,
            "project source ID must be non-empty/NUL-free and mutation time must be canonical Gregorian UTC"
        );
    }

    const source::RegistryLoadResult loaded = registry.load(request.binding, snapshot);
    if (!loaded.ok()) {
        SourceBridgeResult result = failure(
            SourceBridgeError::registry_rejected,
            loaded.diagnostic.empty() ? "source registry rejected verified snapshot" : loaded.diagnostic
        );
        result.registry_error = loaded.error;
        result.source_error = loaded.source_error;
        return result;
    }

    const source::Adapter* adapter = registry.find(request.binding.adapter_id);
    if (adapter == nullptr) {
        SourceBridgeResult result = failure(
            SourceBridgeError::registry_rejected,
            "source adapter disappeared after successful registry load"
        );
        result.registry_error = source::RegistryError::adapter_not_found;
        return result;
    }

    const source::SnapshotManifest& manifest = snapshot.manifest();
    const source::Provenance& provenance = loaded.source.provenance;
    if (!provenance_matches_verified_manifest(provenance, manifest, snapshot, request.binding)) {
        return failure(
            SourceBridgeError::verified_snapshot_mismatch,
            "adapter provenance does not describe the exact verified acquisition manifest"
        );
    }

    storage::SourceSnapshotRecord record{};
    record.source_id = request.source_id;
    record.adapter_id = std::string(adapter->descriptor().adapter_id);
    record.capability_bits = source::capability_bit(request.binding.capability);
    record.temporal_class = static_cast<std::uint8_t>(adapter->descriptor().temporal_class);
    record.provider = provenance.provider;
    record.dataset = provenance.dataset;
    record.snapshot = provenance.snapshot;
    record.dataset_version = provenance.dataset_version;
    record.source_uri = provenance.source_uri;
    record.license_id = provenance.license_id;
    record.content_sha256 = provenance.content_sha256;
    record.retrieved_at_utc = provenance.retrieved_at_utc;
    record.worldview = provenance.worldview;
    record.resources.reserve(manifest.resources.size());

    for (const source::ResourceSpec& resource : manifest.resources) {
        storage::SourceResourceRecord stored{};
        stored.logical_name = resource.logical_name;
        stored.sha256 = resource.sha256;
        if (resource.size_bytes.has_value()) {
            if (*resource.size_bytes >
                static_cast<std::uintmax_t>(std::numeric_limits<std::uint64_t>::max())) {
                return failure(
                    SourceBridgeError::resource_size_overflow,
                    "verified resource size exceeds the project format's uint64 range"
                );
            }
            stored.size_bytes = static_cast<std::uint64_t>(*resource.size_bytes);
        }
        record.resources.push_back(std::move(stored));
    }

    const storage::SourceSnapshotMutationResult stored =
        storage::store_source_snapshot(project, record, request.modified_utc);
    if (!stored.ok()) {
        SourceBridgeResult result = failure(
            SourceBridgeError::storage_rejected,
            stored.status.diagnostic.empty() ? "project storage rejected verified source provenance" : stored.status.diagnostic
        );
        result.storage_error = stored.status.error;
        // A storage error after SQLite commit (for example, failure to refresh
        // the caller's open ProjectStore metadata) must not erase the durable
        // outcome. Recovery logic needs to know that the mutation is already
        // present on disk even though the overall operation returned an error.
        result.inserted = stored.inserted;
        result.durably_committed = stored.durably_committed;
        return result;
    }

    SourceBridgeResult result{};
    result.inserted = stored.inserted;
    result.durably_committed = stored.durably_committed;
    return result;
}

}  // namespace aeris::project
