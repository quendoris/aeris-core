// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/source/adapter.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace aeris::viewer {

struct WorldLoadResult final {
    std::shared_ptr<const source::Result> world;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept {
        return static_cast<bool>(world) && world->ok();
    }
};

[[nodiscard]] WorldLoadResult load_pinned_demo_world(
    const std::filesystem::path& snapshot_root,
    std::string retrieved_at_utc
);

}  // namespace aeris::viewer
