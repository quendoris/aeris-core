// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "aeris/project/source_bridge.hpp"
#include "aeris/storage/provenance.hpp"

#include <optional>

namespace aeris::project::detail {

struct PreparedVerifiedSource final {
    storage::SourceSnapshotRecord provenance;
    source::Result source_result;
};

struct PrepareVerifiedSourceResult final {
    SourceBridgeResult status;
    std::optional<PreparedVerifiedSource> value;

    [[nodiscard]] bool ok() const noexcept {
        return status.ok() && value.has_value();
    }
};

[[nodiscard]] PrepareVerifiedSourceResult prepare_verified_source(
    const source::AdapterRegistry& registry,
    const source::VerifiedSnapshot& snapshot,
    const VerifiedSourceRecordRequest& request);

[[nodiscard]] SourceBridgeResult bridge_failure(
    SourceBridgeError error,
    std::string diagnostic);

}  // namespace aeris::project::detail
