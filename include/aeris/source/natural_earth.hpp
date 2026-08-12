// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/source/acquisition.hpp"
#include "aeris/source/adapter.hpp"

namespace aeris::source {

class NaturalEarthLand110mAdapter final : public Adapter {
public:
    explicit NaturalEarthLand110mAdapter(VerifiedSnapshot snapshot);

    [[nodiscard]] AdapterDescriptor descriptor() const noexcept override;
    [[nodiscard]] Result load(const Request& request) const override;

private:
    VerifiedSnapshot snapshot_;
};

}  // namespace aeris::source
