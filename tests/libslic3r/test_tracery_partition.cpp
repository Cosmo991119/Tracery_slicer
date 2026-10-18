#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "libslic3r/TraceryPartition.hpp"
#include "libslic3r/TraceryMaterialPlan.hpp"
#include "libslic3r/TraceryRestore.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include <boost/filesystem.hpp>
#include <map>
#include <sstream>
#include <cereal/archives/binary.hpp>
#include <cereal/types/map.hpp>
#include <cereal/types/vector.hpp>

using namespace Slic3r;

TEST_CASE("Erasing tracery restores the exact original mesh and annotations", "[TraceryPartition]")
{
    TriangleMesh     source(its_make_cube(10., 10., 1.));
    TriangleSelector paint(source), support(source);
    paint.set_facet(0, EnforcerBlockerType(2));
    support.set_facet(1, EnforcerBlockerType::ENFORCER);
    const std::vector<TriangleSelector::TriangleSplittingData> annotations{paint.serialize(), support.serialize()};
    auto                                                       state = capture_tracery_restore_state(source, annotations);
    // Use a subdivided annotation as the same mesh-changing partition operation
    // used by a flower stroke, then verify that erase undoes its topology too.
    TriangleSelector::TraceryFacet2DData facet;
    const auto&                          triangle = source.its.indices.front();
    facet.origin                                  = source.its.vertices[triangle[0]].cast<double>();
    const Vec3d b                                 = source.its.vertices[triangle[1]].cast<double>() - facet.origin;
    const Vec3d c                                 = source.its.vertices[triangle[2]].cast<double>() - facet.origin;
    facet.axis_u                                  = b.normalized();
    facet.axis_v                                  = b.cross(c).normalized().cross(facet.axis_u);
    facet.source_facet_id                         = 0;
    TriangleSelector::TraceryRegion2D region;
    region.color   = EnforcerBlockerType(120);
    region.polygon = {Vec2d::Zero(), Vec2d(b.norm() * .37, 0.), Vec2d(c.dot(facet.axis_u) * .37, c.dot(facet.axis_v) * .37)};
    facet.regions.push_back(region);
    const auto partition = build_exact_tracery_partition(source, annotations, {facet});
    REQUIRE(partition.mesh.indices.size() > source.its.indices.size());
    advance_tracery_restore_state(state, source, annotations, partition);
    const auto restored = restore_tracery_mesh(state, TriangleMesh(partition.mesh), partition.annotations);
    CHECK(restored.mesh.indices == source.its.indices);
    CHECK(restored.mesh.vertices == source.its.vertices);
    CHECK(restored.annotations == annotations);
}

