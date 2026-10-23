#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "libslic3r/TraceryDocument.hpp"
#include "libslic3r/TraceryRestore.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/TraceryPerformance.hpp"
#include "libslic3r/TraceryPolygonPreparation.hpp"
#include "libslic3r/TraceryBasePaint.hpp"
#include "libslic3r/TraceryDisjointSet.hpp"
#include "libslic3r/TraceryPreviousFacetIndex.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include <boost/filesystem.hpp>
#include <fstream>
#include <cstdlib>
#include <chrono>
#include <random>
using namespace Slic3r;

TEST_CASE("Restoring ordinary paint does not replace the mesh or other annotations", "[TraceryRestore]")
{
    Model model;
    auto* object = model.add_object();
    auto* volume = object->add_volume(make_cube(10, 10, 1));
    const auto mesh = volume->mesh().its;
    const auto id = volume->id();
    TriangleSelector original(volume->mesh());
    original.set_facet(0, EnforcerBlockerType(2));
    const auto paint = original.serialize();
    TriangleSelector support(volume->mesh());
    support.set_facet(1, EnforcerBlockerType::ENFORCER);
    volume->supported_facets.set(support);
    const auto old_support = volume->supported_facets.get_data();
    CHECK(restore_tracery_base_paint(*volume, paint));
    CHECK(volume->mmu_segmentation_facets.get_data() == paint);
    CHECK(volume->supported_facets.get_data() == old_support);
    CHECK(volume->mesh().its.vertices == mesh.vertices);
    CHECK(volume->mesh().its.indices == mesh.indices);
    CHECK(volume->id() == id);
    CHECK_FALSE(restore_tracery_base_paint(*volume, paint));
}

TEST_CASE("Exact edge reduction preserves winding around ambiguous junctions", "[TraceryVector][TraceryPrepare]")
{
    const int variant = GENERATE(0, 1, 2, 3);
    Polygons original;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            if (x >= 5 && x < 11 && y >= 5 && y < 11)
                continue;
            // An oblique integer grid, including negative coordinates.
            auto point = [](int u, int v) { return Point(100 * u + 17 * v - 800, 100 * v - 9 * u - 400); };
            original.push_back(Polygon{{point(x, y), point(x + 1, y), point(x + 1, y + 1)}});
            original.push_back(Polygon{{point(x, y), point(x + 1, y + 1), point(x, y + 1)}});
        }
    if (variant == 1)
        original.push_back(original.front()); // same-direction multiplicity
    if (variant == 2) {
        const Point corner = original.back().points[1];
        original.push_back(Polygon{{corner, Point(corner.x() + 100, corner.y()), Point(corner.x(), corner.y() + 100)}});
    }
    if (variant == 3)
        original.push_back(Polygon{{Point(-1000, 0), Point(2000, 0), Point(2000, 100), Point(-1000, 100)}});
    const Polygons expected = union_(original);
    Polygons reduced = original;
    const bool compacted = tracery_cancel_internal_edges(reduced);
    if (variant == 0) {
        CHECK(compacted);
        CHECK(reduced.size() == 2); // contour and clockwise hole
    }
    if (variant == 1 || variant == 2)
        CHECK(compacted);
    const auto actual = union_(reduced);
    CHECK(diff(expected, actual).empty());
    CHECK(diff(actual, expected).empty());
}

TEST_CASE("One ambiguous edge does not prevent reduction of neighbouring patches", "[TraceryVector][TraceryLocalReduction]")
{
    Polygons polygons;
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            polygons.push_back(Polygon{{Point(x, y), Point(x + 1, y), Point(x + 1, y + 1)}});
            polygons.push_back(Polygon{{Point(x, y), Point(x + 1, y + 1), Point(x, y + 1)}});
        }
    // Duplicate one triangle, and add a separate ambiguous island. Neither may
    // disable compression of the rest of the connected, same-material surface.
    polygons.push_back(polygons.front());
    for (int i = 0; i < 64; ++i)
        polygons.push_back(Polygon{{Point(50, 0), Point(51, 0), Point(50, 1)}});
    const auto expected = union_(polygons);
    TraceryReductionStatistics statistics;
    REQUIRE(tracery_cancel_internal_edges(polygons, &statistics));
    CHECK(statistics.group_failures[size_t(TraceryReductionFailure::Multiplicity)] == 1);
    CHECK(statistics.local_compacted_components > 0);
    CHECK(statistics.recovered_input_polygons > 1900);
    CHECK(statistics.quarantined_polygons >= 66);
    CHECK(polygons.size() < 100);
    const auto actual = union_(polygons);
    CHECK(diff(expected, actual).empty());
    CHECK(diff(actual, expected).empty());
}

TEST_CASE("Local reductions preserve holes overlaps and shuffled connected patches", "[TraceryVector][TraceryLocalReduction]")
{
    std::mt19937 random(47291);
    for (int trial = 0; trial < 12; ++trial) {
        CAPTURE(trial);
        Polygons polygons;
        auto point = [&](int x, int y) {
            return Point(100 * x + 7 * y + (y * y + trial) % 13 - 2400,
                         100 * y - 11 * x + (x * x + trial) % 17 - 1800);
        };
        for (int y = 0; y < 48; ++y)
            for (int x = 0; x < 48; ++x) {
                if ((x > 8 && x < 18 && y > 9 && y < 19) || (x > 28 && x < 34 && y > 24 && y < 31))
                    continue;
                polygons.push_back(Polygon{{point(x, y), point(x + 1, y), point(x + 1, y + 1)}});
                polygons.push_back(Polygon{{point(x, y), point(x + 1, y + 1), point(x, y + 1)}});
            }
        // Defects at different places and an overlapping, non-edge-connected
        // component must preserve the same final union after local reduction.
        for (int i = 0; i <= trial; ++i)
            polygons.push_back(polygons[random() % polygons.size()]);
        polygons.push_back(Polygon{{Point(-2600, -100), Point(3100, -100), Point(3100, 100), Point(-2600, 100)}});
        std::shuffle(polygons.begin(), polygons.end(), random);
        const auto original = polygons;
        const auto expected = union_(polygons);
        const bool changed = tracery_cancel_internal_edges(polygons);
        const auto actual = union_(polygons);
        CHECK(diff(expected, actual).empty());
        CHECK(diff(actual, expected).empty());
        if (!changed) {
            REQUIRE(polygons.size() == original.size());
            for (size_t i = 0; i < polygons.size(); ++i)
                CHECK(polygons[i].points == original[i].points);
        }
    }
}

TEST_CASE("Edge reduction reports size and exact-edge limits without altering input", "[TraceryVector][TraceryLocalReduction]")
{
    Polygons polygons;
    for (int i = 0; i < 64; ++i)
        polygons.push_back(Polygon{{Point(3 * i, 0), Point(3 * i + 1, 0), Point(3 * i, 1)}});
    TraceryReductionStatistics statistics;
    CHECK_FALSE(tracery_cancel_internal_edges(polygons, &statistics));
    CHECK(statistics.group_failures[size_t(TraceryReductionFailure::FewSharedEdges)] == 1);
    CHECK(statistics.group_input_polygons[size_t(TraceryReductionFailure::FewSharedEdges)] == 64);
    CHECK(statistics.retained_polygons == 64);
    CHECK(statistics.local_components == 0);
    // An oversized input must stop before building either edge index.
    polygons.front().points.resize(2000001, Point(0, 0));
    CHECK_FALSE(tracery_cancel_internal_edges(polygons, &statistics));
    CHECK(statistics.group_failures[size_t(TraceryReductionFailure::EdgeBudget)] == 1);
    CHECK(statistics.local_components == 0);
    CHECK(polygons.front().points.size() == 2000001);
}

