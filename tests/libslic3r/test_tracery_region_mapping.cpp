#include <catch2/catch_test_macros.hpp>
#include "libslic3r/TraceryRegionMapping.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include <algorithm>
#include <map>
#include <chrono>
#include <iostream>
#include <numeric>

using namespace Slic3r;
namespace {
TraceryPartitionResult subdivided_triangle(int side)
{
    TraceryPartitionResult partition;
    for (int y = 0; y <= side; ++y)
        for (int x = 0; x <= side; ++x)
            partition.mesh.vertices.emplace_back(float(x), float(y), 0.f);
    for (int y = 0; y < side; ++y)
        for (int x = 0; x + y < side; ++x) {
            const int a = y * (side + 1) + x;
            partition.mesh.indices.emplace_back(a, a + 1, a + side + 1);
            if (x + y + 1 < side)
                partition.mesh.indices.emplace_back(a + 1, a + side + 2, a + side + 1);
        }
    partition.source_facets.assign(partition.mesh.indices.size(), 0);
    return partition;
}
indexed_triangle_set source_triangle(float side)
{
    indexed_triangle_set source;
    source.vertices = {{0, 0, 0}, {side, 0, 0}, {0, side, 0}};
    source.indices = {{0, 1, 2}};
    return source;
}
Polygons planar(const std::vector<Vec3f>& vertices)
{
    Polygon p;
    for (const auto& v : vertices) p.points.emplace_back(scale_(double(v.x())), scale_(double(v.y())));
    p.make_counter_clockwise();
    return {std::move(p)};
}
void check_reference(const TraceryPartitionResult& partition, int source, const std::vector<Vec3f>& region,
                     const std::vector<TraceryMappedRegion>& actual)
{
    std::map<int, std::vector<Vec3f>> expected;
    for (size_t i = 0; i < partition.mesh.indices.size(); ++i) {
        if (partition.source_facets[i] != source) continue;
        auto p = clip_tracery_polygon_to_facet(partition.mesh, int(i), region);
        if (p.size() >= 3) expected.emplace(int(i), std::move(p));
    }
    REQUIRE(actual.size() == expected.size());
    for (const auto& result : actual) {
        REQUIRE(expected.count(result.facet) == 1);
        const auto a = planar(result.polygon), b = planar(expected.at(result.facet));
        CHECK(diff_ex(a, b).empty());
        CHECK(diff_ex(b, a).empty());
        expected.erase(result.facet);
    }
}
}
TEST_CASE("Local tracery mapping preserves coverage without clipping distant children", "[TraceryMapping]")
{
    auto source = source_triangle(40);
    auto partition = subdivided_triangle(40);
    TraceryRegionMapper mapper(source, partition);
    const std::vector<Vec3f> region{{3.2f, 4.2f, 0}, {4.3f, 4.2f, 0}, {4.3f, 5.3f, 0}, {3.2f, 5.3f, 0}};
    TraceryMappingStatistics statistics;
    const auto actual = mapper.map(0, region, statistics);
    check_reference(partition, 0, region, actual);
    CHECK(statistics.spatial_candidates < partition.mesh.indices.size() / 10);
    CHECK(statistics.clip_calls < partition.mesh.indices.size() / 10);
}

TEST_CASE("Whole child and contained region mappings inherit exact coverage", "[TraceryMapping]")
{
    auto source = source_triangle(8);
    auto partition = subdivided_triangle(8);
    TraceryRegionMapper mapper(source, partition);
    TraceryMappingStatistics whole;
    const std::vector<Vec3f> all{{0,0,0}, {8,0,0}, {0,8,0}};
    check_reference(partition, 0, all, mapper.map(0, all, whole));
    CHECK(whole.clip_calls == 0);
    CHECK(whole.inherited_facets + whole.inherited_regions == partition.mesh.indices.size());
    TraceryMappingStatistics contained;
    const std::vector<Vec3f> small{{0.1f,0.1f,0}, {0.3f,0.1f,0}, {0.1f,0.3f,0}};
    check_reference(partition, 0, small, mapper.map(0, small, contained));
    CHECK(contained.inherited_regions == 1);
    CHECK(contained.clip_calls == 0);
}

