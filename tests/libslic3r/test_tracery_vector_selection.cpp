#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include "libslic3r/TraceryVectorSelection.hpp"
#include "libslic3r/TraceryDocument.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include <cmath>

using namespace Slic3r;

static TriangleMesh selection_plane()
{
    indexed_triangle_set its;
    its.vertices = {{-100, -100, 0}, {100, -100, 0}, {100, 100, 0}, {-100, 100, 0}};
    its.indices  = {{0, 1, 2}, {0, 2, 3}};
    return TriangleMesh(its);
}

TEST_CASE("Vector brush keeps source and selector trees unchanged while preserving holes", "[TraceryVectorSelection]")
{
    const auto       mesh = selection_plane();
    TriangleSelector selector(mesh);
    const auto       before    = selector.serialize();
    auto             cursor    = TriangleSelector::SinglePointCursor::cursor_factory(Vec3f::Zero(), Vec3f(0, 0, 100), 20,
                                                                                     TriangleSelector::CursorType::CIRCLE, Transform3d::Identity(), {});
    auto             selection = TraceryVectorSelection::from_data(mesh, selector.project_cursor(0, *cursor, Transform3d::Identity(), 0));
    CHECK(selector.serialize() == before);
    CHECK(std::abs(selection.area() - M_PI * 400) < 3.);
    auto       small = TriangleSelector::SinglePointCursor::cursor_factory(Vec3f::Zero(), Vec3f(0, 0, 100), 5,
                                                                           TriangleSelector::CursorType::CIRCLE, Transform3d::Identity(), {});
    const auto hole  = TraceryVectorSelection::from_data(mesh, selector.project_cursor(0, *small, Transform3d::Identity(), 0));
    selection.apply(hole, true);
    CHECK(std::abs(selection.area() - M_PI * 375) < 4.);
    auto   triangles = selection.triangulated(mesh);
    size_t count     = 0;
    for (const auto& facet : triangles)
        count += facet.regions.size();
    CHECK(count < 1000);
    const auto roundtrip = TraceryVectorSelection::from_data(mesh, triangles);
    CHECK(std::abs(roundtrip.area() - selection.area()) < 0.01);
    selection.apply(hole, false);
    CHECK(std::abs(selection.area() - M_PI * 400) < 3.);
}

#include "libslic3r/TracerySurfaceSampling.hpp"
#include "libslic3r/TraceryDocumentMetadata.hpp"
#include <catch2/generators/catch_generators.hpp>

TEST_CASE("Vector brush respects transforms, sphere sections and clipping", "[TraceryVectorSelection]")
{
    const auto       mesh = selection_plane();
    TriangleSelector selector(mesh);
    const bool       sphere    = GENERATE(false, true);
    const double     sx        = GENERATE(1., 2.);
    const double     sy        = GENERATE(1., 2.);
    Transform3d      transform = Transform3d::Identity();
    transform.scale(Vec3d(sx, sy, 1.));
    const auto type   = sphere ? TriangleSelector::CursorType::SPHERE : TriangleSelector::CursorType::CIRCLE;
    auto       cursor = TriangleSelector::SinglePointCursor::cursor_factory(Vec3f::Zero(), Vec3f(0, 0, 100), 20, type, transform,
                                                                            TriangleSelector::ClippingPlane({1.f, 0.f, 0.f, 0.f}));
    const auto mask   = TraceryVectorSelection::from_data(mesh, selector.project_cursor(0, *cursor, transform, 0));
    CHECK(std::abs(mask.area() * sx * sy - M_PI * 200) < 2.);
    for (const auto& f : mask.triangulated(mesh))
        for (const auto& r : f.regions)
            for (const auto& p : r.polygon)
                CHECK((f.origin + p.x() * f.axis_u + p.y() * f.axis_v).x() <= 1e-5);
}

