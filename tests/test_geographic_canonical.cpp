// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/geometry/geographic.hpp"
#include "aeris/geo/wgs84.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect_true(const std::string_view name, const bool condition) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << name << '\n';
    }
}

[[nodiscard]] double radians(const double degrees) noexcept {
    return degrees * aeris::geo::kPi / 180.0;
}

void test_constructor_output_validates_directly() {
    const std::vector<aeris::geometry::GeodeticPoint> points{
        {radians(170.0), radians(-10.0)},
        {radians(-170.0), radians(-10.0)},
        {radians(-170.0), radians(10.0)},
        {radians(170.0), radians(10.0)},
    };
    const auto ring = aeris::geometry::canonicalize_wgs84_linear_ring(points);
    expect_true("ordinary ring constructs", ring.ok());
    if (ring.ok()) {
        expect_true(
            "ordinary constructor output validates",
            aeris::geometry::validate_canonical_wgs84_linear_ring(ring.value) ==
                aeris::geometry::GeographicError::none
        );
    }
}

void test_winding_topology_and_closing_relation() {
    const std::vector<aeris::geometry::GeodeticPoint> points{
        {radians(0.0), radians(80.0)},
        {radians(90.0), radians(80.0)},
        {radians(179.0), radians(80.0)},
        {radians(-90.0), radians(80.0)},
    };
    auto ring = aeris::geometry::canonicalize_wgs84_linear_ring(points);
    expect_true("winding ring constructs", ring.ok());
    if (!ring.ok()) return;

    expect_true("winding ring exposes nonzero winding", ring.value.longitude_winding != 0);
    expect_true(
        "winding ring without topology fails closed",
        aeris::geometry::validate_canonical_wgs84_linear_ring(ring.value) ==
            aeris::geometry::GeographicError::longitude_winding_unsupported
    );

    ring.value.interior_side = aeris::geometry::RingInteriorSide::left;
    expect_true(
        "winding ring with explicit topology validates",
        aeris::geometry::validate_canonical_wgs84_linear_ring(ring.value) ==
            aeris::geometry::GeographicError::none
    );

    ring.value.closing_longitude_rad += 0.25;
    expect_true(
        "closing longitude drift is noncanonical",
        aeris::geometry::validate_canonical_wgs84_linear_ring(ring.value) ==
            aeris::geometry::GeographicError::noncanonical_ring
    );
}

void test_noncanonical_representation_rejected() {
    const std::vector<aeris::geometry::GeodeticPoint> points{
        {radians(-30.0), radians(-10.0)},
        {radians(20.0), radians(-10.0)},
        {radians(20.0), radians(20.0)},
        {radians(-30.0), radians(20.0)},
    };
    const auto canonical = aeris::geometry::canonicalize_wgs84_linear_ring(points);
    expect_true("validator fixture constructs", canonical.ok());
    if (!canonical.ok()) return;

    auto shifted_first = canonical.value;
    shifted_first.vertices.front().longitude_rad += 2.0 * aeris::geo::kPi;
    expect_true(
        "first longitude outside canonical branch rejected",
        aeris::geometry::validate_canonical_wgs84_linear_ring(shifted_first) ==
            aeris::geometry::GeographicError::noncanonical_ring
    );

    auto duplicate_close = canonical.value;
    duplicate_close.vertices.push_back({
        duplicate_close.vertices.front().longitude_rad,
        duplicate_close.vertices.front().latitude_rad,
    });
    expect_true(
        "duplicate terminal closure rejected",
        aeris::geometry::validate_canonical_wgs84_linear_ring(duplicate_close) ==
            aeris::geometry::GeographicError::noncanonical_ring
    );
}

}  // namespace

int main() {
    test_constructor_output_validates_directly();
    test_winding_topology_and_closing_relation();
    test_noncanonical_representation_rejected();

    if (failures != 0) {
        std::cerr << failures << " canonical ring assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "geographic_canonical: PASS\n";
    return EXIT_SUCCESS;
}