TEST_CASE("Vector preparation preserves fragmented colours and holes", "[TraceryVector][TraceryPrepare]")
{
    const bool duplicate = GENERATE(false, true);
    // Raise the grid size for a repeatable performance run without changing the
    // geometry, colours, source facet or final expected regions.
    const char* setting = std::getenv("TRACERY_PREP_BENCH_SIZE");
    const int n = setting ? std::clamp(std::atoi(setting), 32, 1024) / 32 * 32 : 32;
    indexed_triangle_set its;
    its.vertices = {{0, 0, 0}, {256, 0, 0}, {0, 256, 0}, {0, 0, 2}, {256, 0, 2}, {0, 256, 2}};
    its.indices = {{0, 1, 2}, {3, 4, 5}};
    const TriangleMesh mesh(its);
    std::vector<TraceryDocument::Annotation> annotations(8);
    auto document = TraceryDocument::capture(mesh, annotations);
    TriangleSelector::TraceryFacet2DData facet;
    facet.source_facet_id = 0;
    const double step = 128. / n;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            if (x >= 3 * n / 8 && x < 5 * n / 8 && y >= 3 * n / 8 && y < 5 * n / 8)
                continue;
            const Vec2d a(x * step, y * step), b((x + 1) * step, y * step);
            const Vec2d c((x + 1) * step, (y + 1) * step), d(x * step, (y + 1) * step);
            for (const auto& points : {std::vector<Vec2d>{a, b, c}, std::vector<Vec2d>{a, c, d}}) {
                TriangleSelector::TraceryRegion2D region;
                region.color = EnforcerBlockerType(x < n / 2 ? 2 : 7);
                region.polygon = points;
                facet.regions.push_back(std::move(region));
            }
        }
    if (duplicate)
        facet.regions.push_back(facet.regions.front());
    const auto begin = std::chrono::steady_clock::now();
    const auto result = document.apply_vector_edit(mesh, annotations, {facet}, TraceryEdit::Paint);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    std::fprintf(stderr, "TRACERY_PREP_BENCH grid=%d duplicate=%d polygons=%zu elapsed_ms=%.3f\n",
                 n, int(duplicate), facet.regions.size(), ms);
    CHECK(result.mesh.vertices == its.vertices);
    CHECK(result.mesh.indices == its.indices);
    auto rectangle = [](double x0, double y0, double x1, double y1) {
        return Polygon{{Point(scale_(x0), scale_(y0)), Point(scale_(x1), scale_(y0)),
                        Point(scale_(x1), scale_(y1)), Point(scale_(x0), scale_(y1))}};
    };
    const Polygons hole{rectangle(48, 48, 80, 80)};
    const Polygons boundary{Polygon{{Point(0, 0), Point(scale_(256.), 0.), Point(0., scale_(256.))}}};
    std::map<unsigned, Polygons> expected;
    expected[2] = diff(Polygons{rectangle(0, 0, 64, 128)}, hole);
    expected[7] = diff(Polygons{rectangle(64, 0, 128, 128)}, hole);
    Polygons painted = expected[2];
    append(painted, expected[7]);
    expected[0] = diff(boundary, painted);
    auto verify = [&](const TraceryDocument& doc, const std::map<unsigned, Polygons>& wanted) {
        std::map<unsigned, Polygons> actual;
        for (const auto& surface : doc.surface_regions()) {
            if (surface.source_facet_id != 0) {
                for (const auto& region : surface.regions)
                    CHECK(region.color == EnforcerBlockerType::NONE);
                continue;
            }
            for (const auto& region : surface.regions) {
                Polygon p;
                for (const auto& v : region.polygon)
                    p.points.emplace_back(scale_(v.x()), scale_(v.y()));
                actual[unsigned(region.color)].push_back(std::move(p));
            }
        }
        for (const auto& [color, polygons] : wanted) {
            CHECK(diff(polygons, actual[color]).empty());
            CHECK(diff(actual[color], polygons).empty());
        }
        for (const auto& [color, polygons] : actual)
            CHECK(wanted.count(color) != 0);
    };
    verify(document, expected);
    verify(TraceryDocument::decode(document.encode()), expected);
    // A local erase must restore the base without changing the other colours or
    // the untouched second surface. A snapshot remains usable for undo/redo.
    const auto snapshot = document.encode();
    facet.regions.resize(1);
    facet.regions[0].polygon = {{8, 8}, {24, 8}, {24, 24}, {8, 24}};
    document.apply_vector_edit(mesh, annotations, {facet}, TraceryEdit::Erase);
    const Polygons erased{rectangle(8, 8, 24, 24)};
    expected[2] = diff(expected[2], erased);
    append(expected[0], erased);
    verify(document, expected);
    auto restored = TraceryDocument::decode(snapshot);
    restored.apply_vector_edit(mesh, annotations, {facet}, TraceryEdit::Erase);
    verify(restored, expected);
}

TEST_CASE("Vector projection clips crossing regions and rejects distant regions", "[TraceryVector][TraceryPrepare]")
{
    indexed_triangle_set its;
    its.vertices = {{0, 0, 0}, {32, 0, 0}, {0, 32, 0}};
    its.indices = {{0, 1, 2}};
    const TriangleMesh mesh(its);
    std::vector<TraceryDocument::Annotation> annotations(8);
    auto document = TraceryDocument::capture(mesh, annotations);
    TriangleSelector::TraceryFacet2DData facet;
    facet.source_facet_id = 0;
    facet.origin = Vec3d(16, 8, 0);
    facet.axis_u = Vec3d(0, 2, 0);
    facet.axis_v = Vec3d(-3, 0, 0);
    Polygons reference;
    for (int y = -12; y < 12; ++y)
        for (int x = -12; x < 12; ++x) {
            TriangleSelector::TraceryRegion2D region;
            region.color = EnforcerBlockerType(7);
            region.polygon = {{double(x), double(y)}, {double(x + 1), double(y)}, {double(x), double(y + 1)}};
            Polygon projected;
            for (const auto& p : region.polygon) {
                const Vec3d v = facet.origin + p.x() * facet.axis_u + p.y() * facet.axis_v;
                projected.points.emplace_back(scale_(v.x()), scale_(v.y()));
            }
            projected.make_counter_clockwise();
            reference.push_back(std::move(projected));
            facet.regions.push_back(std::move(region));
        }
    // The established union-then-clip result is the oracle, including clipping
    // at the diagonal, outside candidates and a differently scaled input frame.
    const Polygons boundary{Polygon{{Point(0, 0), Point(scale_(32.), 0.), Point(0., scale_(32.))}}};
    reference = intersection(union_(reference), boundary);
    const auto result = document.apply_vector_edit(mesh, annotations, {facet}, TraceryEdit::Paint);
    Polygons actual;
    for (const auto& surface : document.surface_regions())
        for (const auto& region : surface.regions)
            if (region.color == EnforcerBlockerType(7)) {
                Polygon p;
                for (const auto& v : region.polygon)
                    p.points.emplace_back(scale_(v.x()), scale_(v.y()));
                actual.push_back(std::move(p));
            }
    CHECK(diff(reference, actual).empty());
    CHECK(diff(actual, reference).empty());
    CHECK(result.mesh.vertices == its.vertices);
    CHECK(result.mesh.indices == its.indices);
}

namespace {
TriangleSelector::TriangleSplittingData uniform_annotation_tree(int depth)
{
    TriangleSelector::TriangleSplittingData data;
    data.triangles_to_split.emplace_back(0, 0);
    auto append_tree = [&](auto&& self, int level) -> void {
        const unsigned code = level == 0 ? 4 : 3; // state 1 leaf / four children
        for (unsigned bit = 0; bit < 4; ++bit)
            data.bitstream.push_back((code & (1u << bit)) != 0);
        if (level > 0)
            for (int child = 0; child < 4; ++child)
                self(self, level - 1);
    };
    append_tree(append_tree, depth);
    data.update_used_states(0);
    return data;
}
}