TEST_CASE("Swept vector brush stays continuous across source facets", "[TraceryVectorSelection]")
{
    const auto       mesh = selection_plane();
    TriangleSelector selector(mesh);
    const auto       type = GENERATE(TriangleSelector::CursorType::CIRCLE, TriangleSelector::CursorType::SPHERE);
    auto       cursor = TriangleSelector::DoublePointCursor::cursor_factory(Vec3f(-30, 0, 0), Vec3f(30, 0, 0), Vec3f(-30, 0, 100), 10, type,
                                                                            Transform3d::Identity(), {});
    const auto mask   = TraceryVectorSelection::from_data(mesh, selector.project_cursor(1, *cursor, Transform3d::Identity(), 0));
    CHECK(std::abs(mask.area() - (M_PI * 100 + 1200)) < 4.);
    CHECK(mask.regions().size() == 2);
    TriangleSelector paint(mesh);
    paint.set_facet(0, EnforcerBlockerType(2));
    paint.set_facet(1, EnforcerBlockerType(3));
    const auto half = TraceryVectorSelection::from_data(mesh, mask.colored(mesh, paint.serialize(), {EnforcerBlockerType(3)}));
    CHECK(std::abs(half.area() * 2 - mask.area()) < .01);
    selector.bucket_fill_select_triangles(Vec3f::Zero(), 0, {}, 30, true);
    const auto bucket = TraceryVectorSelection::from_data(mesh, selector.seed_fill_regions());
    CHECK(std::abs(bucket.area() - 40000.) < .01);
}

TEST_CASE("Vector selection persists independently of ordinary paint and through operations", "[TraceryVectorSelection]")
{
    const auto       mesh    = selection_plane();
    const bool       through = GENERATE(false, true);
    TriangleSelector selector(mesh);
    selector.set_facet(0, EnforcerBlockerType(2));
    selector.set_facet(1, EnforcerBlockerType(2));
    std::vector<TraceryDocument::Annotation> annotations(8);
    annotations[0] = annotations[5] = selector.serialize();
    auto document                   = TraceryDocument::capture(mesh, annotations);
    CHECK(TraceryDocument::decode(document.encode()).format_version() == 1);
    auto cursor = TriangleSelector::SinglePointCursor::cursor_factory(Vec3f::Zero(), Vec3f(0, 0, 100), 20,
                                                                      TriangleSelector::CursorType::CIRCLE, Transform3d::Identity(), {});
    auto mask   = TraceryVectorSelection::from_data(mesh, selector.project_cursor(0, *cursor, Transform3d::Identity(), 0));
    auto paint  = mask.triangulated(mesh);
    for (auto& f : paint)
        for (auto& r : f.regions)
            r.color = EnforcerBlockerType(3);
    const auto applied = document.apply_vector_edit(mesh, annotations, paint, TraceryEdit::Paint, through ? &paint : nullptr);
    CHECK(applied.mesh.indices == mesh.its.indices);
    CHECK(applied.mesh.vertices == mesh.its.vertices);
    CHECK(document.format_version() == (through ? 3 : 2));
    document.set_vector_selection(mask.triangulated(mesh));
    const auto saved = document.encode();
    CHECK(tracery_document_version(saved) == 4);
    auto restored = TraceryDocument::decode(saved);
    CHECK(restored.has_vector_selection());
    CHECK(restored.through_operations().size() == size_t(through));
    CHECK(restored.last_annotations()[0] == annotations[0]);
    CHECK(std::abs(TraceryVectorSelection::from_data(mesh, restored.selection_regions()).area() - mask.area()) < .01);
    auto       small    = TriangleSelector::SinglePointCursor::cursor_factory(Vec3f::Zero(), Vec3f(0, 0, 100), 5,
                                                                              TriangleSelector::CursorType::CIRCLE, Transform3d::Identity(), {});
    const auto erased   = TraceryVectorSelection::from_data(mesh, selector.project_cursor(0, *small, Transform3d::Identity(), 0));
    const auto previous = restored.encode();
    restored.apply_vector_edit(mesh, restored.last_annotations(), erased.triangulated(mesh), TraceryEdit::Erase);
    mask.apply(erased, true);
    restored.set_vector_selection(mask.triangulated(mesh));
    const auto after = TraceryDocument::decode(restored.encode());
    CHECK(std::abs(TraceryVectorSelection::from_data(mesh, after.selection_regions()).area() - mask.area()) < .01);
    CHECK(TraceryDocument::decode(previous).through_operations().size() == size_t(through));
    CHECK(after.base_mesh().its.indices == mesh.its.indices);
    // Surface inside the hole returns to the original material.
    std::map<unsigned, double> areas;
    for (const auto& [material, surface] : after.material_surfaces())
        for (const auto& f : surface.indices)
            areas[material] +=
                .5 * (surface.vertices[f[1]] - surface.vertices[f[0]]).cross(surface.vertices[f[2]] - surface.vertices[f[0]]).norm();
    CHECK(std::abs(areas[3] - mask.area()) < .1);
    CHECK(std::abs(areas[2] + areas[3] - 40000.) < .1);
}

