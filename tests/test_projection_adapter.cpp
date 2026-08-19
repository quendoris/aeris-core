// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/projection/adapter.hpp"
#include "aeris/projection/ring.hpp"

#include "aeris/geo/wgs84.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

[[nodiscard]] double radians(const double degrees) noexcept {
    return degrees * aeris::geo::kPi / 180.0;
}

void expect_true(const std::string_view name, const bool condition) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << name << '\n';
    }
}

[[nodiscard]] double angular_difference(const double a, const double b) noexcept {
    return std::remainder(a - b, 2.0 * aeris::geo::kPi);
}

void test_catalog_contract() {
    const auto adapters = aeris::projection::builtin_projection_adapters();
    expect_true("built-in projection catalog contains three adapters", adapters.size() == 3U);

    std::set<std::string> ids;
    for (const auto* adapter : adapters) {
        expect_true("built-in projection adapter pointer is non-null", adapter != nullptr);
        if (adapter == nullptr) continue;
        const auto descriptor = adapter->descriptor();
        expect_true("projection model ID is non-empty", !descriptor.model_id.empty());
        expect_true("projection display name is non-empty", !descriptor.display_name.empty());
        expect_true(
            "built-in projection is equal-area",
            descriptor.area_contract == aeris::projection::ProjectionAreaContract::equal_area
        );
        expect_true(
            "built-in projection uses the currently verified cut topology",
            descriptor.cut_topology == aeris::projection::ProjectionCutTopology::single_antimeridian &&
            descriptor.cut_model_id == aeris::projection::kProjectionCutSingleAntimeridianV1
        );
        expect_true(
            "projection model IDs are unique",
            ids.insert(std::string(descriptor.model_id)).second
        );
        expect_true(
            "projection catalog lookup returns identical adapter",
            aeris::projection::find_builtin_projection_adapter(descriptor.model_id) == adapter
        );
    }
    expect_true(
        "unknown projection ID is not silently substituted",
        aeris::projection::find_builtin_projection_adapter("aeris.projection.unknown.v1") == nullptr
    );
}

void test_forward_inverse_roundtrip() {
    const std::vector<aeris::geometry::GeodeticPoint> samples{
        {radians(-122.4194), radians(37.7749)},
        {radians(18.4241), radians(-33.9249)},
        {radians(139.6917), radians(35.6895)},
        {radians(-73.9857), radians(40.7484)},
        {radians(45.0), radians(75.0)},
    };
    const double central_meridian = radians(17.0);

    for (const auto* adapter : aeris::projection::builtin_projection_adapters()) {
        for (const auto sample : samples) {
            const auto projected = adapter->forward_wgs84(
                sample.longitude_rad,
                sample.latitude_rad,
                central_meridian
            );
            expect_true("adapter forward projection succeeds", projected.ok());
            if (!projected.ok()) continue;

            const auto inverse = adapter->inverse_wgs84(
                projected.value.x,
                projected.value.y,
                central_meridian
            );
            expect_true("adapter inverse projection succeeds", inverse.ok());
            if (!inverse.ok()) continue;

            expect_true(
                "adapter round-trip longitude is stable",
                std::abs(angular_difference(
                    inverse.value.longitude_rad,
                    sample.longitude_rad
                )) <= 2e-11
            );
            expect_true(
                "adapter round-trip latitude is stable",
                std::abs(inverse.value.latitude_rad - sample.latitude_rad) <= 2e-11
            );
        }
    }
}

void test_lambert_analytic_equator() {
    const auto& adapter =
        aeris::projection::lambert_cylindrical_equal_area_projection_adapter();
    const double radius = aeris::geo::authalic_radius_m();
    const auto projected = adapter.forward_wgs84(1.0, 0.0, 0.0, radius);
    expect_true("Lambert CEA equator projection succeeds", projected.ok());
    if (projected.ok()) {
        expect_true(
            "Lambert CEA x follows equatorial analytic form",
            std::abs(projected.value.x - radius) <= radius * 1e-14
        );
        expect_true(
            "Lambert CEA equator has zero y",
            projected.value.y == 0.0
        );
    }
}

[[nodiscard]] aeris::geometry::LinearRing make_verified_ring() {
    const std::vector<aeris::geometry::GeodeticPoint> points{
        {radians(-35.0), radians(-20.0)},
        {radians(25.0), radians(-12.0)},
        {radians(40.0), radians(28.0)},
        {radians(-15.0), radians(42.0)},
    };
    return aeris::geometry::canonicalize_wgs84_linear_ring(points).value;
}