namespace {
TriangleMesh square_mesh()
{
    indexed_triangle_set mesh;
    mesh.vertices = {Vec3f(0, 0, 0), Vec3f(10, 0, 0), Vec3f(10, 10, 0), Vec3f(0, 10, 0)};
    mesh.indices  = {Vec3i32(0, 1, 2), Vec3i32(0, 2, 3)};
    return TriangleMesh(mesh);
}

TriangleSelector::Tracery2DData straight_partition()
{
    TriangleSelector::Tracery2DData data;
    for (int facet = 0; facet < 2; ++facet) {
        TriangleSelector::TraceryFacet2DData part;
        part.source_facet_id = facet;
        TriangleSelector::TraceryRegion2D region;
        region.polygon = {Vec2d(0, 0), Vec2d(3.17, 0), Vec2d(6.17, 10), Vec2d(0, 10)};
        region.color   = EnforcerBlockerType(120);
        part.regions.push_back(region);
        data.push_back(part);
    }
    return data;
}

Polygons painted_xy(const TriangleSelector& selector)
{
    Polygons triangles;
    for (const auto& leaf : selector.painted_leaves()) {
        Polygon polygon;
        for (const Vec3f& p : leaf.vertices)
            polygon.points.emplace_back(scale_(p.x()), scale_(p.y()));
        polygon.make_counter_clockwise();
        triangles.push_back(std::move(polygon));
    }
    return union_(triangles);
}

TriangleSelector::Tracery2DData inset_facet_partition(const TriangleMesh& mesh)
{
    size_t facet        = 0;
    double largest_area = 0.;
    for (size_t i = 0; i < mesh.its.indices.size(); ++i) {
        const auto&  t = mesh.its.indices[i];
        const double area =
            (mesh.its.vertices[t[1]] - mesh.its.vertices[t[0]]).cross(mesh.its.vertices[t[2]] - mesh.its.vertices[t[0]]).norm();
        if (area > largest_area) {
            largest_area = area;
            facet        = i;
        }
    }
    const auto&                          t = mesh.its.indices[facet];
    const Vec3d                          a = mesh.its.vertices[t[0]].cast<double>();
    const Vec3d                          b = mesh.its.vertices[t[1]].cast<double>() - a;
    const Vec3d                          c = mesh.its.vertices[t[2]].cast<double>() - a;
    TriangleSelector::TraceryFacet2DData data;
    data.source_facet_id = int(facet);
    data.origin          = a;
    data.axis_u          = b.normalized();
    data.axis_v          = b.cross(c).normalized().cross(data.axis_u);
    TriangleSelector::TraceryRegion2D region;
    region.color   = EnforcerBlockerType(2);
    region.polygon = {Vec2d::Zero(), Vec2d(b.norm() * .371, 0.), Vec2d(c.dot(data.axis_u) * .371, c.dot(data.axis_v) * .371)};
    data.regions.push_back(region);
    return {data};
}

double precise_volume(const indexed_triangle_set& mesh)
{
    double volume = 0.;
    for (const auto& t : mesh.indices) {
        const Vec3d a = mesh.vertices[t[0]].cast<double>();
        const Vec3d b = mesh.vertices[t[1]].cast<double>();
        const Vec3d c = mesh.vertices[t[2]].cast<double>();
        volume += a.dot(b.cross(c)) / 6.;
    }
    return volume;
}
} // namespace

TEST_CASE("Replacing an old paint colour frees its head while preserving the black border", "[TraceryPartition][TraceryBudget]")
{
    const TriangleMesh source = square_mesh();
    TriangleSelector paint(source);
    paint.set_facet(0, EnforcerBlockerType(5)); // old target colour, not a CMY ingredient
    paint.set_facet(1, EnforcerBlockerType(4)); // retained black border / base material
    const auto original = paint.serialize();
    REQUIRE_FALSE(tracery_fits_four_heads({4, 5}, {1, 2, 3}));

    TriangleSelector::TraceryFacet2DData facet;
    facet.source_facet_id = 0;
    TriangleSelector::TraceryRegion2D region;
    region.polygon = {Vec2d(0, 0), Vec2d(10, 0), Vec2d(10, 10)};
    facet.regions.push_back(region);
    const auto retained = tracery_retained_paint_materials(source, original, {facet});
    REQUIRE(retained == std::vector<unsigned int>{4});
    REQUIRE(tracery_fits_four_heads(retained, {1, 2, 3}));
    REQUIRE(paint.serialize() == original);
}

TEST_CASE("Exact cut sharing does not join coincident disconnected shells", "[TraceryPartition]")
{
    indexed_triangle_set mesh = square_mesh().its;
    const auto original = mesh;
    mesh.vertices.insert(mesh.vertices.end(), original.vertices.begin(), original.vertices.end());
    for (const auto& t : original.indices)
        mesh.indices.emplace_back(t[0] + 4, t[1] + 4, t[2] + 4);
    auto regions = straight_partition();
    const auto first_regions = regions;
    for (auto facet : first_regions) {
        facet.source_facet_id += 2;
        regions.push_back(std::move(facet));
    }
    const auto result = build_exact_tracery_partition(TriangleMesh(mesh), {TriangleSelector::TriangleSplittingData{}}, regions);
    std::map<int, int> vertex_shells;
    for (size_t i = 0; i < result.mesh.indices.size(); ++i) {
        const int shell = result.source_facets[i] / 2;
        const auto& t = result.mesh.indices[i];
        for (int e = 0; e < 3; ++e) {
            const auto entry = vertex_shells.emplace(t[e], shell);
            CHECK(entry.first->second == shell);
        }
    }
    CHECK(result.mesh.indices.size() > mesh.indices.size());
}