TEST_CASE("Surface adjacency joins T junctions but not point contacts or separate volumes", "[TraceryVectorSelection]")
{
    std::vector<TracerySurfaceEdge>     edges{{0, 0, {0, 0, 0}, {10, 0, 0}},  {0, 1, {5, 0, 0}, {0, 0, 0}},  {0, 2, {10, 0, 0}, {5, 0, 0}},
                                              {0, 3, {10, 0, 0}, {15, 0, 0}}, {1, 4, {0, 0, 0}, {10, 0, 0}}, {0, 5, {0, .01, 0}, {10, .01, 0}}};
    std::set<std::pair<size_t, size_t>> pairs;
    tracery_shared_edges(edges, [&](size_t a, size_t b) { pairs.insert(std::minmax(a, b)); });
    CHECK(pairs == std::set<std::pair<size_t, size_t>>{{0, 1}, {0, 2}});
}

TEST_CASE("Seed candidate count follows seed budget and surface area", "[TraceryVectorSelection]")
{
    const std::vector<std::array<Vec3f, 3>> triangles{{{{0, 0, 0}, {2, 0, 0}, {0, 1, 0}}}, {{{10, 0, 0}, {16, 0, 0}, {10, 3, 0}}}};
    const auto                              candidates = tracery_area_candidates(triangles, 20);
    CHECK(candidates.size() == 320);
    CHECK(candidates == tracery_area_candidates(triangles, 20));
    CHECK(std::count_if(candidates.begin(), candidates.end(), [](const Vec3f& p) { return p.x() < 5; }) == 32);
    for (const auto& p : candidates) {
        const bool   first = p.x() < 5;
        const double x     = first ? p.x() / 2. : (p.x() - 10.) / 6.;
        const double y     = first ? p.y() : p.y() / 3.;
        CHECK(x >= 0.);
        CHECK(y >= 0.);
        CHECK(x + y <= 1.000001);
    }
    std::vector<std::array<Vec3f, 3>> dense(10000, triangles.front());
    CHECK(tracery_area_candidates(dense, 20).size() == candidates.size());
    CHECK(tracery_area_candidates({}, 20).empty());
}

TEST_CASE("Vector selection retains sub-micron source slivers", "[TraceryVectorSelection]")
{
    indexed_triangle_set its;
    its.vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1e-8f, 0}};
    its.indices  = {{0, 1, 2}};
    TriangleMesh     mesh(its);
    TriangleSelector selector(mesh);
    selector.set_facet(0, EnforcerBlockerType::ENFORCER);
    const auto mask = TraceryVectorSelection::from_annotation(mesh, selector.serialize());
    CHECK(std::abs(mask.area() - 5e-9) < 1e-12);
    const auto recovered = TraceryVectorSelection::from_data(mesh, mask.triangulated(mesh));
    CHECK(std::abs(recovered.area() - mask.area()) < 1e-12);
}