void test_all_adapters_pass_verified_area_contract() {
    const auto ring = make_verified_ring();
    expect_true("verified adapter area fixture is non-empty", !ring.vertices.empty());
    if (ring.vertices.empty()) return;

    for (const auto* adapter : aeris::projection::builtin_projection_adapters()) {
        aeris::projection::RingProjectionOptions options{};
        options.adapter = adapter;
        options.relative_area_tolerance = 1e-9;
        options.absolute_area_tolerance_m2 = 1.0;
        options.initial_geometric_tolerance_m = 8.0;
        options.initial_local_area_tolerance_m2 = 1024.0;
        options.max_refinement_rounds = 18U;

        const auto projected =
            aeris::projection::project_wgs84_linear_ring_verified(ring, options);
        expect_true("built-in adapter passes verified WGS84 area contract", projected.ok());
        if (projected.ok()) {
            expect_true(
                "built-in adapter satisfies published area budget",
                projected.absolute_area_error_m2 <= projected.allowed_area_error_m2
            );
        }
    }
}

void test_all_adapters_share_verified_seam_contract() {
    auto canonical = aeris::geometry::canonicalize_wgs84_linear_ring({
        {radians(170.0), radians(-20.0)},
        {radians(-170.0), radians(-20.0)},
        {radians(-170.0), radians(20.0)},
        {radians(170.0), radians(20.0)},
    });
    expect_true("seam adapter fixture canonicalizes", canonical.ok());
    if (!canonical.ok()) return;
    canonical.value.interior_side = aeris::geometry::RingInteriorSide::left;

    for (const auto* adapter : aeris::projection::builtin_projection_adapters()) {
        aeris::projection::RingProjectionOptions options{};
        options.adapter = adapter;
        options.relative_area_tolerance = 1e-7;
        options.absolute_area_tolerance_m2 = 10'000.0;
        options.initial_geometric_tolerance_m = 1000.0;
        options.initial_local_area_tolerance_m2 = 1e8;
        options.max_refinement_rounds = 18U;
        options.max_projection_pieces = 16U;

        const auto projected =
            aeris::projection::project_wgs84_linear_ring_piecewise_verified(
                canonical.value,
                options
            );
        expect_true("built-in adapter projects seam-crossing ring", projected.ok());
        if (projected.ok()) {
            expect_true("seam-crossing ring becomes two pieces", projected.projected_pieces == 2U);
            expect_true("seam-crossing ring reports two crossings", projected.seam_crossings == 2U);
            expect_true(
                "piecewise adapter result satisfies global area budget",
                projected.absolute_area_error_m2 <= projected.allowed_area_error_m2
            );
        }
    }
}

class WrongCutAdapter final : public aeris::projection::ProjectionAdapter {
public:
    [[nodiscard]] aeris::projection::ProjectionDescriptor descriptor() const noexcept override {
        return {
            "test.projection.wrong-cut.v1",
            "Wrong cut",
            "test.cut.unsupported.v1",
            aeris::projection::ProjectionAreaContract::equal_area,
            aeris::projection::ProjectionCutTopology::single_antimeridian,
        };
    }

    [[nodiscard]] aeris::projection::PlanarResult forward_wgs84(
        double,
        double,
        double,
        double
    ) const noexcept override {
        return {};
    }

    [[nodiscard]] aeris::projection::GeodeticResult inverse_wgs84(
        double,
        double,
        double,
        double
    ) const noexcept override {
        return {};
    }
};

void test_incompatible_adapter_fails_closed() {
    const auto ring = make_verified_ring();
    WrongCutAdapter invalid{};
    aeris::projection::RingProjectionOptions options{};
    options.adapter = &invalid;
    const auto projected =
        aeris::projection::project_wgs84_linear_ring_verified(ring, options);
    expect_true(
        "adapter with unsupported cut identity fails at verifier boundary",
        projected.error == aeris::projection::RingProjectionError::invalid_options
    );
}

}  // namespace

int main() {
    test_catalog_contract();
    test_forward_inverse_roundtrip();
    test_lambert_analytic_equator();
    test_all_adapters_pass_verified_area_contract();
    test_all_adapters_share_verified_seam_contract();
    test_incompatible_adapter_fails_closed();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "projection_adapter: PASS\n";
    return EXIT_SUCCESS;
}
