// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/geo/rotation.hpp"
#include "aeris/geo/wgs84.hpp"
#include "aeris/projection/primitives.hpp"
#include "aeris/projection/subdivide.hpp"
#include "aeris/view/globe.hpp"

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

struct GlobeScreenPoint final {
    double x = 0.0;
    double y = 0.0;
    double depth = 0.0;
    bool visible = false;
};

GlobeScreenPoint globe_screen(
    const double longitude,
    const double latitude,
    const aeris::geo::Mat3& camera,
    const Panel panel,
    const double radius
) {
    const auto projected = aeris::view::orthographic_globe_point(
        longitude,
        latitude,
        camera,
        radius
    );
    if (!projected.ok()) {
        return {};
    }

    return {
        panel.center_x + (projected.value.x / radius) * panel.scale,
        panel.center_y - (projected.value.y / radius) * panel.scale,
        projected.value.depth,
        projected.value.visible,
    };
}

void write_globe_curve(
    std::ofstream& output,
    const bool constant_longitude,
    const double fixed_rad,
    const aeris::geo::Mat3& camera,
    const Panel panel,
    const double radius,
    const std::string& css_class
) {
    constexpr int samples = 720;
    constexpr double deg = aeris::geo::kPi / 180.0;
    const double variable_start = constant_longitude ? -89.5 * deg : -aeris::geo::kPi;
    const double variable_end = constant_longitude ? 89.5 * deg : aeris::geo::kPi;

    bool path_open = false;
    GlobeScreenPoint previous{};
    bool have_previous = false;

    output << std::fixed << std::setprecision(3);
    for (int index = 0; index <= samples; ++index) {
        const double t = static_cast<double>(index) / static_cast<double>(samples);
        const double variable = variable_start + t * (variable_end - variable_start);
        const double longitude = constant_longitude ? fixed_rad : variable;
        const double latitude = constant_longitude ? variable : fixed_rad;
        const GlobeScreenPoint current = globe_screen(longitude, latitude, camera, panel, radius);

        if (have_previous && previous.visible != current.visible) {
            const double denominator = previous.depth - current.depth;
            if (denominator != 0.0) {
                const double alpha = previous.depth / denominator;
                const double boundary_x = previous.x + alpha * (current.x - previous.x);
                const double boundary_y = previous.y + alpha * (current.y - previous.y);
                if (previous.visible && path_open) {
                    output << 'L' << boundary_x << ',' << boundary_y << ' ';
                    path_open = false;
                } else if (current.visible) {
                    output << "<path class=\"" << css_class << "\" d=\"M"
                           << boundary_x << ',' << boundary_y << ' ';
                    path_open = true;
                }
            }
        }

        if (current.visible) {
            if (!path_open) {
                output << "<path class=\"" << css_class << "\" d=\"M"
                       << current.x << ',' << current.y << ' ';
                path_open = true;
            } else {
                output << 'L' << current.x << ',' << current.y << ' ';
            }
        }

        if (path_open && (!current.visible || index == samples)) {
            output << "\"/>\n";
            path_open = false;
        }

        previous = current;
        have_previous = true;
    }
}

void draw_globe(
    std::ofstream& output,
    const Panel panel,
    const double radius
) {
    constexpr double deg = aeris::geo::kPi / 180.0;
    const auto camera = aeris::geo::multiply(
        aeris::geo::rotation_y(-18.0 * deg),
        aeris::geo::rotation_z(28.0 * deg)
    );

    output << "<circle class=\"outline\" cx=\"" << panel.center_x
           << "\" cy=\"" << panel.center_y
           << "\" r=\"" << panel.scale << "\"/>\n";

    for (int longitude_deg = -150; longitude_deg <= 180; longitude_deg += 30) {
        write_globe_curve(
            output,
            true,
            static_cast<double>(longitude_deg) * deg,
            camera,
            panel,
            radius,
            longitude_deg == 0 ? "axis" : "grid"
        );
    }
    for (int latitude_deg = -60; latitude_deg <= 60; latitude_deg += 30) {
        write_globe_curve(
            output,
            false,
            static_cast<double>(latitude_deg) * deg,
            camera,
            panel,
            radius,
            latitude_deg == 0 ? "axis" : "grid"
        );
    }
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
    const Panel globe{300.0, 350.0, 220.0};
    const Panel sinusoidal{900.0, 350.0, 95.0};
    const Panel mollweide{1500.0, 350.0, 95.0};

    output << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1800\" height=\"700\" "
              "viewBox=\"0 0 1800 700\" role=\"img\" "
              "aria-label=\"AERIS diagnostic globe and equal-area projection graticules\">\n";
    output << "<style>\n"
              "  .background{fill:#f7f7f7;}\n"
              "  .grid{fill:none;stroke:#777;stroke-width:1;vector-effect:non-scaling-stroke;}\n"
              "  .axis{fill:none;stroke:#111;stroke-width:1.8;vector-effect:non-scaling-stroke;}\n"
              "  .outline{fill:none;stroke:#111;stroke-width:2.2;}\n"
              "  .title{font:24px sans-serif;fill:#111;}\n"
              "  .note{font:15px sans-serif;fill:#555;}\n"
              "</style>\n";
    output << "<rect class=\"background\" width=\"1800\" height=\"700\"/>\n";
    output << "<text class=\"title\" x=\"300\" y=\"48\" text-anchor=\"middle\">Authalic globe</text>\n";
    output << "<text class=\"title\" x=\"900\" y=\"48\" text-anchor=\"middle\">Sinusoidal</text>\n";
    output << "<text class=\"title\" x=\"1500\" y=\"48\" text-anchor=\"middle\">Mollweide</text>\n";
    output << "<text class=\"note\" x=\"900\" y=\"675\" text-anchor=\"middle\">"
              "AERIS CPU reference core — one authalic sphere, multiple verified representations"
              "</text>\n";

    draw_globe(output, globe, radius);
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
