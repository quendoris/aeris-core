// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/source/registry.hpp"
#include "aeris/storage/project.hpp"

#include <cstdint>
#include <string>

namespace aeris::project {

enum class SourceBridgeError : std::uint8_t {
    none = 0U,
    invalid_request,
    registry_rejected,
    verified_snapshot_mismatch,
    resource_size_overflow,
    storage_rejected,
};

struct VerifiedSourceRecordRequest final {
    std::string source_id;
    source::SourceBinding binding;
    std::string modified_utc;
};

struct SourceBridgeResult final {
    SourceBridgeError error = SourceBridgeError::none;
    source::RegistryError registry_error = source::RegistryError::none;
    source::SourceError source_error = source::SourceError::none;
    storage::StorageError storage_error = storage::StorageError::none;

    // These outcome flags remain meaningful even when ok() is false. A storage
    // failure can occur after SQLite has durably committed the mutation (for
    // example while refreshing the caller's ProjectStore metadata), and callers
    // must not mistake such an error for "nothing reached disk".
    bool inserted = false;
    bool durably_committed = false;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept {
        return error == SourceBridgeError::none;
    }
};

// The bridge deliberately accepts a VerifiedSnapshot and AdapterRegistry rather
// than a caller-supplied source::Result. Adapter output must therefore pass the
// ordinary registry validation path and describe the exact verified acquisition
// before provenance, resource identity and canonical WGS84 feature geometry are
// committed to the project as one acknowledged dataset mutation.
[[nodiscard]] SourceBridgeResult record_verified_source_snapshot(
    storage::ProjectStore& project,
    const source::AdapterRegistry& registry,
    const source::VerifiedSnapshot& snapshot,
    const VerifiedSourceRecordRequest& request
);

}  // namespace aeris::project