TEST_CASE("Partially covered and unselected paint still reserves its physical materials", "[TraceryPartition][TraceryBudget]")
{
    const TriangleMesh source = square_mesh();
    TriangleSelector paint(source);
    paint.set_facet(0, EnforcerBlockerType(5));
    paint.set_facet(1, EnforcerBlockerType(4));
    const auto retained = tracery_retained_paint_materials(source, paint.serialize(), straight_partition());
    REQUIRE(retained == std::vector<unsigned int>{4, 5});
    REQUIRE_FALSE(tracery_fits_four_heads(retained, {1, 2, 3}));

    // A later stroke must retain older recipes beyond its own footprint.
    paint.set_facet(1, EnforcerBlockerType(120));
    const auto incremental = tracery_retained_paint_materials(source, paint.serialize(), straight_partition());
    REQUIRE(incremental == std::vector<unsigned int>{5, 120});
    REQUIRE(tracery_retained_paint_materials(source, paint.serialize(), {}) == incremental);
}

TEST_CASE("Exact tracery keeps an oblique material boundary straight after paint serialization", "[TraceryPartition]")
{
    const TriangleMesh source = square_mesh();
    TriangleSelector   base(source);
    base.set_facet(0, EnforcerBlockerType(2));
    base.set_facet(1, EnforcerBlockerType(2));
    const auto       result = build_exact_tracery_partition(source, {base.serialize()}, straight_partition());
    TriangleMesh     output(result.mesh);
    TriangleSelector restored(output);
    restored.deserialize(result.annotations.front(), false);
    std::map<std::pair<int, int>, std::vector<int>> edges;
    for (size_t i = 0; i < result.mesh.indices.size(); ++i) {
        const auto& triangle = result.mesh.indices[i];
        const Vec3f center   = (result.mesh.vertices[triangle[0]] + result.mesh.vertices[triangle[1]] + result.mesh.vertices[triangle[2]]) /
                               3.f;
        const int   state    = int(restored.state_at(center, int(i)));
        REQUIRE(state == (center.x() < 3.17f + 0.3f * center.y() ? 120 : 2));
        for (int edge = 0; edge < 3; ++edge)
            edges[std::minmax(triangle[edge], triangle[(edge + 1) % 3])].push_back(state);
    }
    size_t interfaces = 0;
    for (const auto& [edge, states] : edges) {
        if (states.size() != 2 || states[0] == states[1])
            continue;
        ++interfaces;
        for (int index : {edge.first, edge.second}) {
            const Vec3f& point = result.mesh.vertices[index];
            CHECK(std::abs(point.x() - 3.17f - 0.3f * point.y()) < 0.00002f);
        }
    }
    CHECK(interfaces >= 2);
}

TEST_CASE("Exact tracery retains a closed solid when only its top is painted", "[TraceryPartition]")
{
    TriangleMesh     source = make_cube(10, 10, 1);
    TriangleSelector base(source);
    auto             region = straight_partition().front();
    region.origin.z()       = 1.;
    TriangleSelector::Tracery2DData data;
    for (size_t i = 0; i < source.its.indices.size(); ++i) {
        const auto& t = source.its.indices[i];
        if (source.its.vertices[t[0]].z() == 1.f && source.its.vertices[t[1]].z() == 1.f && source.its.vertices[t[2]].z() == 1.f) {
            region.source_facet_id = int(i);
            data.push_back(region);
        }
    }
    REQUIRE(data.size() == 2);
    const auto                         result = build_exact_tracery_partition(source, {base.serialize()}, data);
    std::map<std::pair<int, int>, int> edge_counts;
    for (const Vec3i32& t : result.mesh.indices)
        for (int i = 0; i < 3; ++i)
            ++edge_counts[std::minmax(t[i], t[(i + 1) % 3])];
    for (const auto& [edge, count] : edge_counts)
        CHECK(count == 2);
    TriangleMesh mesh(result.mesh);
    CHECK(std::abs(mesh.volume() - source.volume()) < 0.001);
}