TEST_CASE("Uniform selection subtrees export once without changing the brush tree", "[TraceryVector][TraceryCoalesce]")
{
    indexed_triangle_set its;
    its.vertices = {{0, 0, 0}, {16, 0, 0}, {0, 16, 0}};
    its.indices = {{0, 1, 2}};
    const TriangleMesh mesh(its);
    TriangleSelector selector(mesh);
    selector.deserialize(uniform_annotation_tree(8));
    const auto original = selector.serialize();
    REQUIRE(selector.painted_leaves(true).size() == 65536);
    const auto compact = selector.coalesced_painted_leaves(true);
    REQUIRE(compact.size() == 1);
    CHECK(compact.front().vertices == std::array<Vec3f, 3>{its.vertices[0], its.vertices[1], its.vertices[2]});
    CHECK(compact.front().source_facet == 0);
    CHECK(compact.front().state == EnforcerBlockerType(1));
    CHECK(selector.serialize() == original);
    const std::vector<int> no_changes;
    CHECK(selector.coalesced_painted_leaves(true, &no_changes).empty());
}

TEST_CASE("Coalesced annotation export preserves colours holes and original facets", "[TraceryVector][TraceryCoalesce]")
{
    indexed_triangle_set its;
    its.vertices = {{0, 0, 0}, {16, 0, 0}, {0, 16, 0}, {20, 0, 0}, {36, 0, 0}, {20, 16, 0}};
    its.indices = {{0, 1, 2}, {3, 4, 5}};
    const TriangleMesh mesh(its);
    TriangleSelector selector(mesh);
    selector.deserialize(uniform_annotation_tree(7));
    auto polygons = [](const std::vector<TriangleSelector::PaintedLeaf>& leaves) {
        std::map<std::pair<int, unsigned>, Polygons> groups;
        for (const auto& leaf : leaves) {
            Polygon p;
            for (const auto& v : leaf.vertices)
                p.points.emplace_back(scale_(v.x()), scale_(v.y()));
            p.make_counter_clockwise();
            groups[{leaf.source_facet, unsigned(leaf.state)}].push_back(std::move(p));
        }
        for (auto& [key, polys] : groups)
            polys = union_(polys);
        return groups;
    };
    for (bool erase : {false, true}) {
        selector.set_leaf_states([&](const Vec3f& p, int facet) {
            if (facet == 1 || p.x() > 10)
                return EnforcerBlockerType::ExtruderMax;
            if (erase && p.y() > 4)
                return EnforcerBlockerType::NONE;
            const float r2 = (p.x() - 4) * (p.x() - 4) + (p.y() - 4) * (p.y() - 4);
            return EnforcerBlockerType(r2 > 1 && r2 < 9 ? 1 : 0);
        });
        const auto before = selector.serialize();
        const auto raw = selector.painted_leaves(true);
        decltype(polygons(raw)) expected, actual;
        {
            TraceryPerformanceScope performance("test.annotation_baseline_union");
            expected = polygons(raw);
        }
        {
            TraceryPerformanceScope performance("test.annotation_compact_union");
            const auto compact = selector.coalesced_painted_leaves(true);
            performance.count("raw_leaves", raw.size());
            performance.count("compact_leaves", compact.size());
            REQUIRE(compact.size() < raw.size() / 4);
            actual = polygons(compact);
        }
        REQUIRE(actual.size() == expected.size());
        for (const auto& [key, area] : expected) {
            REQUIRE(actual.count(key) == 1);
            CHECK(diff(area, actual.at(key)).empty());
            CHECK(diff(actual.at(key), area).empty());
        }
        CHECK(selector.serialize() == before);
        const std::vector<int> only_second{1};
        const auto second = selector.coalesced_painted_leaves(false, &only_second);
        REQUIRE(second.size() == 1);
        CHECK(second.front().source_facet == 1);
        CHECK(second.front().state == EnforcerBlockerType::ExtruderMax);
        for (const auto& leaf : selector.coalesced_painted_leaves(false))
            CHECK(leaf.state != EnforcerBlockerType::NONE);
    }
}

TEST_CASE("Annotation deltas ignore shifted unchanged roots and restore implicit unpainted faces", "[TraceryVector][TraceryCoalesce]")
{
    indexed_triangle_set its;
    its.vertices = {{0, 0, 0}, {16, 0, 0}, {0, 16, 0}, {20, 0, 0}, {36, 0, 0}, {20, 16, 0}};
    its.indices = {{0, 1, 2}, {3, 4, 5}};
    const TriangleMesh mesh(its);
    auto data = uniform_annotation_tree(5);
    const auto bits = data.bitstream;
    data.triangles_to_split.emplace_back(1, int(bits.size()));
    data.bitstream.insert(data.bitstream.end(), bits.begin(), bits.end());
    TriangleSelector selector(mesh);
    selector.deserialize(data);
    const auto before = selector.serialize();
    selector.set_facet(0, EnforcerBlockerType::NONE);
    const auto after = selector.serialize();
    REQUIRE(after.triangles_to_split.size() == 1);
    CHECK(after.triangles_to_split.front().bitstream_start_idx == 0);
    const auto erased = TraceryDocument::erased_regions(mesh, before, after);
    REQUIRE(erased.size() == 1);
    CHECK(erased.front().source_facet_id == 0);
    CHECK(TraceryDocument::erased_regions(mesh, after, after).empty());
    CHECK(TraceryDocument::erased_regions(mesh, after, before).empty()); // repaint is not an erase
    std::vector<TraceryDocument::Annotation> annotations(8);
    annotations[0] = before;
    auto document = TraceryDocument::capture(mesh, annotations);
    annotations[0] = after;
    document.apply_vector_edit(mesh, annotations, {}, TraceryEdit::Paint);
    const auto surface = document.surface_regions();
    REQUIRE(surface.size() == 2);
    for (const auto& facet : surface)
        for (const auto& region : facet.regions)
            CHECK(region.color == EnforcerBlockerType(facet.source_facet_id == 0 ? 0 : 1));
    CHECK(document.base_mesh().its.indices == its.indices);
}

TEST_CASE("Base paint snapshots stay aligned with all model parts", "[TraceryVector][TraceryBasePaint]")
{
    // A later part has a saved document; earlier parts must retain their own
    // ordinary paint rather than becoming empty placeholders.
    using Paint = std::vector<int>;
    const Paint magenta{8}, cyan{5}, saved{11}, unpainted;
    std::vector<Paint> snapshots;
    tracery_append_base_paint(snapshots, magenta, static_cast<const Paint*>(nullptr));
    tracery_append_base_paint(snapshots, cyan, static_cast<const Paint*>(nullptr));
    tracery_append_base_paint(snapshots, magenta, &saved);
    tracery_append_base_paint(snapshots, cyan, &unpainted);
    REQUIRE(snapshots.size() == 4);
    CHECK(snapshots[0] == magenta);
    CHECK(snapshots[1] == cyan);
    CHECK(snapshots[2] == saved);
    // An empty saved snapshot is valid and must not fall back to current paint.
    CHECK(snapshots[3].empty());
}

TEST_CASE("One UI refresh reuses documents beyond the idle cache budget", "[TraceryVector][TraceryCache]")
{
    const TriangleMesh mesh = make_cube(10, 10, 1);
    std::vector<TraceryDocument::Annotation> annotations(8);
    auto document = TraceryDocument::capture(mesh, annotations);
    document.apply_vector_edit(mesh, annotations, {}, TraceryEdit::Paint);
    const auto encoded = document.encode();
    // Legal JSON whitespace isolates cache capacity from geometry algorithms.
    // The first volume exceeds the idle budget on its own; the others exceed it
    // together, reproducing the repeated scans by sidebar/object list/rendering.
    std::vector<std::string> volumes;
    volumes.push_back(encoded + std::string(33 * 1024 * 1024, ' '));
    for (size_t i = 0; i < 5; ++i)
        volumes.push_back(encoded + std::string(8 * 1024 * 1024 + i, ' '));
    std::weak_ptr<const TraceryDocument> released;
    {
        TraceryDocument::ReadCacheScope refresh;
        std::vector<std::shared_ptr<const TraceryDocument>> first_pass;
        for (const auto& data : volumes)
            first_pass.push_back(TraceryDocument::decode_cached(data));
        auto interrupted_callback = [&]() {
            TraceryDocument::ReadCacheScope nested_refresh;
            CHECK(TraceryDocument::decode_cached(volumes.front()) == first_pass.front());
            // Invalid replacement data must throw, never return a stale snapshot.
            TraceryDocument::decode_cached("{invalid replacement}");
        };
        CHECK_THROWS(interrupted_callback());
        for (size_t pass = 0; pass < 3; ++pass)
            for (size_t i = 0; i < volumes.size(); ++i)
                CHECK(TraceryDocument::decode_cached(volumes[i]) == first_pass[i]);
        released = first_pass.front();
    }
    CHECK(released.expired()); // oversized working sets must not accumulate
}