TEST_CASE("Vector input preserves existing painted boundaries inside a source triangle", "[TraceryVectorSelection]")
{
    const auto       mesh = selection_plane();
    TriangleSelector paint(mesh);
    auto             ordinary = TriangleSelector::SinglePointCursor::cursor_factory(Vec3f::Zero(), Vec3f(0, 0, 100), 5,
                                                                                    TriangleSelector::CursorType::CIRCLE, Transform3d::Identity(), {});
    paint.select_patch(0, std::move(ordinary), EnforcerBlockerType(2), Transform3d::Identity(), true);
    const auto saved     = paint.serialize();
    auto       cursor    = TriangleSelector::SinglePointCursor::cursor_factory(Vec3f::Zero(), Vec3f(0, 0, 100), 20,
                                                                               TriangleSelector::CursorType::CIRCLE, Transform3d::Identity(), {});
    const auto selection = TraceryVectorSelection::from_data(mesh, paint.project_cursor(0, *cursor, Transform3d::Identity(), 0));
    const auto result    = selection.colored(mesh, saved);
    TraceryVectorSelection::Data painted;
    for (const auto& facet : result) {
        auto f = facet;
        f.regions.erase(std::remove_if(f.regions.begin(), f.regions.end(), [](const auto& r) { return r.color != EnforcerBlockerType(2); }),
                        f.regions.end());
        painted.push_back(std::move(f));
    }
    const auto colored  = TraceryVectorSelection::from_data(mesh, painted);
    const auto original = TraceryVectorSelection::from_annotation(mesh, saved);
    CHECK(std::abs(colored.area() - original.area()) < .01);
    CHECK(std::abs(colored.area() - M_PI * 25) < 1.);
    CHECK(paint.serialize() == saved);
}

TEST_CASE("Vector brush preview removes shared triangle edges including T junctions", "[TraceryVectorSelection]")
{
    const std::vector<TracerySurfaceEdge> edges{{0, 0, {0, 0, 0}, {10, 0, 0}},
                                                {0, 1, {5, 0, 0}, {0, 0, 0}},
                                                {0, 2, {10, 0, 0}, {5, 0, 0}},
                                                {0, 3, {10, 0, 0}, {10, 10, 0}},
                                                {0, 4, {10, 10, 0}, {0, 10, 0}}};
    const auto                            boundary = tracery_exposed_edges(edges);
    CHECK(boundary.size() == 2);
    double length = 0.;
    for (const auto& edge : boundary)
        length += (edge.second - edge.first).norm();
    CHECK(std::abs(length - 20.) < 1e-8);
}

TEST_CASE("Spherical vector brush follows a sharp surface fold without bridging empty space", "[TraceryVectorSelection]")
{
    const TriangleMesh mesh   = make_cube(10, 10, 10);
    const auto         bounds = mesh.bounding_box();
    const Vec3f        center(bounds.max.x(), (bounds.min.y() + bounds.max.y()) * .5, bounds.max.z());
    int                start = -1;
    for (size_t i = 0; i < mesh.its.indices.size(); ++i) {
        int top = 0, side = 0;
        for (int v : mesh.its.indices[i]) {
            top += mesh.its.vertices[v].z() == center.z();
            side += mesh.its.vertices[v].x() == center.x();
        }
        if (top == 3 && side == 2)
            start = int(i);
    }
    REQUIRE(start >= 0);
    TriangleSelector selector(mesh);
    auto             cursor = TriangleSelector::SinglePointCursor::cursor_factory(center, center + Vec3f(20, 0, 20), 3,
                                                                                  TriangleSelector::CursorType::SPHERE, Transform3d::Identity(), {});
    const auto       mask   = TraceryVectorSelection::from_data(mesh, selector.project_cursor(start, *cursor, Transform3d::Identity(), 0));
    CHECK(std::abs(mask.area() - M_PI * 9) < .2);
    for (const auto& f : mask.triangulated(mesh))
        for (const auto& r : f.regions)
            for (const auto& p : r.polygon) {
                const Vec3d point = f.origin + p.x() * f.axis_u + p.y() * f.axis_v;
                CHECK((std::abs(point.x() - center.x()) < 1e-5 || std::abs(point.z() - center.z()) < 1e-5));
                CHECK((point - center.cast<double>()).norm() <= 3.00001);
            }
}

#include "libslic3r/TraceryVectorFill.hpp"