TEST_CASE("Exact tracery preserves subdivided annotations and accepts a subsequent stroke", "[TraceryPartition]")
{
    const TriangleMesh source = square_mesh();
    TriangleSelector   base(source), support(source);
    auto               support_data = straight_partition();
    for (auto& facet : support_data)
        facet.regions.front().color = EnforcerBlockerType::ENFORCER;
    support.paint_all_tracery_regions(support_data, 0.5f);
    auto             result = build_exact_tracery_partition(source, {base.serialize(), support.serialize()}, straight_partition());
    TriangleMesh     mesh(result.mesh);
    TriangleSelector restored(mesh);
    restored.deserialize(result.annotations[1], false);
    const Polygons before = painted_xy(support), after = painted_xy(restored);
    CHECK(std::abs(area(diff_ex(before, after))) * SCALING_FACTOR * SCALING_FACTOR < 0.0001);
    CHECK(std::abs(area(diff_ex(after, before))) * SCALING_FACTOR * SCALING_FACTOR < 0.0001);
    TriangleSelector::Tracery2DData second_stroke;
    for (size_t i = 0; i < mesh.its.indices.size(); ++i) {
        auto region                    = straight_partition().front();
        region.source_facet_id         = int(i);
        region.regions.front().polygon = {Vec2d(2, 2), Vec2d(8, 2), Vec2d(8, 8), Vec2d(2, 8)};
        region.regions.front().color   = EnforcerBlockerType(3);
        second_stroke.push_back(region);
    }
    const auto second = build_exact_tracery_partition(mesh, result.annotations, second_stroke);
    REQUIRE(second.annotations.size() == 2);
    CHECK(second.annotations[0].used_states[3]);
    CHECK(second.annotations[0].used_states[120]);
    CHECK(second.annotations[1].used_states[1]);
}

TEST_CASE("Exact tracery paint survives a 3MF save and reload", "[TraceryPartition]")
{
    const TriangleMesh source = square_mesh();
    TriangleSelector   base(source);
    const auto         result = build_exact_tracery_partition(source, {base.serialize()}, straight_partition());
    Model              model;
    ModelObject*       object = model.add_object();
    ModelVolume*       volume = object->add_volume(TriangleMesh(result.mesh));
    object->add_instance();
    TriangleSelector paint(volume->mesh());
    paint.deserialize(result.annotations[0], false);
    volume->mmu_segmentation_facets.set(paint);
    const auto path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("tracery-%%%%-%%%%.3mf");
    struct Cleanup
    {
        boost::filesystem::path path;
        ~Cleanup() { boost::filesystem::remove(path); }
    } cleanup{path};
    REQUIRE(store_3mf(path.string().c_str(), &model, nullptr, false));
    Model                     loaded;
    DynamicPrintConfig        config;
    ConfigSubstitutionContext context{ForwardCompatibilitySubstitutionRule::Disable};
    REQUIRE(load_3mf(path.string().c_str(), config, context, &loaded, false));
    REQUIRE(loaded.objects.size() == 1);
    const ModelVolume& restored = *loaded.objects.front()->volumes.front();
    CHECK(restored.mesh().its.indices.size() == result.mesh.indices.size());
    CHECK(restored.mmu_segmentation_facets.get_data().used_states[120]);
}

