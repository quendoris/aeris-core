// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/projection/adapter.hpp"

namespace aeris::projection {

const ProjectionAdapter& projection_adapter_for_primitive(
    const EqualAreaPrimitive primitive
) noexcept {
    switch (primitive) {
        case EqualAreaPrimitive::sinusoidal:
            return sinusoidal_projection_adapter();
        case EqualAreaPrimitive::mollweide:
            return mollweide_projection_adapter();
    }
    return sinusoidal_projection_adapter();
}

}  // namespace aeris::projection
