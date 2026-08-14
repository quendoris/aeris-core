// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/source/adapter.hpp"
#include "scene.hpp"

#include <functional>

namespace aeris::viewer {

using CancelCheck = std::function<bool()>;

[[nodiscard]] SceneData build_scene(
    const source::Result& world,
    const SceneRequest& request,
    const CancelCheck& canceled = {}
);

}  // namespace aeris::viewer