TEST_CASE("Exact tracery remains straight through the normal painted-region slicer", "[TraceryPartition]")
{
    const TriangleMesh              source = make_cube(10, 10, 1);
    TriangleSelector                base(source);
    TriangleSelector::Tracery2DData data;
    for (size_t i = 0; i < source.its.indices.size(); ++i) {
        const auto& t = source.its.indices[i];
        if (source.its.vertices[t[0]].z() != 1.f || source.its.vertices[t[1]].z() != 1.f || source.its.vertices[t[2]].z() != 1.f)
            continue;
        auto region                  = straight_partition().front();
        region.source_facet_id       = int(i);
        region.origin.z()            = 1.;
        region.regions.front().color = EnforcerBlockerType(2);
        data.push_back(region);
    }
    const auto   result = build_exact_tracery_partition(source, {base.serialize()}, data);
    Model        model;
    ModelObject* object = model.add_object();
    ModelVolume* volume = object->add_volume(TriangleMesh(result.mesh));
    object->add_instance();
    TriangleSelector paint(volume->mesh());
    paint.deserialize(result.annotations[0], false);
    volume->mmu_segmentation_facets.set(paint);
    DynamicPrintConfig config;
    config.apply(FullPrintConfig::defaults());
    config.set_key_value("filament_diameter", new ConfigOptionFloats{1.75, 1.75});
    config.set_key_value("filament_colour", new ConfigOptionStrings{"#FFFFFF", "#FF0000"});
    config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    config.set_key_value("outer_wall_line_width", new ConfigOptionFloatOrPercent(0.42, false));
    config.set_key_value("inner_wall_line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));
    Print print;
    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);
    PrintObject& printed = *print.get_object(0);
    printed.slice();
    REQUIRE(printed.layer_count() > 0);
    const Layer*       top             = printed.get_layer(int(printed.layer_count()) - 1);
    const Transform3d  inverse         = (printed.trafo_centered() * volume->get_matrix()).inverse();
    size_t             boundary_points = 0;
    std::ostringstream diagnostic;
    for (const LayerRegion* region : top->regions()) {
        diagnostic << " material=" << region->region().config().wall_filament.value << " surfaces=" << region->slices.surfaces.size();
        if (region->region().config().wall_filament.value != 2)
            continue;
        for (const Surface& surface : region->slices.surfaces) {
            const Points& points = surface.expolygon.contour.points;
            for (size_t edge = 0; edge < points.size(); ++edge) {
                const Point p     = (points[edge] + points[(edge + 1) % points.size()]) / 2;
                const Vec3d local = inverse * Vec3d(unscale<double>(p.x()), unscale<double>(p.y()), top->print_z) +
                                    volume->source.mesh_offset;
                diagnostic << " midpoint=" << local.transpose();
                if (local.x() < 1. || local.x() > 9. || local.y() < 1. || local.y() > 9.)
                    continue;
                ++boundary_points;
                CHECK(std::abs(local.x() - 3.17 - 0.3 * local.y()) < 0.02);
            }
        }
    }
    INFO(diagnostic.str());
    CHECK(boundary_points > 0);
}

TEST_CASE("Three tracery colors share one junction without background slivers", "[TraceryPartition]")
{
    const TriangleMesh                    source = square_mesh();
    TriangleSelector                      base(source);
    auto                                  data     = straight_partition();
    const std::vector<std::vector<Vec2d>> polygons = {{Vec2d(0, 0), Vec2d(4.2, 0), Vec2d(4.2, 10), Vec2d(0, 10)},
                                                      {Vec2d(4.2, 0), Vec2d(10, 0), Vec2d(10, 5), Vec2d(4.2, 5)},
                                                      {Vec2d(4.2, 5), Vec2d(10, 5), Vec2d(10, 10), Vec2d(4.2, 10)}};
    for (auto& facet : data) {
        facet.regions.clear();
        for (size_t i = 0; i < polygons.size(); ++i) {
            TriangleSelector::TraceryRegion2D region;
            region.polygon     = polygons[i];
            region.fragment_id = i;
            region.color       = EnforcerBlockerType(i + 1);
            facet.regions.push_back(region);
        }
    }
    const auto       result = build_exact_tracery_partition(source, {base.serialize()}, data);
    TriangleMesh     mesh(result.mesh);
    TriangleSelector paint(mesh);
    paint.deserialize(result.annotations[0], false);
    CHECK_FALSE(paint.has_facets(EnforcerBlockerType::NONE));
    size_t junctions = 0;
    for (const Vec3f& vertex : result.mesh.vertices)
        if ((vertex - Vec3f(4.2f, 5.f, 0.f)).norm() < 0.00002f)
            ++junctions;
    CHECK(junctions == 1);
    CHECK(paint.has_facets(EnforcerBlockerType(1)));
    CHECK(paint.has_facets(EnforcerBlockerType(2)));
    CHECK(paint.has_facets(EnforcerBlockerType(3)));
}