TEST_CASE("Mapping keeps boundary slivers and rejects line-only contacts", "[TraceryMapping]")
{
    auto source = source_triangle(12);
    auto partition = subdivided_triangle(12);
    TraceryRegionMapper mapper(source, partition);
    for (float offset : {-0.00002f, -0.000002f, 0.f, 0.000002f, 0.00002f}) {
        const std::vector<Vec3f> region{{3.f+offset,2.f,0}, {4.f+offset,2.f,0}, {4.f+offset,3.f,0}, {3.f+offset,3.f,0}};
        TraceryMappingStatistics stats;
        check_reference(partition, 0, region, mapper.map(0, region, stats));
    }
    const std::vector<Vec3f> line{{2,2,0}, {3,2,0}, {4,2,0}};
    TraceryMappingStatistics stats;
    CHECK(mapper.map(0, line, stats).empty());
}

TEST_CASE("Spatial mapping handles tilted float children and off-plane projection", "[TraceryMapping]")
{
    auto source = source_triangle(16);
    auto partition = subdivided_triangle(16);
    const auto transform = [](Vec3f p) -> Vec3f {
        return Vec3f(p.x() + 0.17f*p.y()+200.f, 0.81f*p.y()-50.f, 0.42f*p.x()+0.23f*p.y()+20.f);
    };
    for (auto& v : source.vertices) v = transform(v);
    for (auto& v : partition.mesh.vertices) v = transform(v);
    TraceryRegionMapper mapper(source, partition);
    for (int i = 0; i < 20; ++i) {
        const float x = 0.31f * i, y = 0.23f * i;
        std::vector<Vec3f> region{{x,y,0}, {x+0.7f,y,0}, {x+0.7f,y+1.3f,0}, {x,y+1.3f,0}};
        for (auto& v : region) {
            v = transform(v);
            if (i % 2) v.z() += 0.01f;
        }
        TraceryMappingStatistics stats;
        check_reference(partition, 0, region, mapper.map(0, region, stats));
    }
}

TEST_CASE("Mapping does not connect coincident disconnected source shells", "[TraceryMapping]")
{
    auto source = source_triangle(10);
    source.vertices.insert(source.vertices.end(), {Vec3f(0,0,0), Vec3f(10,0,0), Vec3f(0,10,0)});
    source.indices.emplace_back(3,4,5);
    auto partition = subdivided_triangle(10);
    const auto count = partition.mesh.indices.size();
    for (size_t i = 0; i < count; ++i) {
        partition.mesh.indices.push_back(partition.mesh.indices[i]);
        partition.source_facets.push_back(1);
    }
    TraceryRegionMapper mapper(source, partition);
    const std::vector<Vec3f> region{{1.2f,2.3f,0}, {3.1f,2.3f,0}, {1.2f,4.1f,0}};
    for (int id : {0,1}) {
        TraceryMappingStatistics stats;
        const auto mapped = mapper.map(id, region, stats);
        check_reference(partition, id, region, mapped);
        for (const auto& child : mapped) CHECK(partition.source_facets[child.facet] == id);
    }
}

TEST_CASE("Unchanged faces and repeated history queries retain their own geometry", "[TraceryMapping]")
{
    auto source = source_triangle(8);
    TraceryPartitionResult partition;
    partition.mesh = source;
    partition.source_facets = {0};
    TraceryRegionMapper mapper(source, partition);
    const std::vector<std::vector<Vec3f>> regions{
        {{0,0,0},{8,0,0},{0,8,0}},
        {{1,1,0},{2,1,0},{1,2,0}},
        {{3,1,0},{4,1,0},{3,2,0}},
        {{1,1,0},{2,1,0},{1,2,0}}};
    for (int repeat = 0; repeat < 3; ++repeat)
        for (const auto& region : regions) {
            TraceryMappingStatistics stats;
            check_reference(partition, 0, region, mapper.map(0, region, stats));
            CHECK(stats.inherited_regions == 1);
            CHECK(stats.clip_calls == 0);
        }
}

TEST_CASE("Concave and thin input uses the same clipping coverage as the legacy mapper", "[TraceryMapping]")
{
    auto source = source_triangle(10);
    auto partition = subdivided_triangle(10);
    // A thin but nondegenerate output mesh exercises adaptive clip coordinates.
    for (auto& v : source.vertices) v.y() *= 0.0001f;
    for (auto& v : partition.mesh.vertices) v.y() *= 0.0001f;
    TraceryRegionMapper mapper(source, partition);
    std::vector<Vec3f> region{{1,1,0},{5,1,0},{5,2,0},{2,2,0},{2,5,0},{1,5,0}};
    for (auto& v : region) v.y() *= 0.0001f;
    TraceryMappingStatistics stats;
    check_reference(partition, 0, region, mapper.map(0, region, stats));
    CHECK(stats.clip_calls > 0);
}

