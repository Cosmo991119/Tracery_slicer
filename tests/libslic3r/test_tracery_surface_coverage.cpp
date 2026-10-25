#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/MultiMaterialSegmentation.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <boost/filesystem.hpp>
#include "libslic3r/Print.hpp"
#include "libslic3r/TraceryDocument.hpp"
#include "libslic3r/TraceryVectorSelection.hpp"
#include <array>
#include <cmath>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace Slic3r;
namespace {
TriangleMesh tiled_box()
{
    // Each top triangle is 0.005 mm^2: below the projection's small-island threshold.
    constexpr int n = 40;
    indexed_triangle_set mesh;
    const auto id = [](int x, int y, int z) { return z * (n + 1) * (n + 1) + y * (n + 1) + x; };
    for (int z = 0; z <= 1; ++z)
        for (int y = 0; y <= n; ++y)
            for (int x = 0; x <= n; ++x)
                mesh.vertices.emplace_back(x * 0.1f, y * 0.1f, z * 1.f);
    const auto quad = [&](int a, int b, int c, int d) {
        mesh.indices.emplace_back(a, b, c);
        mesh.indices.emplace_back(a, c, d);
    };
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            quad(id(x, y, 1), id(x + 1, y, 1), id(x + 1, y + 1, 1), id(x, y + 1, 1));
            quad(id(x, y, 0), id(x, y + 1, 0), id(x + 1, y + 1, 0), id(x + 1, y, 0));
        }
    for (int i = 0; i < n; ++i) {
        quad(id(i, 0, 0), id(i + 1, 0, 0), id(i + 1, 0, 1), id(i, 0, 1));
        quad(id(i, n, 0), id(i, n, 1), id(i + 1, n, 1), id(i + 1, n, 0));
        quad(id(0, i, 0), id(0, i, 1), id(0, i + 1, 1), id(0, i + 1, 0));
        quad(id(n, i, 0), id(n, i + 1, 0), id(n, i + 1, 1), id(n, i, 1));
    }
    return TriangleMesh(mesh);
}

double area_mm2(const ExPolygons& polygons)
{
    double area = 0;
    for (const auto& polygon : polygons)
        area += polygon.area() * SCALING_FACTOR * SCALING_FACTOR;
    return area;
}
void paint_surface(ModelVolume& volume, bool bottom, bool patterned)
{
    const auto& mesh = volume.mesh();
    std::vector<TraceryDocument::Annotation> annotations(8);
    auto document = TraceryDocument::capture(mesh, annotations);
    const float surface_z = bottom ? mesh.bounding_box().min.z() : mesh.bounding_box().max.z();
    TriangleSelector::Tracery2DData paint;
    for (size_t i = 0; i < mesh.its.indices.size(); ++i) {
        const auto& t = mesh.its.indices[i];
        if (mesh.its.vertices[t[0]].z() != surface_z || mesh.its.vertices[t[1]].z() != surface_z ||
            mesh.its.vertices[t[2]].z() != surface_z)
            continue;
        const Vec3f center = (mesh.its.vertices[t[0]] + mesh.its.vertices[t[1]] + mesh.its.vertices[t[2]]) / 3.f;
        // A real unpainted hole and a neighboring color must survive seam repair.
        if (patterned && std::abs(center.x()) < 0.5f && std::abs(center.y()) < 0.5f)
            continue;
        auto facet = TraceryVectorSelection::frame(mesh, int(i));
        TriangleSelector::TraceryRegion2D region;
        // The neighboring strip is only 0.6 mm wide.
        region.color = EnforcerBlockerType(patterned && center.x() > 1.4f ? 3 : 2);
        for (int j : {t[0], t[1], t[2]}) {
            const Vec3d delta = mesh.its.vertices[j].cast<double>() - facet.origin;
            region.polygon.emplace_back(delta.dot(facet.axis_u), delta.dot(facet.axis_v));
        }
        facet.regions.push_back(std::move(region));
        paint.push_back(std::move(facet));
    }
    REQUIRE_FALSE(paint.empty());
    document.apply_vector_edit(mesh, annotations, paint, TraceryEdit::Paint);
    volume.tracery_edit_data = document.encode();
}

ExPolygons rectangle(const Transform3d& transform, double x0, double y0, double x1, double y1)
{
    Polygon polygon;
    for (const Vec3d& p : {Vec3d(x0, y0, 0), Vec3d(x1, y0, 0), Vec3d(x1, y1, 0), Vec3d(x0, y1, 0)}) {
        const Vec3d point = transform * p;
        polygon.points.emplace_back(scale_(point.x()), scale_(point.y()));
    }
    polygon.make_counter_clockwise();
    return {ExPolygon(std::move(polygon))};
}
}