TEST_CASE("Exact tracery follows a tilted translated surface", "[TraceryPartition]")
{
    TriangleMesh          source   = square_mesh();
    const Eigen::Matrix3d rotation = Eigen::AngleAxisd(0.37, Vec3d(1, 2, 3).normalized()).toRotationMatrix();
    const Vec3d           offset(12, 24, 36);
    for (Vec3f& vertex : source.its.vertices)
        vertex = (rotation * vertex.cast<double>() + offset).cast<float>();
    auto data = straight_partition();
    for (auto& facet : data) {
        facet.origin = offset;
        facet.axis_u = rotation.col(0);
        facet.axis_v = rotation.col(1);
    }
    TriangleSelector base(source);
    const auto       result = build_exact_tracery_partition(source, {base.serialize()}, data);
    for (const Vec3f& vertex : result.mesh.vertices) {
        const Vec3d original = rotation.transpose() * (vertex.cast<double>() - offset);
        CHECK(std::abs(original.z()) < 0.00002);
    }
    CHECK(result.annotations.front().used_states[120]);
}

TEST_CASE("Exact tracery cancellation leaves the source mesh unchanged", "[TraceryPartition]")
{
    const TriangleMesh source = square_mesh();
    TriangleSelector   base(source);
    int                calls = 0;
    REQUIRE_THROWS_AS(build_exact_tracery_partition(source, {base.serialize()}, straight_partition(),
                                                    [&] {
                                                        if (++calls == 3)
                                                            throw std::runtime_error("cancelled");
                                                    }),
                      std::runtime_error);
    CHECK(source.its.indices.size() == 2);
    CHECK(source.its.vertices.size() == 4);
    CHECK(base.serialize().triangles_to_split.empty());
}

TEST_CASE("Erasing tracery keeps later paint and support edits across another stroke", "[TraceryPartition]")
{
    const TriangleMesh source = square_mesh();
    TriangleSelector   paint(source), support(source);
    for (int i = 0; i < 2; ++i) {
        paint.set_facet(i, EnforcerBlockerType(2));
        support.set_facet(i, EnforcerBlockerType::ENFORCER);
    }
    const std::vector<TriangleSelector::TriangleSplittingData> original{paint.serialize(), support.serialize()};
    auto                                                       snapshot = capture_tracery_restore_state(source, original);
    const auto first = build_exact_tracery_partition(source, original, straight_partition());
    advance_tracery_restore_state(snapshot, source, original, first);
    TriangleMesh     current(first.mesh);
    TriangleSelector edited_paint(current), edited_support(current);
    edited_paint.deserialize(first.annotations[0], false);
    edited_support.deserialize(first.annotations[1], false);
    Vec3f erased_point = Vec3f::Zero();
    for (size_t i = 0; i < first.mesh.indices.size(); ++i) {
        const auto& t      = first.mesh.indices[i];
        const Vec3f center = (first.mesh.vertices[t[0]] + first.mesh.vertices[t[1]] + first.mesh.vertices[t[2]]) / 3.f;
        if (first.source_facets[i] == 1) {
            edited_paint.set_facet(int(i), EnforcerBlockerType(3));
            edited_support.set_facet(int(i), EnforcerBlockerType::BLOCKER);
        } else if (center.x() < 3.17f + .3f * center.y()) {
            edited_paint.set_facet(int(i), EnforcerBlockerType::NONE);
            erased_point = center;
        }
    }
    const std::vector<TriangleSelector::TriangleSplittingData> edited{edited_paint.serialize(), edited_support.serialize()};
    const auto                                                 second = build_exact_tracery_partition(current, edited, {});
    advance_tracery_restore_state(snapshot, current, edited, second);
    const auto restored = restore_tracery_mesh(snapshot, TriangleMesh(second.mesh), second.annotations);
    CHECK(restored.mesh.indices == source.its.indices);
    paint.deserialize(restored.annotations[0], false);
    support.deserialize(restored.annotations[1], false);
    CHECK(paint.state_at(erased_point, 0) == EnforcerBlockerType::NONE);
    CHECK(paint.state_at(Vec3f(9, 1, 0), 0) == EnforcerBlockerType(2));
    CHECK(paint.state_at(Vec3f(1, 8, 0), 1) == EnforcerBlockerType(3));
    CHECK(support.state_at(Vec3f(1, 8, 0), 1) == EnforcerBlockerType::BLOCKER);
}

