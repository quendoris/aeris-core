// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/source/adapter.hpp"

namespace aeris::source {

class NaturalEarthLand110mAdapter final : public Adapter {
public:
    [[nodiscard]] AdapterDescriptor descriptor() const noexcept override;
    [[nodiscard]] Result load(
        const VerifiedSnapshot& snapshot,
        const Request& request
    ) const override;
};

}  // namespace aeris::source
