// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/source/adapter.hpp"

#include <filesystem>
#include <string>

namespace aeris::source {

struct NaturalEarthLandConfig final {
    std::filesystem::path dataset_directory;
    std::string snapshot;
    std::string source_uri;
    std::string retrieved_at_utc;
    std::string expected_shp_sha256;
};

class NaturalEarthLand110mAdapter final : public Adapter {
public:
    explicit NaturalEarthLand110mAdapter(NaturalEarthLandConfig config);

    [[nodiscard]] AdapterDescriptor descriptor() const noexcept override;
    [[nodiscard]] Result load(const Request& request) const override;

private:
    NaturalEarthLandConfig config_;
};

}  // namespace aeris::source