TEST_CASE("Existing stained-glass project can migrate its painted topology", "[TraceryPartition]")
{
    Model                     model;
    DynamicPrintConfig        config;
    ConfigSubstitutionContext context{ForwardCompatibilitySubstitutionRule::Disable};
    const std::string         path = (boost::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
                                      "test file/cathedral-stained-glass-window.3mf")
                                         .string();
    INFO(path);
    REQUIRE(boost::filesystem::exists(path));
    PlateDataPtrs        plates;
    std::vector<Preset*> presets;
    bool                 is_bbs = false;
    Semver               version;
    struct Cleanup
    {
        Model&         model;
        PlateDataPtrs& plates;
        ~Cleanup()
        {
            model.remove_backup_path_if_exist();
            for (auto* plate : plates)
                delete plate;
        }
    } cleanup{model, plates};
    model.set_backup_path((boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("tracery-fixture-%%%%-%%%%")).string());
    REQUIRE(load_bbs_3mf(path.c_str(), &config, &context, &model, &plates, &presets, &is_bbs, &version, nullptr, LoadStrategy::LoadModel));
    REQUIRE(presets.empty());
    size_t painted_volumes = 0;
    size_t arched_volumes = 0;
    for (const ModelObject* object : model.objects) {
        for (const ModelVolume* volume : object->volumes) {
            if (volume->is_model_part() && volume->name == "part_22") {
                ++arched_volumes;
                for (bool both_sides : {false, true}) {
                    CAPTURE(both_sides);
                    // The arched window's shared edge produces independently allocated
                    // cut vertices that round to identical float coordinates.
                    const auto& mesh = volume->mesh();
                    TriangleSelector::Tracery2DData regions;
                    for (size_t i = 0; i < mesh.its.indices.size(); ++i) {
                        const auto& t = mesh.its.indices[i];
                        const Vec3d a = mesh.its.vertices[t[0]].cast<double>();
                        const Vec3d b = mesh.its.vertices[t[1]].cast<double>() - a;
                        const Vec3d c = mesh.its.vertices[t[2]].cast<double>() - a;
                        const Vec3d normal = b.cross(c);
                        if (std::abs(normal.normalized().z()) < .5)
                            continue;
                        if (!both_sides && normal.z() < 0.)
                            continue;
                        TriangleSelector::TraceryFacet2DData facet;
                        facet.source_facet_id = int(i);
                        facet.origin = a;
                        facet.axis_u = b.normalized();
                        facet.axis_v = normal.normalized().cross(facet.axis_u);
                        TriangleSelector::TraceryRegion2D region;
                        region.color = EnforcerBlockerType(2);
                        region.polygon = {Vec2d::Zero(), Vec2d(b.norm() * .371, 0.),
                                          Vec2d(c.dot(facet.axis_u) * .371, c.dot(facet.axis_v) * .371)};
                        facet.regions.push_back(region);
                        regions.push_back(facet);
                    }
                    const auto partition = build_exact_tracery_partition(mesh, {volume->mmu_segmentation_facets.get_data()}, regions);
                    CHECK(precise_volume(partition.mesh) == Catch::Approx(precise_volume(mesh.its)).epsilon(1e-6).margin(.001));
                    for (size_t i = 0; i < partition.mesh.indices.size(); ++i) {
                        const auto& t = partition.mesh.indices[i];
                        const Vec3d a = partition.mesh.vertices[t[0]].cast<double>();
                        const Vec3d b = partition.mesh.vertices[t[1]].cast<double>();
                        const Vec3d c = partition.mesh.vertices[t[2]].cast<double>();
                        REQUIRE((b - a).cross(c - a).norm() >= 1e-12);
                        CHECK_NOTHROW(clip_tracery_polygon_to_facet(partition.mesh, int(i),
                            {a.cast<float>(), b.cast<float>(), c.cast<float>()}));
                    }
                    std::map<std::pair<int, int>, size_t> edges;
                    for (const auto& t : partition.mesh.indices)
                        for (int e = 0; e < 3; ++e)
                            ++edges[std::minmax(t[e], t[(e + 1) % 3])];
                    for (const auto& edge : edges)
                        CHECK(edge.second == 2);
                }
            }
            if (!volume->is_model_part() || volume->mmu_segmentation_facets.empty())
                continue;
            ++painted_volumes;
            const auto result = build_exact_tracery_partition(volume->mesh(), {volume->mmu_segmentation_facets.get_data()}, {});
            CHECK(result.mesh.indices.size() >= volume->mesh().its.indices.size());
            CHECK_FALSE(result.annotations.front().triangles_to_split.empty());
            CHECK(precise_volume(result.mesh) == Catch::Approx(precise_volume(volume->mesh().its)).epsilon(1e-6).margin(.001));
            if (painted_volumes == 1) {
                TriangleMesh migrated(result.mesh);
                const auto   regions   = inset_facet_partition(migrated);
                const auto   repainted = build_exact_tracery_partition(migrated, result.annotations, regions);
                CHECK(repainted.annotations.front().used_states[2]);
                CHECK(precise_volume(repainted.mesh) == Catch::Approx(precise_volume(migrated.its)).epsilon(1e-6).margin(.001));
                std::vector<Vec3f> polygon;
                const auto&        plane = regions.front();
                for (const Vec2d& p : plane.regions.front().polygon)
                    polygon.push_back((plane.origin + p.x() * plane.axis_u + p.y() * plane.axis_v).cast<float>());
                for (size_t i = 0; i < repainted.source_facets.size(); ++i)
                    if (repainted.source_facets[i] == plane.source_facet_id)
                        CHECK_NOTHROW(clip_tracery_polygon_to_facet(repainted.mesh, int(i), polygon));
            }
        }
    }
    CHECK(painted_volumes > 0);
    CHECK(arched_volumes > 0);
}