TEST_CASE("Vector surface coverage does not depend on source triangle size", "[TraceryVector][TracerySurfaceCoverage]")
{
    const bool bottom = GENERATE(false, true);
    const bool patterned = GENERATE(false, true);
    const bool transformed = GENERATE(false, true);
    CAPTURE(bottom, patterned, transformed);
    Model model;
    auto* object = model.add_object();
    auto* instance = object->add_instance();
    if (transformed) {
        instance->set_rotation(Vec3d(0, 0, 0.37));
        instance->set_mirror(Vec3d(-1, 1, 1));
    }
    auto* volume = object->add_volume(tiled_box());
    paint_surface(*volume, bottom, patterned);
    DynamicPrintConfig config;
    config.apply(FullPrintConfig::defaults());
    config.set_key_value("filament_diameter", new ConfigOptionFloats{1.75, 1.75, 1.75});
    config.set_key_value("filament_colour", new ConfigOptionStrings{"#FFFFFF", "#FF0000", "#00FF00"});
    config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));
    config.set_key_value("outer_wall_line_width", new ConfigOptionFloatOrPercent(0.42, false));
    Print print;
    print.set_status_silent();
    print.apply(model, config);
    auto* printed = print.get_object(0);
    printed->slice();
    const Layer* layer = printed->get_layer(bottom ? 0 : int(printed->layer_count()) - 1);
    std::array<ExPolygons, 4> colors;
    ExPolygons all;
    for (const auto* region : layer->regions())
        for (const auto& surface : region->slices.surfaces) {
            all.push_back(surface.expolygon);
            const auto material = region->region().config().wall_filament.value;
            REQUIRE(material >= 1);
            REQUIRE(material <= 3);
            colors[material].push_back(surface.expolygon);
        }
    const auto transform = printed->trafo_centered() * volume->get_matrix();
    // Expected coverage comes from the input geometry, not the possibly broken output.
    CHECK(area_mm2(union_ex(all)) == Catch::Approx(16).margin(0.001));
    auto red = rectangle(transform, -1.5, -1.5, patterned ? 0.7 : 1.5, 1.5);
    if (patterned) {
        red = diff_ex(red, rectangle(transform, -0.8, -0.8, 0.8, 0.8));
        CHECK(area_mm2(diff_ex(rectangle(transform, -0.2, -0.2, 0.2, 0.2), colors[1])) < 0.001);
        CHECK(area_mm2(diff_ex(rectangle(transform, 1.6, -1.5, 1.7, 1.5), colors[3])) < 0.001);
        CHECK(area_mm2(intersection_ex(colors[2], colors[3])) < 0.001);
    }
    CHECK(area_mm2(red) > 4);
    CHECK(area_mm2(diff_ex(red, colors[2])) < 0.001);
}


TEST_CASE("Upright vector color survives nearly coincident triangle intersections", "[TracerySurfaceCoverage]")
{
    // Captured before region construction at Z=63.05 mm in the upright cathedral model.
    // In particular, color 57 contains two vertices separated by just 0.000002 mm.
    std::ifstream file((boost::filesystem::path(__FILE__).parent_path().parent_path() / "data/tracery_side_contour_630.json").string());
    REQUIRE(file.good());
    nlohmann::json data;
    file >> data;
    std::vector<ColoredLines> contours;
    for (const auto& loop : data) {
        ColoredLines lines;
        for (size_t i = 0; i < loop.size(); ++i) {
            const auto& a = loop[i];
            const auto& b = loop[(i + 1) % loop.size()];
            lines.push_back({Line(Point(a[0].get<coord_t>(), a[1].get<coord_t>()),
                                 Point(b[0].get<coord_t>(), b[1].get<coord_t>())),
                             a[2].get<int>(), int(contours.size()), int(i)});
        }
        contours.push_back(std::move(lines));
    }
    const bool transformed = GENERATE(false, true);
    Transform3d transform = Transform3d::Identity();
    if (transformed) {
        transform.rotate(Eigen::AngleAxisd(0.37, Vec3d::UnitZ()));
        transform.scale(Vec3d(-1, 1, 1));
        for (auto& contour : contours) {
            for (auto& line : contour) {
                for (auto* point : {&line.line.a, &line.line.b}) {
                    const Vec3d p = transform * Vec3d(unscale<double>(point->x()), unscale<double>(point->y()), 0);
                    *point = Point(scale_(p.x()), scale_(p.y()));
                }
                std::swap(line.line.a, line.line.b);
            }
            std::reverse(contour.begin(), contour.end());
        }
    }
    const auto regions = segment_colored_contours(contours, 58);
    // The source assigns this entire interior strip to material 57, never white.
    const auto expected = rectangle(transform, 3.2, -2.20, 28.5, -1.8);
    CHECK(area_mm2(diff_ex(expected, regions[57])) < 0.001);
    CHECK(area_mm2(regions[57]) > 50);
}


