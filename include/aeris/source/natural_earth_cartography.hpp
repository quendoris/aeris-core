// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/source/adapter.hpp"

namespace aeris::source {

// Enriches the strict Natural Earth admin0 country result with cartographic
// attributes that are presentation-relevant but not required by the canonical
// political identity contract. Geometry, identity and worldview semantics remain
// those of NaturalEarthAdmin0Countries110mAdapter.
class NaturalEarthAdmin0Cartography110mAdapter final : public Adapter {
public:
    [[nodiscard]] AdapterDescriptor descriptor() const noexcept override;
    [[nodiscard]] Result load(
        const VerifiedSnapshot& snapshot,
        const Request& request
    ) const override;
};

}  // namespace aeris::source
