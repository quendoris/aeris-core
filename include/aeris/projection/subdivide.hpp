// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/projection/primitives.hpp"

#include <cstddef>
#include <vector>

namespace aeris::projection {

using ProjectedCurveSampler = PlanarResult (*)(double parameter, void* context) noexcept;

enum class SubdivisionError {
    none = 0,
    invalid_options,
    sample_failed,
    non_finite_sample,
    limit_exceeded,
};

struct SubdivisionOptions final {
    double geometric_tolerance = 1.0;
    double area_tolerance = 1.0;
    unsigned max_depth = 32U;
    std::size_t max_segments = 1'000'000U;
};

struct SubdivisionResult final {
    std::vector<PlanarPoint> points;
    SubdivisionError error = SubdivisionError::none;
    geo::MathError sample_error = geo::MathError::none;
    unsigned deepest_level = 0U;

    [[nodiscard]] bool ok() const noexcept {
        return error == SubdivisionError::none;
    }
};

[[nodiscard]] SubdivisionResult subdivide_projected_curve(
    ProjectedCurveSampler sampler,
    void* context,
    const SubdivisionOptions& options
);

}  // namespace aeris::projection
