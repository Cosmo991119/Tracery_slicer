#include <catch2/catch_test_macros.hpp>
#include "libslic3r/TraceryVoronoi.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>

using namespace Slic3r;

namespace {
struct Fragment {
    size_t triangle, seed;
    std::vector<TraceryVoronoiVertex> polygon;
};

// Independent copy of the previous GUI algorithm, including plane order,
// float interpolation and degenerate/bisector cases. Kept as a differential
// oracle so the optimized code cannot silently change shared boundaries.
std::vector<Fragment> reference(const std::vector<TraceryVoronoiTriangle>& triangles,
                                const std::vector<Vec3f>& seeds, const std::vector<size_t>& indices)
{
    std::map<std::pair<size_t, size_t>, std::pair<Vec3d, double>> bisectors;
    for (size_t a = 0; a < indices.size(); ++a)
        for (size_t b = a + 1; b < indices.size(); ++b) {
            const auto key = std::minmax(indices[a], indices[b]);
            const Vec3d first = seeds[key.first].cast<double>(), second = seeds[key.second].cast<double>();
            const Vec3d direction = second - first;
            bisectors.emplace(key, std::make_pair(direction, direction.dot((first + second) * 0.5)));
        }
    std::vector<Fragment> result;
    for (size_t i = 0; i < triangles.size(); ++i)
        for (size_t seed : indices) {
            std::vector<TraceryVoronoiVertex> polygon(triangles[i].begin(), triangles[i].end());
            for (size_t other : indices) {
                if (other == seed || polygon.empty()) continue;
                const auto& boundary = bisectors.at(std::minmax(seed, other));
                const double side = seed < other ? 1. : -1.;
                const Vec3d direction = side * boundary.first;
                const double limit = side * boundary.second;
                std::vector<TraceryVoronoiVertex> output;
                output.reserve(polygon.size() + 1);
                for (size_t j = 0; j < polygon.size(); ++j) {
                    const auto& first = polygon[j];
                    const auto& second = polygon[(j + 1) % polygon.size()];
                    const double a = first.warped.cast<double>().dot(direction) - limit;
                    const double b = second.warped.cast<double>().dot(direction) - limit;
                    if (a <= 0.) output.push_back(first);
                    if ((a <= 0.) != (b <= 0.)) {
                        const float fraction = float(std::clamp(a / (a - b), 0., 1.));
                        output.push_back({first.local + fraction * (second.local - first.local),
                                          first.world + fraction * (second.world - first.world),
                                          first.warped + fraction * (second.warped - first.warped)});
                    }
                }
                polygon = std::move(output);
            }
            if (polygon.size() >= 3)
                result.push_back({i, seed, std::move(polygon)});
        }
    return result;
}

std::vector<Fragment> optimized(const std::vector<TraceryVoronoiTriangle>& triangles,
                                const std::vector<Vec3f>& seeds, const std::vector<size_t>& indices,
                                TraceryVoronoiStatistics& statistics)
{
    TraceryPerformanceScope performance("voronoi_test");
    TraceryVoronoiClipper clipper(seeds, indices);
    std::vector<Fragment> result;
    for (size_t start = 0; start < triangles.size(); start += 4096) {
        const size_t end = std::min(start + 4096, triangles.size());
        const std::vector<TraceryVoronoiTriangle> batch(triangles.begin() + start, triangles.begin() + end);
        clipper.generate_batch(batch, [&](size_t i, size_t seed, const auto& polygon) {
            result.push_back({start + i, seed, {polygon.begin(), polygon.end()}});
        }, statistics, performance);
    }
    return result;
}

void require_identical(const std::vector<Fragment>& expected, const std::vector<Fragment>& actual)
{
    REQUIRE(actual.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        REQUIRE(actual[i].triangle == expected[i].triangle);
        REQUIRE(actual[i].seed == expected[i].seed);
        REQUIRE(actual[i].polygon.size() == expected[i].polygon.size());
        for (size_t j = 0; j < expected[i].polygon.size(); ++j) {
            REQUIRE(actual[i].polygon[j].local == expected[i].polygon[j].local);
            REQUIRE(actual[i].polygon[j].world == expected[i].polygon[j].world);
            REQUIRE(actual[i].polygon[j].warped == expected[i].polygon[j].warped);
        }
    }
}
TraceryVoronoiTriangle triangle(const Vec3f& a, const Vec3f& b, const Vec3f& c)
{
    return {{{a, a, a}, {b, b, b}, {c, c, c}}};
}
} // namespace

