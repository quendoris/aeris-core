// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/source/adapter.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace aeris::viewer {

enum class MapContent {
    physical = 0,
    political,
};

struct WorldLoadResult final {
    std::shared_ptr<const source::Result> world;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept {
        return static_cast<bool>(world) && world->ok();
    }
};

struct WorkbenchWorlds final {
    std::shared_ptr<const source::Result> physical;
    std::shared_ptr<const source::Result> political;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept {
        return static_cast<bool>(physical) && physical->ok() &&
               static_cast<bool>(political) && political->ok();
    }

    [[nodiscard]] std::shared_ptr<const source::Result> select(
        MapContent content
    ) const noexcept {
        return content == MapContent::political ? political : physical;
    }
};

[[nodiscard]] WorldLoadResult load_pinned_demo_world(
    const std::filesystem::path& snapshot_root,
    std::string retrieved_at_utc
);

[[nodiscard]] WorldLoadResult load_pinned_political_world(
    const std::filesystem::path& snapshot_root,
    std::string retrieved_at_utc
);

[[nodiscard]] WorkbenchWorlds load_pinned_workbench_worlds(
    const std::filesystem::path& snapshot_root,
    std::string retrieved_at_utc
);

[[nodiscard]] const char* map_content_name(MapContent content) noexcept;

}  // namespace aeris::viewer
