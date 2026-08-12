// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/projection/subdivide.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void expect_true(const std::string_view name, const bool condition) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << name << '\n';
    }
}

struct CurveContext final {
    double curvature = 0.0;
};

aeris::projection::PlanarResult sample_parabola(const double t, void* const context) noexcept {
    const auto* curve = static_cast<const CurveContext*>(context);
    if (curve == nullptr) {
        return {{}, aeris::geo::MathError::numerical_domain_error};
    }

    return {{t, curve->curvature * t * (1.0 - t)}, aeris::geo::MathError::none};
}

aeris::projection::PlanarResult sample_failure(const double t, void*) noexcept {
    if (t > 0.4 && t < 0.6) {
        return {{}, aeris::geo::MathError::numerical_domain_error};
    }
    return {{t, 0.0}, aeris::geo::MathError::none};
}

void test_straight_line_is_minimal() {
    CurveContext context{0.0};
    aeris::projection::SubdivisionOptions options{};
    options.geometric_tolerance = 1e-12;
    options.area_tolerance = 1e-12;

    const auto result = aeris::projection::subdivide_projected_curve(sample_parabola, &context, options);
    expect_true("straight subdivision succeeds", result.ok());
    expect_true("straight curve has one segment", result.points.size() == 2U);
}

void test_curved_line_refines() {
    CurveContext context{1.0};
    aeris::projection::SubdivisionOptions options{};
    options.geometric_tolerance = 1e-3;
    options.area_tolerance = 1e-4;

    const auto result = aeris::projection::subdivide_projected_curve(sample_parabola, &context, options);
    expect_true("curved subdivision succeeds", result.ok());
    expect_true("curved subdivision adds points", result.points.size() > 2U);
    expect_true("curved subdivision remains bounded", result.points.size() < 1024U);
}

void test_fail_closed_limits_and_samples() {
    CurveContext context{1.0};
    aeris::projection::SubdivisionOptions impossible{};
    impossible.geometric_tolerance = 1e-30;
    impossible.area_tolerance = 1e-30;
    impossible.max_depth = 1U;
    impossible.max_segments = 8U;

    const auto limited = aeris::projection::subdivide_projected_curve(sample_parabola, &context, impossible);
    expect_true(
        "hard subdivision limit fails closed",
        limited.error == aeris::projection::SubdivisionError::limit_exceeded
    );

    aeris::projection::SubdivisionOptions normal{};
    normal.geometric_tolerance = 1e-3;
    normal.area_tolerance = 1e-3;
    const auto failed = aeris::projection::subdivide_projected_curve(sample_failure, nullptr, normal);
    expect_true(
        "sampler error propagates",
        failed.error == aeris::projection::SubdivisionError::sample_failed
    );
    expect_true(
        "sampler math error is preserved",
        failed.sample_error == aeris::geo::MathError::numerical_domain_error
    );
}

void test_invalid_options() {
    aeris::projection::SubdivisionOptions invalid{};
    invalid.geometric_tolerance = 0.0;
    const auto result = aeris::projection::subdivide_projected_curve(sample_failure, nullptr, invalid);
    expect_true(
        "zero geometric tolerance rejected",
        result.error == aeris::projection::SubdivisionError::invalid_options
    );
}

}  // namespace

int main() {
    test_straight_line_is_minimal();
    test_curved_line_refines();
    test_fail_closed_limits_and_samples();
    test_invalid_options();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "adaptive_subdivision: PASS\n";
    return EXIT_SUCCESS;
}