TEST_CASE("Vector fill selects the connected colour and face without changing source paint", "[TraceryVectorFill]")
{
    const auto mesh = selection_plane();
    TriangleSelector source(mesh);
    source.set_facet(0, EnforcerBlockerType(2));
    source.set_facet(1, EnforcerBlockerType(3));
    const auto before = source.serialize();
    TraceryVectorFill fill(source);
    auto region = TraceryVectorSelection::from_data(mesh, fill.select(Vec3f(50, -50, 0), 0, {}, 30, true));
    CHECK(region.area() == Catch::Approx(20000.));
    region = TraceryVectorSelection::from_data(mesh, fill.select(Vec3f(-50, 50, 0), 1, {}, -1, false));
    CHECK(region.area() == Catch::Approx(20000.));
    CHECK(source.serialize() == before);
}

TEST_CASE("Vector fill preserves subdivided ordinary paint and invalidates changed query policy", "[TraceryVectorFill]")
{
    const auto mesh = selection_plane();
    TriangleSelector source(mesh);
    auto brush = TriangleSelector::SinglePointCursor::cursor_factory(Vec3f(2, -2, 0), Vec3f(2, -2, 100), 8,
                                                                    TriangleSelector::CursorType::CIRCLE, Transform3d::Identity(), {});
    source.select_patch(0, std::move(brush), EnforcerBlockerType(2), Transform3d::Identity(), true);
    const auto saved = source.serialize();
    TriangleSelector reference(mesh);
    reference.deserialize(saved, false);
    reference.bucket_fill_select_triangles(Vec3f(2, -2, 0), 0, {}, 30, true, true);
    const auto expected = TraceryVectorSelection::from_data(mesh, reference.seed_fill_regions());
    TraceryVectorFill fill(source);
    const auto actual = TraceryVectorSelection::from_data(mesh, fill.select(Vec3f(2, -2, 0), 0, {}, 30, true));
    CHECK(actual.difference(expected).area() < 1e-6);
    CHECK(expected.difference(actual).area() < 1e-6);
    CHECK(fill.show_preview());
    CHECK_FALSE(fill.show_preview());
    CHECK(fill.clear_preview());
    const size_t revision = fill.revision();
    CHECK(TraceryVectorSelection::from_data(mesh, fill.select(Vec3f(2, -2, 0), 0, {}, 30, true)) == actual);
    CHECK(fill.revision() == revision);
    CHECK(fill.show_preview());
    // Policy changes must invalidate a warmed colour-region cache.
    source.set_unselectable_states({EnforcerBlockerType(2)});
    CHECK(fill.select(Vec3f(2, -2, 0), 0, {}, 30, true).empty());
    CHECK(fill.revision() != revision);
    CHECK(fill.show_preview()); // The previous outline needs removing.
    source.set_unselectable_states({});
    CHECK(TraceryVectorSelection::from_data(mesh, fill.select(Vec3f(2, -2, 0), 0, {}, 30, true)) == actual);
    reference.bucket_fill_select_triangles(Vec3f(2, -2, 0), 0, {}, -1, false, true);
    const auto face = TraceryVectorSelection::from_data(mesh, reference.seed_fill_regions());
    CHECK(TraceryVectorSelection::from_data(mesh, fill.select(Vec3f(2, -2, 0), 0, {}, -1, false)) == face);
    CHECK(fill.select(Vec3f::Zero(), -1, {}, 30, true).empty());
    CHECK(source.serialize() == saved);
}

TEST_CASE("Vector fill obeys fold angles and clips the selected face in vector coordinates", "[TraceryVectorFill]")
{
    indexed_triangle_set its;
    its.vertices = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}, {0, 0, 10}};
    its.indices = {{0, 1, 2}, {1, 0, 3}};
    TriangleMesh mesh(its);
    TriangleSelector source(mesh);
    TraceryVectorFill fill(source);
    const Vec3f hit(1, 1, 0);
    CHECK(TraceryVectorSelection::from_data(mesh, fill.select(hit, 0, {}, 30, true)).area() == Catch::Approx(50.));
    CHECK(TraceryVectorSelection::from_data(mesh, fill.select(hit, 0, {}, -1, true)).area() == Catch::Approx(100.));
    CHECK(TraceryVectorSelection::from_data(mesh, fill.select(hit, 0, {}, -1, false)).area() == Catch::Approx(50.));
    const TriangleSelector::ClippingPlane clip({1.f, 0.f, 0.f, 5.f});
    CHECK(TraceryVectorSelection::from_data(mesh, fill.select(hit, 0, clip, -1, false)).area() == Catch::Approx(37.5));
    CHECK(TraceryVectorSelection::from_data(mesh, fill.select(hit, 0, {}, -1, false)).area() == Catch::Approx(50.));
}