TEST_CASE("Tracery restore snapshots survive undo serialization for independent volumes", "[TraceryPartition]")
{
    const auto       source = square_mesh();
    TriangleSelector paint(source);
    paint.set_facet(0, EnforcerBlockerType(2));
    const std::vector<TriangleSelector::TriangleSplittingData> annotations{paint.serialize()};
    const auto partition = build_exact_tracery_partition(source, annotations, straight_partition());
    auto       state     = capture_tracery_restore_state(source, annotations);
    advance_tracery_restore_state(state, source, annotations, partition);
    Model                                   model;
    auto*                                   object = model.add_object();
    auto*                                   first  = object->add_volume(source);
    auto*                                   second = object->add_volume(source);
    std::map<ObjectID, TraceryRestoreState> snapshots{{first->id(), state}, {second->id(), state}};
    std::stringstream                       storage;
    {
        cereal::BinaryOutputArchive output(storage);
        output(snapshots);
    }
    snapshots.clear();
    {
        cereal::BinaryInputArchive input(storage);
        input(snapshots);
    }
    REQUIRE(snapshots.size() == 2);
    snapshots.erase(first->id());
    REQUIRE(snapshots.size() == 1);
    const auto restored = restore_tracery_mesh(snapshots.at(second->id()), TriangleMesh(partition.mesh), partition.annotations);
    CHECK(restored.mesh.indices == source.its.indices);
    CHECK(restored.mesh.vertices == source.its.vertices);
    CHECK(restored.annotations == annotations);
    auto invalid = partition;
    invalid.annotations.clear();
    CHECK_THROWS(advance_tracery_restore_state(state, source, annotations, invalid));
}