TEST_CASE("Vector document snapshots reuse identical data without sharing mutable edits", "[TraceryVector][TraceryCache]")
{
    indexed_triangle_set its;
    its.vertices = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}};
    its.indices = {{0, 1, 2}};
    TriangleMesh mesh(its);
    std::vector<TraceryDocument::Annotation> annotations(8);
    auto document = TraceryDocument::capture(mesh, annotations);
    document.apply_vector_edit(mesh, annotations, {}, TraceryEdit::Paint);
    const auto original = document.encode();
    const auto before = TraceryDocument::decode_cached(original);
    REQUIRE(before == TraceryDocument::decode_cached(original));
    TriangleSelector::TraceryFacet2DData facet;
    facet.source_facet_id = 0;
    TriangleSelector::TraceryRegion2D region;
    region.color = EnforcerBlockerType(7);
    region.polygon = {{0, 0}, {4, 0}, {4, 6}, {0, 10}};
    facet.regions.push_back(region);
    document.apply_vector_edit(mesh, annotations, {facet}, TraceryEdit::Paint);
    const auto after = TraceryDocument::decode_cached(document.encode());
    REQUIRE(after != before);
    REQUIRE(before->material_ids().empty());
    REQUIRE(after->material_ids() == std::vector<unsigned>{7});
    REQUIRE(TraceryDocument::decode_cached(original) == before); // undo
    auto editable = *after;
    editable.apply_vector_edit(mesh, annotations, {}, TraceryEdit::Regenerate);
    REQUIRE(after->material_ids() == std::vector<unsigned>{7});
    REQUIRE(editable.material_ids().empty());
}

TEST_CASE("Previous selection candidates preserve the original scan order", "[TraceryVector][TracerySelectionIndex]")
{
    const std::vector<int> sources = {3, 1, 3, 8, 1, 3, -1};
    TraceryPreviousFacetIndex index;
    for (size_t i = 0; i < sources.size(); ++i)
        index.add(sources[i], i);
    for (int source : {-1, 1, 3, 8, 9}) {
        std::vector<size_t> expected;
        for (size_t i = 0; i < sources.size(); ++i)
            if (sources[i] == source)
                expected.push_back(i);
        CHECK(index.candidates(source) == expected);
    }
}

TEST_CASE("Vector snapshots follow ordinary paint and undo without mutating cached documents", "[TraceryVector][TraceryCache]")
{
    Model model;
    auto* volume = model.add_object()->add_volume(make_cube(10, 10, 1));
    std::vector<TraceryDocument::Annotation> annotations(8);
    auto document = TraceryDocument::capture(volume->mesh(), annotations);
    document.apply_vector_edit(volume->mesh(), annotations, {}, TraceryEdit::Paint);
    volume->tracery_edit_data = document.encode();
    const auto before = TraceryDocument::from_volume_snapshot(*volume);
    REQUIRE(before);
    REQUIRE(before == TraceryDocument::from_volume_snapshot(*volume));
    TriangleSelector ordinary(volume->mesh());
    ordinary.set_facet(0, EnforcerBlockerType(7));
    volume->mmu_segmentation_facets.set(ordinary);
    const auto painted = TraceryDocument::from_volume_snapshot(*volume);
    REQUIRE(painted->material_ids() == std::vector<unsigned>{7});
    REQUIRE(before->material_ids().empty());
    ordinary.set_facet(0, EnforcerBlockerType::NONE);
    volume->mmu_segmentation_facets.set(ordinary);
    REQUIRE(TraceryDocument::from_volume_snapshot(*volume)->material_ids().empty());
    auto editable = TraceryDocument::from_volume(*volume);
    REQUIRE(editable);
    REQUIRE(editable.get() != before.get());
    // A replaced mesh with stale vector data must never reuse the old surface.
    const auto saved = volume->tracery_edit_data;
    volume->set_mesh(make_cube(20, 10, 1));
    volume->tracery_edit_data = saved;
    REQUIRE_THROWS(TraceryDocument::from_volume_snapshot(*volume));
}

TEST_CASE("Stained glass groups retain their connectivity for long merge chains", "[TraceryVector][TraceryConnectivity]")
{
    constexpr size_t count = 20000;
    TraceryDisjointSet groups(count);
    // This merge order previously built a deep parent chain.
    for (size_t i = 1; i < count / 2; ++i)
        groups.unite(i, i - 1);
    for (size_t i = count / 2 + 1; i < count; ++i)
        groups.unite(i, i - 1);
    const size_t first = groups.root(0);
    const size_t second = groups.root(count / 2);
    REQUIRE(first != second);
    for (size_t i = 0; i < count; ++i)
        REQUIRE(groups.root(i) == (i < count / 2 ? first : second));
    groups.unite(0, count / 2);
    REQUIRE(groups.root(0) == groups.root(count - 1));
}

TEST_CASE("Rebuilding stained glass never promotes derived triangles into the base", "[TraceryDocument]")
{
    indexed_triangle_set mesh;
    mesh.vertices = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}};
    mesh.indices  = {{0, 1, 2}};
    TriangleMesh                                         base(mesh);
    std::vector<TriangleSelector::TriangleSplittingData> annotations(8);
    auto                                                 document = TraceryDocument::capture(base, annotations);
    TriangleSelector::TraceryFacet2DData                 facet;
    facet.source_facet_id = 0;
    TriangleSelector::TraceryRegion2D region;
    region.polygon = {{0, 0}, {4, 0}, {4, 6}, {0, 10}};
    region.color   = EnforcerBlockerType(2);
    facet.regions.push_back(region);
    auto first = document.rebuild(base, annotations, {facet}, TraceryEdit::Paint);
    REQUIRE(first.mesh.indices.size() > 1);
    const auto expected = first.mesh;
    for (int stroke = 0; stroke < 10; ++stroke) {
        TriangleMesh current(first.mesh);
        first = document.rebuild(current, first.annotations, {}, TraceryEdit::Paint);
        CHECK(document.base_mesh().its.indices.size() == 1);
        CHECK(first.mesh.vertices == expected.vertices);
        CHECK(first.mesh.indices == expected.indices);
        for (int source : first.source_facets)
            CHECK(source == 0);
    }
}

TEST_CASE("Erasing part of stained glass restores paint without regenerating the remainder", "[TraceryDocument]")
{
    indexed_triangle_set mesh;
    mesh.vertices = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}};
    mesh.indices  = {{0, 1, 2}};
    TriangleMesh     base(mesh);
    TriangleSelector original(base), mask(base);
    original.set_facet(0, EnforcerBlockerType(5));
    mask.set_facet(0, EnforcerBlockerType(1));
    std::vector<TriangleSelector::TriangleSplittingData> annotations(8);
    annotations[0] = annotations[5] = original.serialize();
    auto document                   = TraceryDocument::capture(base, annotations);
    annotations[6]                  = mask.serialize();
    TriangleSelector::TraceryFacet2DData facet;
    facet.source_facet_id = 0;
    TriangleSelector::TraceryRegion2D left, right;
    left.polygon           = {{0, 0}, {4, 0}, {4, 6}, {0, 10}};
    right.polygon          = {{4, 0}, {10, 0}, {4, 6}};
    left.color             = EnforcerBlockerType(2);
    right.color            = EnforcerBlockerType(3);
    facet.regions          = {left, right};
    auto             first = document.rebuild(base, annotations, {facet}, TraceryEdit::Paint);
    TriangleMesh     current(first.mesh);
    TriangleSelector erase(current);
    erase.set_leaf_states([](const Vec3f& p, int) { return EnforcerBlockerType(p.x() < 4 ? 0 : 1); });
    auto changed            = first.annotations;
    changed[6]              = erase.serialize();
    auto             result = document.rebuild(current, changed, {}, TraceryEdit::Erase);
    TriangleMesh     output(result.mesh);
    TriangleSelector colours(output);
    colours.deserialize(result.annotations[0], false);
    for (const auto& leaf : colours.painted_leaves(true)) {
        const Vec3f center = (leaf.vertices[0] + leaf.vertices[1] + leaf.vertices[2]) / 3.f;
        CHECK(unsigned(leaf.state) == (center.x() < 4 ? 5 : 3));
    }
}

