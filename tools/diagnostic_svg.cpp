// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/geo/wgs84.hpp"
#include "aeris/projection/primitives.hpp"
#include "aeris/projection/subdivide.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

enum class ProjectionKind {
    sinusoidal,
    mollweide,
};

struct CurveContext final {
    ProjectionKind projection = ProjectionKind::sinusoidal;
    bool constant_longitude = false;
    double fixed_rad = 0.0;
    double variable_start_rad = 0.0;
    double variable_end_rad = 0.0;
    double radius_m = aeris::geo::authalic_radius_m();
};

aeris::projection::PlanarResult sample_curve(const double t, void* const opaque) noexcept {
    const auto* context = static_cast<const CurveContext*>(opaque);
    if (context == nullptr || !std::isfinite(t)) {
        return {{}, aeris::geo::MathError::non_finite_input};
    }

    const double variable =
        context->variable_start_rad + t * (context->variable_end_rad - context->variable_start_rad);
    const double longitude = context->constant_longitude ? context->fixed_rad : variable;
    const double latitude = context->constant_longitude ? variable : context->fixed_rad;

    if (context->projection == ProjectionKind::sinusoidal) {
        return aeris::projection::sinusoidal_forward(longitude, latitude, context->radius_m);
    }
    return aeris::projection::mollweide_forward(longitude, latitude, context->radius_m);
}

struct Panel final {
    double center_x = 0.0;
    double center_y = 0.0;
    double scale = 1.0;
};

void write_polyline(
    std::ofstream& output,
    const std::vector<aeris::projection::PlanarPoint>& points,
    const Panel panel,
    const double radius,
    const std::string& css_class
) {
    output << "  <polyline class=\"" << css_class << "\" points=\"";
    output << std::fixed << std::setprecision(3);
    for (const auto point : points) {
        const double x = panel.center_x + (point.x / radius) * panel.scale;
        const double y = panel.center_y - (point.y / radius) * panel.scale;
        output << x << ',' << y << ' ';
    }
    output << "\"/>\n";
}

bool draw_curve(
    std::ofstream& output,
    CurveContext& context,
    const Panel panel,
    const std::string& css_class
) {
    aeris::projection::SubdivisionOptions options{};
    options.geometric_tolerance = 5'000.0;
    options.area_tolerance = 25'000'000.0;
    options.max_depth = 24U;
    options.max_segments = 65'536U;

    const auto curve = aeris::projection::subdivide_projected_curve(sample_curve, &context, options);
    if (!curve.ok()) {
        std::cerr << "curve subdivision failed with error " << static_cast<int>(curve.error) << '\n';
        return false;
    }

    write_polyline(output, curve.points, panel, context.radius_m, css_class);
    return true;
}

bool draw_graticule(
    std::ofstream& output,
    const ProjectionKind projection,
    const Panel panel,
    const double radius
) {
    constexpr double deg = aeris::geo::kPi / 180.0;

    for (int longitude_deg = -150; longitude_deg <= 150; longitude_deg += 30) {
        CurveContext context{};
        context.projection = projection;
        context.constant_longitude = true;
        context.fixed_rad = static_cast<double>(longitude_deg) * deg;
        context.variable_start_rad = -89.5 * deg;
        context.variable_end_rad = 89.5 * deg;
        context.radius_m = radius;
        if (!draw_curve(output, context, panel, longitude_deg == 0 ? "axis" : "grid")) {
            return false;
        }
    }

    for (int latitude_deg = -60; latitude_deg <= 60; latitude_deg += 30) {
        CurveContext context{};
        context.projection = projection;
        context.constant_longitude = false;
        context.fixed_rad = static_cast<double>(latitude_deg) * deg;
        context.variable_start_rad = -aeris::geo::kPi;
        context.variable_end_rad = aeris::geo::kPi;
        context.radius_m = radius;
        if (!draw_curve(output, context, panel, latitude_deg == 0 ? "axis" : "grid")) {
            return false;
        }
    }

    return true;
}

}  // namespace

int main(const int argc, char** const argv) {
    const std::string output_path = argc > 1 ? argv[1] : "aeris-diagnostic.svg";
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "unable to open output file: " << output_path << '\n';
        return EXIT_FAILURE;
    }

    const double radius = aeris::geo::authalic_radius_m();
    const Panel sinusoidal{400.0, 390.0, 112.0};
    const Panel mollweide{1200.0, 390.0, 112.0};

    output << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1600\" height=\"780\" "
              "viewBox=\"0 0 1600 780\" role=\"img\" "
              "aria-label=\"AERIS diagnostic equal-area projection graticules\">\n";
    output << "<style>\n"
              "  .background{fill:#f7f7f7;}\n"
              "  .grid{fill:none;stroke:#777;stroke-width:1;vector-effect:non-scaling-stroke;}\n"
              "  .axis{fill:none;stroke:#111;stroke-width:1.8;vector-effect:non-scaling-stroke;}\n"
              "  .title{font:24px sans-serif;fill:#111;}\n"
              "  .note{font:15px sans-serif;fill:#555;}\n"
              "</style>\n";
    output << "<rect class=\"background\" width=\"1600\" height=\"780\"/>\n";
    output << "<text class=\"title\" x=\"400\" y=\"48\" text-anchor=\"middle\">Sinusoidal</text>\n";
    output << "<text class=\"title\" x=\"1200\" y=\"48\" text-anchor=\"middle\">Mollweide</text>\n";
    output << "<text class=\"note\" x=\"800\" y=\"752\" text-anchor=\"middle\">"
              "AERIS CPU reference core — WGS84 authalic radius, adaptive projected curves"
              "</text>\n";

    if (!draw_graticule(output, ProjectionKind::sinusoidal, sinusoidal, radius) ||
        !draw_graticule(output, ProjectionKind::mollweide, mollweide, radius)) {
        return EXIT_FAILURE;
    }

    output << "</svg>\n";
    output.flush();
    if (!output) {
        std::cerr << "failed while writing output file: " << output_path << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "wrote " << output_path << '\n';
    return EXIT_SUCCESS;
}