TEST_CASE("Colored side contours preserve holes and narrow white boundaries", "[TracerySurfaceCoverage]")
{
    const auto loop = [](const Points& points, const std::vector<int>& colors) {
        ColoredLines lines;
        for (size_t i = 0; i < points.size(); ++i)
            lines.push_back({Line(points[i], points[(i + 1) % points.size()]), colors[i]});
        return lines;
    };
    const bool hole = GENERATE(false, true);
    const bool split = GENERATE(false, true);
    // A 0.02 mm white run must not be merged with red, even when surrounded by
    // much longer red edges. Zero-length intersections have no colored area.
    Points outer{{0, 0}, {scale_(4.), 0.0}, {scale_(4.02), 0.0}, {scale_(10.), 0.0},
                 {scale_(10.), scale_(10.)}, {0.0, scale_(10.)}};
    std::vector<int> colors{2, 1, 2, 2, 2, 2};
    if (split) {
        outer.insert(outer.begin() + 1, Point(scale_(2.), 0.0));
        colors.insert(colors.begin(), 2);
        outer.insert(outer.begin() + 2, Point(scale_(2.) + 2, 0.0));
        colors.insert(colors.begin(), 2);
        outer.insert(outer.begin() + 2, outer[2]);
        colors.insert(colors.begin(), 2);
    }
    std::vector<ColoredLines> contours{loop(outer, colors)};
    auto domain = rectangle(Transform3d::Identity(), 0, 0, 10, 10);
    const auto hole_mask = rectangle(Transform3d::Identity(), 3, 3, 7, 7);
    if (hole) {
        Points inner = hole_mask.front().contour.points;
        std::reverse(inner.begin(), inner.end());
        contours.push_back(loop(inner, std::vector<int>(inner.size(), 1)));
        domain = diff_ex(domain, hole_mask);
    }
    const auto regions = segment_colored_contours(contours, 3);
    ExPolygons all;
    for (const auto& region : regions) {
        append(all, region);
        for (const auto& polygon : region)
            CHECK(polygon.area() > 0);
    }
    CHECK(area_mm2(diff_ex(domain, all)) < 0.001);
    CHECK(area_mm2(diff_ex(all, domain)) < 0.001);
    CHECK(area_mm2(intersection_ex(regions[1], regions[2])) < 0.001);
    const auto white = rectangle(Transform3d::Identity(), 4.005, 0.001, 4.015, 0.005);
    CHECK(area_mm2(diff_ex(white, regions[1])) < 0.000001);
    if (hole)
        CHECK(area_mm2(intersection_ex(all, hole_mask)) < 0.001);
}


TEST_CASE("Upright surface colors survive the complete slicing path", "[TracerySurfaceCoverage]")
{
    Model model;
    auto* object = model.add_object();
    object->add_instance()->set_rotation(Vec3d(0.5 * PI, 0, 0));
    auto* volume = object->add_volume(tiled_box());
    paint_surface(*volume, false, true);
    DynamicPrintConfig config;
    config.apply(FullPrintConfig::defaults());
    config.set_key_value("filament_diameter", new ConfigOptionFloats{1.75, 1.75, 1.75});
    config.set_key_value("filament_colour", new ConfigOptionStrings{"#FFFFFF", "#FF0000", "#00FF00"});
    config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));
    config.set_key_value("outer_wall_line_width", new ConfigOptionFloatOrPercent(0.42, false));
    Print print;
    print.set_status_silent();
    print.apply(model, config);
    auto* printed = print.get_object(0);
    printed->slice();
    const auto transform = printed->trafo_centered() * volume->get_matrix();
    const Vec3d face_center = transform * Vec3d(0, 0, volume->mesh().bounding_box().max.z());
    size_t checked = 0;
    for (const auto* layer : printed->layers()) {
        const double height = layer->slice_z - face_center.z();
        // Keep away from top/bottom propagation and the intentionally unpainted hole's ends.
        if (std::abs(height) > 1.4 || (std::abs(height) > 0.2 && std::abs(height) < 0.8))
            continue;
        std::array<ExPolygons, 4> colors;
        for (const auto* region : layer->regions())
            for (const auto& surface : region->slices.surfaces)
                colors.at(region->region().config().wall_filament.value).push_back(surface.expolygon);
        const auto strip = [&](double x0, double x1) {
            return rectangle(Transform3d::Identity(), face_center.x() + x0, face_center.y() + 0.01,
                             face_center.x() + x1, face_center.y() + 0.08);
        };
        CAPTURE(layer->slice_z);
        CHECK(area_mm2(diff_ex(strip(-1.5, -0.9), colors[2])) < 0.001);
        CHECK(area_mm2(diff_ex(strip(1.6, 1.7), colors[3])) < 0.001);
        if (std::abs(height) < 0.2)
            CHECK(area_mm2(diff_ex(strip(-0.2, 0.2), colors[1])) < 0.001);
        ++checked;
    }
    CHECK(checked >= 6);
}