TEST_CASE("Saved stained-glass editing data resumes on the same base and rejects changed geometry", "[TraceryDocument]")
{
    indexed_triangle_set mesh;
    mesh.vertices = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}};
    mesh.indices  = {{0, 1, 2}};
    TriangleMesh                                         base(mesh);
    std::vector<TriangleSelector::TriangleSplittingData> annotations(8);
    auto                                                 document = TraceryDocument::capture(base, annotations);
    TriangleSelector::TraceryFacet2DData                 facet;
    facet.source_facet_id = 0;
    TriangleSelector::TraceryRegion2D region;
    region.polygon = {{1, 1}, {4, 1}, {1, 4}};
    region.color   = EnforcerBlockerType(2);
    facet.regions.push_back(region);
    auto         result   = document.rebuild(base, annotations, {facet}, TraceryEdit::Paint);
    const auto   encoded  = document.encode();
    auto         reopened = TraceryDocument::decode(encoded);
    TriangleMesh current(result.mesh);
    CHECK(reopened.matches(current));
    auto again = reopened.rebuild(current, result.annotations, {}, TraceryEdit::Paint);
    CHECK(again.mesh.vertices == result.mesh.vertices);
    CHECK(again.mesh.indices == result.mesh.indices);
    CHECK(again.annotations == result.annotations);
    current.its.vertices[0].x() += 1.f; // same facet count is not proof of identity
    CHECK_FALSE(reopened.matches(current));
    CHECK_THROWS(reopened.rebuild(current, result.annotations, {}, TraceryEdit::Paint));
    CHECK(reopened.encode() == encoded);
    CHECK_THROWS(TraceryDocument::decode("{\"version\":999}"));
}

TEST_CASE("Project save preserves editable base data and volume copies own their edits", "[TraceryDocument][TraceryVector]")
{
    const bool vector_surface = GENERATE(false, true);
    TriangleMesh base = make_cube(10, 10, 1);
    base.translate(-5.f, -5.f, -.5f);
    std::vector<TriangleSelector::TriangleSplittingData> annotations(8);
    auto                                                 document = TraceryDocument::capture(base, annotations);
    TriangleSelector::Tracery2DData                      paint;
    for (size_t i = 0; i < base.its.indices.size(); ++i) {
        const auto& t = base.its.indices[i];
        if (base.its.vertices[t[0]].z() != .5f || base.its.vertices[t[1]].z() != .5f || base.its.vertices[t[2]].z() != .5f)
            continue;
        TriangleSelector::TraceryFacet2DData f;
        f.source_facet_id = int(i);
        f.origin.z()      = .5;
        TriangleSelector::TraceryRegion2D r;
        r.polygon = {{-5, -5}, {.2, -5}, {2.2, 5}, {-5, 5}};
        r.color   = EnforcerBlockerType(2);
        f.regions.push_back(r);
        paint.push_back(f);
    }
    auto result = vector_surface ? document.apply_vector_edit(base, annotations, paint, TraceryEdit::Paint) :
                                   document.rebuild(base, annotations, paint, TraceryEdit::Paint);
    Model model;
    model.set_backup_path((boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("tracery-source-%%%%-%%%%")).string());
    auto* object = model.add_object();
    auto* volume = object->add_volume(TriangleMesh(result.mesh));
    object->add_instance();
    REQUIRE(document.matches(volume->mesh()));
    volume->tracery_edit_data = document.encode();
    TriangleSelector colours(volume->mesh());
    colours.deserialize(result.annotations[0], false);
    volume->mmu_segmentation_facets.set(colours);
    auto* copy = model.add_object(*object);
    CHECK(copy->volumes[0]->tracery_edit_data == volume->tracery_edit_data);
    copy->volumes[0]->set_mesh(make_cube(2, 2, 2));
    CHECK(copy->volumes[0]->tracery_edit_data.empty());
    CHECK_FALSE(volume->tracery_edit_data.empty());
    const auto         path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("tracery-document-%%%%-%%%%.3mf");
    Model              loaded;
    DynamicPrintConfig config;
    ConfigSubstitutionContext context{ForwardCompatibilitySubstitutionRule::Disable};
    PlateDataPtrs             plates;
    std::vector<Preset*>      presets;
    bool                      is_bbs = false;
    Semver                    version;
    struct Cleanup
    {
        boost::filesystem::path path;
        Model&                  source;
        Model&                  loaded;
        PlateDataPtrs&          plates;
        ~Cleanup()
        {
            boost::filesystem::remove(path);
            source.remove_backup_path_if_exist();
            loaded.remove_backup_path_if_exist();
            release_PlateData_list(plates);
        }
    } cleanup{path, model, loaded, plates};
    loaded.set_backup_path((path.string() + ".backup"));
    StoreParams       params;
    const std::string filename = path.string();
    params.path                = filename.c_str();
    params.model               = &model;
    params.config              = &config;
    REQUIRE(store_bbs_3mf(params));
    REQUIRE(
        load_bbs_3mf(filename.c_str(), &config, &context, &loaded, &plates, &presets, &is_bbs, &version, nullptr, LoadStrategy::LoadModel));
    REQUIRE(loaded.objects.size() == 2);
    const auto* restored = loaded.objects.front()->volumes.front();
    REQUIRE_FALSE(restored->tracery_edit_data.empty());
    auto reopened = TraceryDocument::decode(restored->tracery_edit_data);
    REQUIRE(reopened.matches(restored->mesh()));
    CHECK(reopened.is_vector() == vector_surface);
    CHECK(reopened.material_ids() == document.material_ids());
    auto current_annotations = reopened.last_annotations();
    current_annotations[0]   = restored->mmu_segmentation_facets.get_data();
    auto rebuilt             = reopened.rebuild(restored->mesh(), current_annotations, {}, TraceryEdit::Paint);
    CHECK(rebuilt.annotations == result.annotations);
    CHECK(rebuilt.mesh.vertices == result.mesh.vertices);
    CHECK(rebuilt.mesh.indices == result.mesh.indices);
}

