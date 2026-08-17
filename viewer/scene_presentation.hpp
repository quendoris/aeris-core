// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/source/adapter.hpp"
#include "scene.hpp"

namespace aeris::viewer {

// Attach source identity and presentation metadata after mathematical scene
// construction. This pass must never alter projected geometry or verification
// results; it only preserves source semantics for rendering/UI layers.
void apply_source_presentation(SceneData& scene, const source::Result& source);

}  // namespace aeris::viewer