TEST_CASE("Voronoi fast path preserves ordered polygons and float intersections", "[TraceryVoronoi]")
{
    std::mt19937 random(94061);
    std::uniform_real_distribution<float> value(-10.f, 10.f);
    for (int trial = 0; trial < 8; ++trial) {
        CAPTURE(trial);
        std::vector<Vec3f> seeds;
        for (int i = 0; i < 7; ++i)
            seeds.emplace_back(value(random), value(random), value(random));
        if (trial == 1) seeds[2] = seeds[3];
        std::vector<size_t> indices = trial == 2 ? std::vector<size_t>{} :
                                      trial == 3 ? std::vector<size_t>{5} : std::vector<size_t>{3, 0, 5, 2, 6};
        std::vector<TraceryVoronoiTriangle> triangles;
        for (int i = 0; i < 500; ++i) {
            const Vec3f a(value(random), value(random), value(random));
            const float size = i % 2 ? .0001f : 10.f;
            triangles.push_back(triangle(a, a + size * Vec3f(1, .2f, .3f), a + size * Vec3f(.4f, 1, -.2f)));
        }
        if (trial >= 4) {
            const Vec3f offset(10000, -20000, 50000);
            for (auto& seed : seeds) seed += offset;
            for (auto& t : triangles)
                for (auto& vertex : t) {
                    vertex.world = vertex.local.cwiseProduct(Vec3f(2, 3, .5f)) + offset;
                    vertex.warped += offset;
                }
        }
        TraceryVoronoiStatistics statistics;
        require_identical(reference(triangles, seeds, indices), optimized(triangles, seeds, indices, statistics));
    }
}

TEST_CASE("Voronoi boundary ties remain on the exact clipping path", "[TraceryVoronoi]")
{
    const std::vector<Vec3f> seeds{{-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
    const std::vector<size_t> indices{0, 1, 2, 3};
    const std::vector<TraceryVoronoiTriangle> triangles{
        triangle({0, 0, 0}, {0, 0, 1}, {0, 0, 2}),
        triangle({-.01f, 0, 0}, {.01f, 0, 0}, {0, .01f, 0}),
        triangle({-1e-8f, 0, 0}, {1e-8f, 0, 0}, {0, 0, 1})};
    TraceryVoronoiStatistics statistics;
    require_identical(reference(triangles, seeds, indices), optimized(triangles, seeds, indices, statistics));
    CHECK(statistics.contained_triangles == 0);
}

TEST_CASE("Fine Voronoi triangles bypass repeated cell clipping", "[TraceryVoronoi][TraceryVoronoiPerformance]")
{
    const char* setting = std::getenv("TRACERY_VORONOI_BENCH_GRID");
    const int n = setting ? std::clamp(std::atoi(setting), 64, 512) : 64;
    std::vector<Vec3f> seeds;
    std::vector<size_t> indices;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            indices.push_back(seeds.size());
            seeds.emplace_back((x + .5f) / 4, (y + .5f) / 4, 0);
        }
    std::vector<TraceryVoronoiTriangle> triangles;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const Vec3f a(float(x) / n, float(y) / n, 0), b(float(x + 1) / n, float(y) / n, 0);
            const Vec3f c(float(x + 1) / n, float(y + 1) / n, 0), d(float(x) / n, float(y + 1) / n, 0);
            triangles.push_back(triangle(a, b, c));
            triangles.push_back(triangle(a, c, d));
        }
    const auto begin = std::chrono::steady_clock::now();
    const auto expected = reference(triangles, seeds, indices);
    const auto middle = std::chrono::steady_clock::now();
    TraceryVoronoiStatistics statistics;
    const auto actual = optimized(triangles, seeds, indices, statistics);
    const auto end = std::chrono::steady_clock::now();
    std::fprintf(stderr, "TRACERY_VORONOI_BENCH triangles=%zu seeds=%zu reference_ms=%.3f optimized_ms=%.3f contained=%zu boundary=%zu clips=%zu\n",
                 triangles.size(), seeds.size(), std::chrono::duration<double, std::milli>(middle - begin).count(),
                 std::chrono::duration<double, std::milli>(end - middle).count(), statistics.contained_triangles,
                 statistics.boundary_triangles, statistics.clip_calls);
    require_identical(expected, actual);
    CHECK(statistics.contained_triangles > triangles.size() * 3 / 4);
    CHECK(statistics.candidate_cells < statistics.potential_cells / 4);
}