TEST_CASE("Ordinary paint and support edits survive erasing a derived flower mesh", "[TraceryDocument][TraceryVector]")
{
    const bool vector_surface = GENERATE(false, true);    indexed_triangle_set its;
    its.vertices = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}};
    its.indices  = {{0, 1, 2}};
    TriangleMesh     base(its);
    TriangleSelector initial(base), mask(base);
    initial.set_facet(0, EnforcerBlockerType(5));
    mask.set_facet(0, EnforcerBlockerType(1));
    std::vector<TraceryDocument::Annotation> annotations(8);
    annotations[0] = annotations[5] = initial.serialize();
    auto document                   = TraceryDocument::capture(base, annotations);
    annotations[6]                  = mask.serialize();
    TriangleSelector::TraceryFacet2DData facet;
    facet.source_facet_id = 0;
    TriangleSelector::TraceryRegion2D region;
    region.color   = EnforcerBlockerType(2);
    region.polygon = {{0, 0}, {4, 0}, {4, 6}, {0, 10}};
    facet.regions.push_back(region);
    auto             first = vector_surface ? document.apply_vector_edit(base, annotations, {facet}, TraceryEdit::Paint) :
                                            document.rebuild(base, annotations, {facet}, TraceryEdit::Paint);
    TriangleMesh     current(first.mesh);
    TriangleSelector ordinary(current), support(current);
    ordinary.deserialize(first.annotations[0], false);
    ordinary.set_leaf_states([](const Vec3f& p, int) { return EnforcerBlockerType(p.x() < 4 ? 7 : 5); });
    support.set_leaf_states([](const Vec3f&, int) { return EnforcerBlockerType::BLOCKER; });
    auto edited             = first.annotations;
    edited[0]               = ordinary.serialize();
    edited[1]               = support.serialize();
    edited[6]               = {};
    auto             erased = document.rebuild(current, edited, {}, TraceryEdit::Erase);
    TriangleMesh     output(erased.mesh);
    TriangleSelector colours(output), supports(output);
    colours.deserialize(erased.annotations[0], false);
    supports.deserialize(erased.annotations[1], false);
    for (const auto& leaf : colours.painted_leaves(true)) {
        const Vec3f p = (leaf.vertices[0] + leaf.vertices[1] + leaf.vertices[2]) / 3.f;
        CHECK(unsigned(leaf.state) == (p.x() < 4 ? 7 : 5));
        CHECK(supports.state_at(p, leaf.source_facet) == EnforcerBlockerType::BLOCKER);
    }
}

TEST_CASE("Projected erase and material remapping use saved base colours", "[TraceryDocument][TraceryVector]")
{
    const bool vector_surface = GENERATE(false, true);    indexed_triangle_set its;
    its.vertices = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}};
    its.indices  = {{0, 1, 2}};
    TriangleMesh     base(its);
    TriangleSelector original(base);
    original.set_facet(0, EnforcerBlockerType(5));
    std::vector<TraceryDocument::Annotation> annotations(8);
    annotations[0] = annotations[5]               = original.serialize();
    auto                                 document = TraceryDocument::capture(base, annotations);
    TriangleSelector::TraceryFacet2DData facet;
    facet.source_facet_id = 0;
    TriangleSelector::TraceryRegion2D region;
    region.color   = EnforcerBlockerType(2);
    region.polygon = {{0, 0}, {10, 0}, {0, 10}};
    facet.regions.push_back(region);
    auto                    first = vector_surface ? document.apply_vector_edit(base, annotations, {facet}, TraceryEdit::Paint) :
                                            document.rebuild(base, annotations, {facet}, TraceryEdit::Paint);
    EnforcerBlockerStateMap mapping;
    for (size_t i = 0; i < mapping.size(); ++i)
        mapping[i] = EnforcerBlockerType(i);
    mapping[2] = EnforcerBlockerType(3);
    mapping[5] = EnforcerBlockerType(6);
    document.remap_materials(mapping);
    TriangleMesh     current(first.mesh);
    auto             erased = document.rebuild(current, document.last_annotations(), {facet}, TraceryEdit::Erase);
    TriangleMesh     output(erased.mesh);
    TriangleSelector colours(output);
    colours.deserialize(erased.annotations[0], false);
    for (const auto& leaf : colours.painted_leaves(true))
        CHECK(unsigned(leaf.state) == 6);
}

TEST_CASE("Sub-micron base facets retain their paint through project serialization", "[TraceryDocument]")
{
    indexed_triangle_set its;
    its.vertices = {{17.2507915f, 9.7820549f, 5.f}, {17.2342682f, 9.89667511f, 5.f}, {17.2413483f, 9.84755135f, 5.f}};
    its.indices  = {{0, 1, 2}};
    TriangleMesh     base(its);
    TriangleSelector paint(base);
    paint.set_facet(0, EnforcerBlockerType(5));
    std::vector<TraceryDocument::Annotation> annotations(8);
    annotations[0] = annotations[5] = paint.serialize();
    auto             document       = TraceryDocument::decode(TraceryDocument::capture(base, annotations).encode());
    auto             output         = document.rebuild(base, annotations, {}, TraceryEdit::Paint);
    TriangleMesh     mesh(output.mesh);
    TriangleSelector restored(mesh);
    restored.deserialize(output.annotations[0], false);
    for (const auto& leaf : restored.painted_leaves(true))
        CHECK(unsigned(leaf.state) == 5);
}

TEST_CASE("Repeated paint and erase do not accumulate derived topology", "[TraceryDocument]")
{
    indexed_triangle_set its;
    its.vertices = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}};
    its.indices  = {{0, 1, 2}};
    TriangleMesh                             current(its);
    std::vector<TraceryDocument::Annotation> annotations(8);
    auto                                     document    = TraceryDocument::capture(current, annotations);
    size_t                                   first_count = 0;
    for (int stroke = 0; stroke < 12; ++stroke) {
        TriangleSelector mask(current);
        mask.set_leaf_states([](const Vec3f&, int) { return EnforcerBlockerType(1); });
        annotations[6] = mask.serialize();
        TriangleSelector::Tracery2DData regions;
        for (size_t i = 0; i < current.its.indices.size(); ++i) {
            TriangleSelector::TraceryFacet2DData facet;
            facet.source_facet_id = int(i);
            TriangleSelector::TraceryRegion2D region;
            region.color   = EnforcerBlockerType(2);
            region.polygon = {{0, 0}, {4, 0}, {4, 6}, {0, 10}};
            facet.regions.push_back(region);
            regions.push_back(facet);
        }
        auto painted = document.rebuild(current, annotations, regions, TraceryEdit::Paint);
        if (stroke == 0)
            first_count = painted.mesh.indices.size();
        CHECK(painted.mesh.indices.size() == first_count);
        annotations    = painted.annotations;
        annotations[6] = {};
        auto erased    = document.rebuild(TriangleMesh(painted.mesh), annotations, {}, TraceryEdit::Erase);
        CHECK(erased.mesh.indices == its.indices);
        CHECK(document.base_mesh().its.indices == its.indices);
        current     = TriangleMesh(erased.mesh);
        annotations = erased.annotations;
    }
}

TEST_CASE("Material slot changes update colours hidden underneath stained glass", "[TraceryDocument]")
{
    indexed_triangle_set its;
    its.vertices = {{-5, -5, 0}, {5, -5, 0}, {-5, 5, 0}};
    its.indices  = {{0, 1, 2}};
    Model            model;
    auto*            volume = model.add_object()->add_volume(TriangleMesh(its));
    TriangleSelector base_colour(volume->mesh()), displayed_colour(volume->mesh());
    base_colour.set_facet(0, EnforcerBlockerType(5));
    displayed_colour.set_facet(0, EnforcerBlockerType(2));
    std::vector<TraceryDocument::Annotation> annotations(8);
    annotations[0] = displayed_colour.serialize();
    annotations[5] = base_colour.serialize();
    volume->mmu_segmentation_facets.set(displayed_colour);
    volume->tracery_edit_data = TraceryDocument::capture(volume->mesh(), annotations).encode();
    unsigned expected         = 0;
    SECTION("Deleting a hidden slot replaces its base colour")
    {
        volume->update_extruder_count_when_delete_filament(5, 5, 3);
        expected = 3;
    }
    SECTION("Clipping the material count clears hidden out-of-range slots") { volume->update_extruder_count(3); }
    SECTION("Reindexing also remaps hidden slots")
    {
        EnforcerBlockerStateMap mapping;
        for (size_t i = 0; i < mapping.size(); ++i)
            mapping[i] = EnforcerBlockerType(i);
        mapping[5] = EnforcerBlockerType(4);
        volume->remap_extruder_ids(5, mapping);
        expected = 4;
    }
    auto             document = TraceryDocument::decode(volume->tracery_edit_data);
    auto             result   = document.rebuild(volume->mesh(), document.last_annotations(), {}, TraceryEdit::Regenerate);
    TriangleMesh     mesh(result.mesh);
    TriangleSelector restored(mesh);
    restored.deserialize(result.annotations[0], false);
    for (const auto& leaf : restored.painted_leaves(true))
        CHECK(unsigned(leaf.state) == expected);
}