#include <chrono>
#include <iostream>
TEST_CASE("Vector fill benchmark compares cold and repeated selections on identical geometry", "[TraceryVectorFill][TraceryFillBenchmark]")
{
    indexed_triangle_set its;
    constexpr int n = 100;
    for (int y = 0; y <= n; ++y)
        for (int x = 0; x <= n; ++x)
            its.vertices.emplace_back(float(x), float(y), 0.f);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const int a = y * (n + 1) + x;
            its.indices.emplace_back(a, a + 1, a + n + 2);
            its.indices.emplace_back(a, a + n + 2, a + n + 1);
        }
    TriangleMesh mesh(its);
    TriangleSelector source(mesh);
    const auto original = source.serialize();
    auto timed = [](auto&& fn) {
        const auto start = std::chrono::steady_clock::now();
        fn();
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
    const Vec3f hit(.75f, .25f, 0.f);
    TriangleSelector::Tracery2DData old;
    const double legacy = timed([&] {
        source.bucket_fill_select_triangles(hit, 0, {}, 30, true, true);
        old = source.seed_fill_regions();
    });
    source.seed_fill_unselect_all_triangles();
    TraceryVectorFill fill(source);
    const double cold = timed([&] { fill.select(hit, 0, {}, 30, true); });
    const auto expected = TraceryVectorSelection::from_data(mesh, old);
    CHECK(TraceryVectorSelection::from_data(mesh, fill.regions()) == expected);
    CHECK(expected.area() == Catch::Approx(10000.));
    const double repeat = timed([&] {
        for (int i = 0; i < 100; ++i)
            fill.select(hit, 0, {}, 30, true);
    }) / 100.;
    TraceryVectorFill face(source);
    const double single = timed([&] { face.select(hit, 0, {}, -1, false); });
    CHECK(TraceryVectorSelection::from_data(mesh, face.regions()).area() == Catch::Approx(.5));
    CHECK(source.serialize() == original);
    std::cout << "TRACERY_FILL_BENCH facets=" << mesh.its.indices.size() << " legacy_ms=" << legacy
              << " vector_cold_ms=" << cold << " vector_cached_ms=" << repeat << " face_cold_ms=" << single << '\n';
}

TEST_CASE("Vector fill does not reuse a disconnected region or a different clipped seed", "[TraceryVectorFill]")
{
    indexed_triangle_set its;
    its.vertices = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}, {-10, 0, 0}, {20, 0, 0}, {30, 0, 0}, {20, 10, 0}};
    its.indices = {{0, 1, 2}, {0, 2, 3}, {4, 5, 6}};
    TriangleMesh mesh(its);
    TriangleSelector source(mesh);
    TraceryVectorFill fill(source);
    const auto first = TraceryVectorSelection::from_data(mesh, fill.select(Vec3f(1, 1, 0), 0, {}, 30, true));
    const auto separate = TraceryVectorSelection::from_data(mesh, fill.select(Vec3f(21, 1, 0), 2, {}, 30, true));
    CHECK(first.area() == Catch::Approx(100.));
    CHECK(separate.area() == Catch::Approx(50.));
    CHECK(first.difference(separate) == first);
    const TriangleSelector::ClippingPlane clip({1.f, 0.f, 0.f, 5.f});
    CHECK(TraceryVectorSelection::from_data(mesh, fill.select(Vec3f(1, 1, 0), 0, clip, 30, true)).area() == Catch::Approx(87.5));
    // Preserve the existing fill rule: partial neighbours are excluded, but a
    // partial seed can be selected. A different seed cannot reuse this cache.
    CHECK(TraceryVectorSelection::from_data(mesh, fill.select(Vec3f(-1, 1, 0), 1, clip, 30, true)).area() == Catch::Approx(50.));
}