TEST_CASE("Repeated overlapping regions stay equivalent across successive partitions", "[TraceryMapping]")
{
    TriangleMesh source(source_triangle(12));
    TriangleSelector paint(source);
    paint.set_facet(0, EnforcerBlockerType(2));
    TriangleSelector::TraceryFacet2DData plane;
    plane.source_facet_id = 0;
    plane.origin = Vec3d::Zero();
    plane.axis_u = Vec3d::UnitX();
    plane.axis_v = Vec3d::UnitY();
    TriangleSelector::TraceryRegion2D region;
    region.fragment_id = 3;
    region.color = EnforcerBlockerType(3);
    region.polygon = {{1,1},{7,1},{1,7}};
    plane.regions.push_back(region);
    const auto partition = build_exact_tracery_partition(source, {paint.serialize()}, {plane});
    TraceryRegionMapper mapper(source.its, partition);
    const std::vector<Vec3f> first{{1,1,0},{7,1,0},{1,7,0}};
    const std::vector<Vec3f> overlap{{2,1,0},{8,1,0},{2,6,0}};
    TraceryMappingStatistics stats;
    const auto mapped = mapper.map(0, first, stats);
    check_reference(partition, 0, first, mapped);
    check_reference(partition, 0, overlap, mapper.map(0, overlap, stats));
    TriangleMesh next_source(partition.mesh);
    const auto next = build_exact_tracery_partition(next_source, partition.annotations, {});
    TraceryRegionMapper history(next_source.its, next);
    for (const auto& input : mapped) {
        TraceryMappingStatistics next_stats;
        check_reference(next, input.facet, input.polygon, history.map(input.facet, input.polygon, next_stats));
        CHECK(next_stats.clip_calls == 0);
    }
}

TEST_CASE("Profile dense local mapping against exhaustive clipping", "[TraceryMappingPerformance]")
{
    auto source = source_triangle(120);
    auto partition = subdivided_triangle(120);
    std::vector<std::vector<Vec3f>> regions;
    for (int i = 0; i < 600; ++i) {
        const float x = float((i * 37) % 80) + 0.13f, y = float((i * 23) % 30) + 0.17f;
        regions.push_back({{x,y,0}, {x+0.37f,y,0}, {x+0.37f,y+0.61f,0}, {x,y+0.61f,0}});
    }
    using Clock = std::chrono::steady_clock;
    struct Result { size_t count = 0; size_t facets = 0; };
    const auto baseline = [&] {
        Result result;
        for (const auto& region : regions)
            for (size_t face = 0; face < partition.mesh.indices.size(); ++face) {
                const auto p = clip_tracery_polygon_to_facet(partition.mesh, int(face), region);
                if (p.size() >= 3) { ++result.count; result.facets += face; }
            }
        return result;
    };
    TraceryMappingStatistics stats;
    const auto optimized = [&] {
        TraceryRegionMapper mapper(source, partition); // Include index construction.
        Result result;
        stats = {};
        for (const auto& region : regions)
            for (const auto& mapped : mapper.map(0, region, stats)) {
                ++result.count;
                result.facets += size_t(mapped.facet);
            }
        return result;
    };
    std::vector<double> old_times, new_times;
    for (int repeat = 0; repeat < 3; ++repeat) {
        const auto before = Clock::now();
        const auto old = baseline();
        const auto middle = Clock::now();
        const auto current = optimized();
        const auto after = Clock::now();
        REQUIRE(old.count == current.count);
        REQUIRE(old.facets == current.facets);
        old_times.push_back(std::chrono::duration<double, std::milli>(middle-before).count());
        new_times.push_back(std::chrono::duration<double, std::milli>(after-middle).count());
    }
    std::sort(old_times.begin(), old_times.end());
    std::sort(new_times.begin(), new_times.end());
    CHECK(stats.spatial_candidates < stats.candidate_pairs / 100);
    std::cout << "TRACERY_MAPPING_PERF facets=" << partition.mesh.indices.size() << " regions=" << regions.size()
              << " baseline_median_ms=" << old_times[1] << " optimized_median_ms=" << new_times[1]
              << " candidate_pairs=" << stats.candidate_pairs << " spatial_candidates=" << stats.spatial_candidates
              << " clip_calls=" << stats.clip_calls << " inherited_regions=" << stats.inherited_regions
              << " inherited_facets=" << stats.inherited_facets << '\n';
}