TEST_CASE("Sub-tolerance facets keep their dominant state without unstable internal cuts", "[TraceryDocument]")
{
    indexed_triangle_set its;
    its.vertices = {{0, 0, 0}, {1, 0, 0}, {0, 0.000001f, 0}};
    its.indices  = {{0, 1, 2}};
    TriangleMesh     base(its);
    TriangleSelector original(base);
    original.set_facet(0, EnforcerBlockerType(5));
    std::vector<TraceryDocument::Annotation> annotations(8);
    annotations[0] = annotations[5]               = original.serialize();
    auto                                 document = TraceryDocument::capture(base, annotations);
    TriangleSelector::TraceryFacet2DData facet;
    facet.source_facet_id = 0;
    TriangleSelector::TraceryRegion2D region;
    region.color   = EnforcerBlockerType(2);
    region.polygon = {{0, 0}, {.2, 0}, {0, .0000002}};
    facet.regions.push_back(region);
    auto result = document.rebuild(base, annotations, {facet}, TraceryEdit::Paint);
    REQUIRE(result.mesh.indices == its.indices);
    REQUIRE(result.mesh.vertices == its.vertices);
    TriangleSelector colours(base);
    colours.deserialize(result.annotations[0], false);
    CHECK(colours.painted_leaves().front().state == EnforcerBlockerType(5));
    // A full-facet colour change still applies; only sub-tolerance internal
    // boundaries are approximated, not the entire original facet discarded.
    facet.regions.front().polygon = {{0, 0}, {1, 0}, {0, .000001}};
    result                        = document.rebuild(base, result.annotations, {facet}, TraceryEdit::Paint);
    colours.deserialize(result.annotations[0], false);
    CHECK(colours.painted_leaves().front().state == EnforcerBlockerType(2));
    document = TraceryDocument::decode(document.encode());
    result   = document.rebuild(base, result.annotations, {}, TraceryEdit::Regenerate);
    colours.deserialize(result.annotations[0], false);
    CHECK(colours.painted_leaves().front().state == EnforcerBlockerType(5));
}

TEST_CASE("Diagnostic capture replays the complete erase input", "[TraceryDocument][TraceryReplay]")
{
    indexed_triangle_set its;
    its.vertices = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}};
    its.indices  = {{0, 1, 2}};
    TriangleMesh                             base(its);
    std::vector<TraceryDocument::Annotation> annotations(8);
    TriangleSelector                         mask(base);
    mask.set_facet(0, EnforcerBlockerType(1));
    auto                                 doc = TraceryDocument::capture(base, annotations);
    TriangleSelector::TraceryFacet2DData facet;
    facet.source_facet_id = 0;
    TriangleSelector::TraceryRegion2D region;
    region.color   = EnforcerBlockerType(2);
    region.polygon = {{0, 0}, {5, 0}, {0, 5}};
    facet.regions.push_back(region);
    annotations[6]      = mask.serialize();
    auto painted        = doc.rebuild(base, annotations, {facet}, TraceryEdit::Paint);
    auto after          = painted.annotations;
    after[6]            = {};
    const auto captured = doc.encode_replay(after, {}, TraceryEdit::Erase);
    auto       replayed = TraceryDocument::replay(captured);
    auto       direct   = doc.rebuild(TriangleMesh(painted.mesh), after, {}, TraceryEdit::Erase);
    CHECK(replayed.mesh.vertices == direct.mesh.vertices);
    CHECK(replayed.mesh.indices == direct.mesh.indices);
    CHECK(replayed.annotations == direct.annotations);
}

TEST_CASE("Replay a captured stained-glass operation", "[TraceryExternalReplay]")
{
    const char* path = std::getenv("TRACERY_REPLAY_FILE");
    if (!path || !*path)
        SKIP("No external capture specified");
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.good());
    const std::string data((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    const auto        result = TraceryDocument::replay(data);
    REQUIRE_FALSE(result.mesh.indices.empty());
}

TEST_CASE("Vector glass preserves source geometry and exact material boundaries", "[TraceryVector]")
{
    indexed_triangle_set its;
    its.vertices = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}};
    its.indices = {{0, 1, 2}};
    TriangleMesh mesh(its);
    std::vector<TraceryDocument::Annotation> annotations(8);
    auto doc = TraceryDocument::capture(mesh, annotations);
    TriangleSelector::TraceryFacet2DData facet;
    facet.source_facet_id = 0;
    TriangleSelector::TraceryRegion2D region;
    region.polygon = {{0, 0}, {4, 0}, {4, 6}, {0, 10}};
    region.color = EnforcerBlockerType(7);
    facet.regions.push_back(region);
    const auto result = doc.apply_vector_edit(mesh, annotations, {facet}, TraceryEdit::Paint);
    CHECK(result.mesh.vertices == its.vertices);
    CHECK(result.mesh.indices == its.indices);
    REQUIRE(doc.is_vector());
    const auto surfaces = doc.material_surfaces();
    REQUIRE(surfaces.count(7) == 1);
    double area = 0;
    for (const auto& t : surfaces.at(7).indices) {
        const auto& v = surfaces.at(7).vertices;
        area += .5 * (v[t[1]] - v[t[0]]).cross(v[t[2]] - v[t[0]]).norm();
        for (int id : t) CHECK(v[id].x() <= 4.00001f);
    }
    CHECK(std::abs(area - 32.0) < 0.00001);
    auto loaded = TraceryDocument::decode(doc.encode());
    CHECK(loaded.is_vector());
    CHECK(loaded.matches(mesh));
    CHECK(loaded.material_surfaces().at(7).vertices == surfaces.at(7).vertices);
    const auto cleared = loaded.apply_vector_edit(mesh, annotations, {}, TraceryEdit::Regenerate);
    CHECK(cleared.mesh.indices == its.indices);
    CHECK(loaded.material_surfaces().count(7) == 0);
}

TEST_CASE("Ordinary painting over vector glass survives clearing the effect", "[TraceryVector]")
{
    indexed_triangle_set its;
    its.vertices = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}};
    its.indices = {{0, 1, 2}};
    TriangleMesh mesh(its);
    TriangleSelector original(mesh);
    original.set_facet(0, EnforcerBlockerType(5));
    std::vector<TraceryDocument::Annotation> annotations(8);
    annotations[0] = annotations[5] = original.serialize();
    auto doc = TraceryDocument::capture(mesh, annotations);
    TriangleSelector::TraceryFacet2DData facet;
    facet.source_facet_id = 0;
    TriangleSelector::TraceryRegion2D paint;
    paint.polygon = {{0, 0}, {10, 0}, {0, 10}};
    paint.color = EnforcerBlockerType(7);
    facet.regions.push_back(paint);
    doc.apply_vector_edit(mesh, annotations, {facet}, TraceryEdit::Paint);
    // Same as the underlying paint: a state delta alone cannot detect this stroke.
    facet.regions[0].polygon = {{0, 0}, {3, 0}, {0, 3}};
    facet.regions[0].color = EnforcerBlockerType(5);
    doc.apply_ordinary_paint({facet});
    REQUIRE(doc.material_surfaces().count(5) == 1);
    REQUIRE(doc.material_surfaces().count(7) == 1);
    doc.clear_vector_glass(mesh, annotations);
    CHECK(doc.material_surfaces().count(7) == 0);
    CHECK(doc.material_surfaces().at(5).indices.size() == 1);
    auto restored = TraceryDocument::decode(doc.encode());
    CHECK(restored.material_surfaces().count(7) == 0);
    CHECK(restored.material_surfaces().count(5) == 1);
}

