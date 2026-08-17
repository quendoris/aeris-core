// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "aeris/storage/project.hpp"

#include <string>
#include <string_view>

namespace aeris::storage {

inline constexpr std::string_view kProjectionModelUnspecified =
    "aeris.projection.unspecified";
inline constexpr std::string_view kProjectionModelSinusoidalV1 =
    "aeris.projection.sinusoidal.v1";
inline constexpr std::string_view kProjectionModelMollweideV1 =
    "aeris.projection.mollweide.v1";

inline constexpr std::string_view kProjectionCutModelUnspecifiedV1 =
    "aeris.cut.unspecified.v1";
inline constexpr std::string_view kProjectionCutModelSingleAntimeridianV1 =
    "aeris.cut.single-antimeridian.v1";

struct ProjectProjectionRecord final {
    // Versioned semantic projection identity. Known built-in models are checked
    // against the actual runtime contract; bounded unknown IDs remain preservable
    // during pre-1.0 evolution but are never silently interpreted as a known model.
    std::string model_id;

    // Semantic map orientation, not viewer camera state. Persisted canonically as
    // little-endian IEEE-754 binary64 radians in (-pi, pi], with -0 normalized.
    double central_meridian_rad{0.0};

    // Explicit versioned topology contract for the cut drawn on the reference
    // globe before flattening. Current Sinusoidal/Mollweide runtime semantics use
    // the single antimeridian opposite the central meridian.
    std::string cut_model_id;
};

struct ProjectionMutationResult final {
    Status status;
    bool changed{false};
    bool durably_committed{false};

    [[nodiscard]] bool ok() const noexcept { return status.ok(); }
};

struct ProjectProjectionResult final {
    Status status;
    ProjectProjectionRecord record;

    [[nodiscard]] bool ok() const noexcept { return status.ok(); }
};

// Replaces the complete semantic projection definition in one durable project
// mutation and synchronizes aeris_meta.projection_id to model_id atomically.
// Exact retry is idempotent. Numerical subdivision limits, iteration budgets and
// viewer camera state are runtime/session policy and deliberately are not stored.
[[nodiscard]] ProjectionMutationResult set_project_projection(
    ProjectStore& project,
    const ProjectProjectionRecord& record,
    std::string_view modified_utc);

[[nodiscard]] ProjectProjectionResult load_project_projection(
    const ProjectStore& project);

}  // namespace aeris::storage
