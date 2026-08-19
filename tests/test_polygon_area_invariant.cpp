// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/geometry/geographic.hpp"
#include "aeris/geometry/planar.hpp"
#include "aeris/projection/adapter.hpp"
#include "aeris/projection/subdivide.hpp"

#include "aeris/geo/wgs84.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

[[nodiscard]] double radians(const double degrees) {
    return degrees * aeris::geo::kPi / 180.0;
}

void expect_true(const std::string_view name, const bool condition) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << name << '\n';
    }
}

struct EdgeContext final {
    aeris::geometry::GeodeticPoint start{};
    aeris::geometry::GeodeticPoint end{};
    const aeris::projection::ProjectionAdapter* adapter = nullptr;
    double central_meridian_rad = 0.0;
};

[[nodiscard]] aeris::projection::PlanarResult sample_edge(
    const double parameter,
    void* const opaque_context
) noexcept {
    if (opaque_context == nullptr || !std::isfinite(parameter)) {
        return {{}, aeris::geo::MathError::non_finite_input};
    }

    const auto* const context = static_cast<const EdgeContext*>(opaque_context);
    if (context->adapter == nullptr) {
        return {{}, aeris::geo::MathError::numerical_domain_error};
    }
    const auto point = aeris::geometry::interpolate_wgs84_linear_edge(
        context->start,
        context->end,
        parameter
    );

    return context->adapter->forward_wgs84(
        point.longitude_rad,
        point.latitude_rad,
        context->central_meridian_rad
    );
}

[[nodiscard]] bool project_ring(
    const aeris::geometry::LinearRing& ring,
    const aeris::projection::ProjectionAdapter& adapter,
    const double central_meridian_rad,
    std::vector<aeris::geometry::PlanarPoint>& output
) {
    output.clear();

    const aeris::projection::SubdivisionOptions options{
        0.5,
        10.0,
        32U,
        250'000U,
    };

    for (std::size_t index = 0U; index < ring.vertices.size(); ++index) {
        EdgeContext context{};
        context.start = ring.vertices[index];
        context.end = index + 1U < ring.vertices.size()
            ? ring.vertices[index + 1U]
            : aeris::geometry::GeodeticPoint{
                  ring.closing_longitude_rad,
                  ring.vertices.front().latitude_rad,
              };
        context.adapter = &adapter;
        context.central_meridian_rad = central_meridian_rad;

        const auto edge = aeris::projection::subdivide_projected_curve(
            sample_edge,
            &context,
            options
        );
        if (!edge.ok()) {
            std::cerr << "projection subdivision failed on edge " << index
                      << " with subdivision error " << static_cast<int>(edge.error)
                      << " and sample error " << static_cast<int>(edge.sample_error)
                      << '\n';
            return false;
        }

        if (output.empty()) {
            output.insert(output.end(), edge.points.begin(), edge.points.end());
        } else if (edge.points.size() > 1U) {
            output.insert(output.end(), edge.points.begin() + 1, edge.points.end());
        }
    }

    return output.size() >= 4U;
}

void check_area_invariant(
    const std::string_view name,
    const std::vector<aeris::geometry::GeodeticPoint>& input,
    const double central_meridian_rad
) {
    const auto canonical = aeris::geometry::canonicalize_wgs84_linear_ring(input);
    expect_true("area invariant ring canonicalizes", canonical.ok());
    if (!canonical.ok()) return;

    const auto geographic_area =
        aeris::geometry::signed_wgs84_linear_ring_area(canonical.value);
    expect_true("area invariant WGS84 area succeeds", geographic_area.ok());
    if (!geographic_area.ok()) return;

    for (const auto* adapter : aeris::projection::builtin_projection_adapters()) {
        expect_true("area invariant adapter is present", adapter != nullptr);
        if (adapter == nullptr) continue;

        std::vector<aeris::geometry::PlanarPoint> projected;
        const bool projected_ok = project_ring(
            canonical.value,
            *adapter,
            central_meridian_rad,
            projected
        );
        expect_true("area invariant projection succeeds", projected_ok);
        if (!projected_ok) continue;

        const double planar_area = aeris::geometry::signed_planar_area(projected);
        expect_true("projected polygon area is finite", std::isfinite(planar_area));
        expect_true("projected polygon preserves orientation", planar_area > 0.0);

        const double relative_error = std::abs(
            (planar_area - geographic_area.signed_area_m2) /
            geographic_area.signed_area_m2
        );

        if (!std::isfinite(relative_error) || relative_error > 1e-8) {
            ++failures;
            std::cerr << "FAIL " << name
                      << ": adapter=" << adapter->descriptor().model_id
                      << " geographic=" << geographic_area.signed_area_m2
                      << " planar=" << planar_area
                      << " relative_error=" << relative_error
                      << " vertices=" << projected.size() << '\n';
        }
    }
}

void test_oblique_polygon() {
    check_area_invariant(
        "oblique polygon preserves WGS84 area",
        {
            {radians(-35.0), radians(-20.0)},
            {radians(25.0), radians(-12.0)},
            {radians(40.0), radians(28.0)},
            {radians(-15.0), radians(42.0)},
        },
        0.0
    );
}

void test_antimeridian_polygon() {
    check_area_invariant(
        "antimeridian polygon preserves WGS84 area",
        {
            {radians(170.0), radians(-20.0)},
            {radians(-170.0), radians(-20.0)},
            {radians(-170.0), radians(20.0)},
            {radians(170.0), radians(20.0)},
        },
        aeris::geo::kPi
    );
}

}  // namespace

int main() {
    test_oblique_polygon();
    test_antimeridian_polygon();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "polygon_area_invariant: PASS\n";
    return EXIT_SUCCESS;
}