TEST_CASE("Clearing vector glass preserves pending ordinary annotations", "[TraceryVector]")
{
    indexed_triangle_set its;
    its.vertices = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}};
    its.indices = {{0, 1, 2}};
    TriangleMesh mesh(its);
    std::vector<TraceryDocument::Annotation> annotations(8);
    auto doc = TraceryDocument::capture(mesh, annotations);
    TriangleSelector::TraceryFacet2DData facet;
    facet.source_facet_id = 0;
    TriangleSelector::TraceryRegion2D paint;
    paint.polygon = {{0, 0}, {10, 0}, {0, 10}};
    paint.color = EnforcerBlockerType(7);
    facet.regions.push_back(paint);
    doc.apply_vector_edit(mesh, annotations, {facet}, TraceryEdit::Paint);

    TriangleSelector color(mesh);
    color.set_facet(0, EnforcerBlockerType(5));
    annotations[0] = color.serialize();
    TriangleSelector other_annotation(mesh);
    other_annotation.set_facet(0, EnforcerBlockerType::ENFORCER);
    annotations[1] = other_annotation.serialize();
    doc.set_selection_reference(other_annotation.serialize());
    doc.clear_vector_glass(mesh, annotations);
    const auto restored = TraceryDocument::decode(doc.encode());
    CHECK(restored.material_surfaces().count(7) == 0);
    CHECK(restored.material_surfaces().count(5) == 1);
    CHECK(restored.last_annotations()[1] == annotations[1]);
    CHECK(restored.last_annotations()[6].triangles_to_split.empty());
    CHECK(restored.selection_reference().triangles_to_split.empty());
}

TEST_CASE("Vector-only flower regions reach the normal slicer without baking paint", "[TraceryVector]")
{
    Model model;
    auto* object = model.add_object();
    auto* volume = object->add_volume(make_cube(10, 10, 1));
    object->add_instance();
    const auto original = volume->mesh().its;
    std::vector<TraceryDocument::Annotation> annotations(8);
    auto doc = TraceryDocument::capture(volume->mesh(), annotations);
    TriangleSelector::Tracery2DData regions;
    const float top_z = volume->mesh().bounding_box().max.z();
    const float left = volume->mesh().bounding_box().min.x();
    const float bottom = volume->mesh().bounding_box().min.y();
    for (size_t i = 0; i < original.indices.size(); ++i) {
        const auto& t = original.indices[i];
        if (original.vertices[t[0]].z() != top_z || original.vertices[t[1]].z() != top_z || original.vertices[t[2]].z() != top_z)
            continue;
        TriangleSelector::TraceryFacet2DData facet;
        facet.source_facet_id = int(i);
        facet.origin = Vec3d(left, bottom, top_z);
        TriangleSelector::TraceryRegion2D region;
        region.polygon = {{0, 0}, {3.17, 0}, {6.17, 10}, {0, 10}};
        region.color = EnforcerBlockerType(2);
        facet.regions.push_back(region);
        regions.push_back(facet);
    }
    doc.apply_vector_edit(volume->mesh(), annotations, regions, TraceryEdit::Paint);
    volume->tracery_edit_data = doc.encode();
    REQUIRE(volume->mmu_segmentation_facets.empty());
    REQUIRE(volume->is_mm_painted());
    CHECK(volume->vector_material_ids() == std::vector<unsigned>{2});
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
    auto* printed = print.get_object(0);
    printed->slice();
    REQUIRE(printed->layer_count() > 0);
    const Layer* top = printed->get_layer(int(printed->layer_count()) - 1);
    const Transform3d inverse = (printed->trafo_centered() * volume->get_matrix()).inverse();
    size_t edges = 0;
    for (const auto* region : top->regions()) {
        if (region->region().config().wall_filament.value != 2) continue;
        for (const auto& surface : region->slices.surfaces) {
            const auto& points = surface.expolygon.contour.points;
            for (size_t i = 0; i < points.size(); ++i) {
                const Point p = (points[i] + points[(i + 1) % points.size()]) / 2;
                const Vec3d local = inverse * Vec3d(unscale<double>(p.x()), unscale<double>(p.y()), top->print_z);
                const double x = local.x() - left, y = local.y() - bottom;
                if (x < 1 || x > 9 || y < 1 || y > 9) continue;
                ++edges;
                INFO("local=" << local.transpose() << " origin=" << left << "," << bottom << " offset=" << volume->source.mesh_offset.transpose());
                CHECK(std::abs(x - 3.17 - 0.3 * y) < 0.01);
            }
        }
    }
    CHECK(edges > 0);
    CHECK(volume->mesh().its.indices == original.indices);
    CHECK(volume->mesh().its.vertices == original.vertices);
    CHECK(volume->mmu_segmentation_facets.empty());
}

TEST_CASE("A brush records its footprint even when it repaints the same material", "[TraceryVector]")
{
    indexed_triangle_set its;
    its.vertices = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}};
    its.indices = {{0, 1, 2}};
    TriangleMesh mesh(its);
    TriangleSelector brush(mesh);
    brush.set_facet(0, EnforcerBlockerType(5));
    brush.record_painting(true);
    const Vec3f center(2, 2, 0);
    auto cursor = TriangleSelector::SinglePointCursor::cursor_factory(
        center, center + Vec3f(0, 0, 100), 1.f, TriangleSelector::CursorType::CIRCLE,
        Transform3d::Identity(), TriangleSelector::ClippingPlane());
    brush.select_patch(0, std::move(cursor), EnforcerBlockerType(5), Transform3d::Identity(), true);
    const auto paint = brush.take_recorded_paint();
    REQUIRE_FALSE(paint.empty());
    double area = 0;
    for (const auto& facet : paint) for (const auto& region : facet.regions) {
        CHECK(region.color == EnforcerBlockerType(5));
        const auto& p = region.polygon;
        area += .5 * std::abs((p[1].x()-p[0].x())*(p[2].y()-p[0].y()) - (p[1].y()-p[0].y())*(p[2].x()-p[0].x()));
    }
    CHECK(area > 2.5);
    CHECK(area < 3.5);
    CHECK(brush.take_recorded_paint().empty());
    CHECK(mesh.its.indices == its.indices);
}

TEST_CASE("Successive glass strokes preserve the earlier selection footprint", "[TraceryVector]")
{
    indexed_triangle_set its;
    its.vertices = {{0, 0, 0}, {20, 0, 0}, {0, 20, 0}};
    its.indices = {{0, 1, 2}};
    TriangleMesh mesh(its);
    TriangleSelector material(mesh), mask(mesh);
    material.preserve_subdivision(true);
    const auto marker = EnforcerBlockerType::ExtruderMax;
    const auto paint = [&](const Vec3f& center, float radius) {
        auto cursor = TriangleSelector::SinglePointCursor::cursor_factory(
            center, center + Vec3f(0, 0, 100), radius, TriangleSelector::CursorType::CIRCLE,
            Transform3d::Identity(), TriangleSelector::ClippingPlane());
        material.select_patch(0, std::move(cursor), marker, Transform3d::Identity(), true);
        TriangleSelector previous(mesh);
        previous.deserialize(mask.serialize());
        mask.deserialize(material.serialize());
        mask.set_leaf_states([&](const Vec3f& point, int facet) {
            return material.state_at(point, facet) == marker ? EnforcerBlockerType::ENFORCER : previous.state_at(point, facet);
        });
        material.set_leaf_states({});
    };
    paint(Vec3f(3, 3, 0), 1.f);
    const auto first = mask.serialize();
    paint(Vec3f(12, 3, 0), 1.f);
    TriangleSelector original(mesh);
    original.deserialize(first);
    for (float x = .125f; x < 8; x += .25f)
        for (float y = .125f; y < 8; y += .25f)
            CHECK(mask.state_at(Vec3f(x, y, 0), 0) == original.state_at(Vec3f(x, y, 0), 0));
    CHECK(mask.state_at(Vec3f(12, 3, 0), 0) == EnforcerBlockerType::ENFORCER);
    // A larger overlapping stroke must also retain the shared leaf topology.
    paint(Vec3f(12, 3, 0), 2.f);
    CHECK(mask.state_at(Vec3f(3, 3, 0), 0) == EnforcerBlockerType::ENFORCER);
    CHECK(mask.state_at(Vec3f(6, 3, 0), 0) == EnforcerBlockerType::NONE);
}
