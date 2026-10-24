#include "TracerySurfaceSampling.hpp"
#include <boost/geometry.hpp>
#include <boost/geometry/index/rtree.hpp>
#include <algorithm>
#include <cmath>
#include <map>

namespace Slic3r {
size_t tracery_shared_edges(const std::vector<TracerySurfaceEdge>& edges, const std::function<void(size_t, size_t)>& connect)
{
    namespace bg                                                      = boost::geometry;
    namespace bgi                                                     = boost::geometry::index;
    using Point                                                       = bg::model::point<double, 3, bg::cs::cartesian>;
    using Box                                                         = bg::model::box<Point>;
    using Entry                                                       = std::pair<Box, size_t>;
    constexpr double                                        tolerance = 1e-5;
    std::map<size_t, bgi::rtree<Entry, bgi::quadratic<16>>> trees;
    std::vector<Entry>                                      candidates;
    size_t                                                  comparisons = 0;
    for (size_t i = 0; i < edges.size(); ++i) {
        const auto&  edge   = edges[i];
        const Vec3d  delta  = edge.second - edge.first;
        const double length = delta.norm();
        if (length <= tolerance || !edge.first.allFinite() || !edge.second.allFinite())
            continue;
        const Vec3d axis = delta / length;
        const Vec3d lo   = edge.first.cwiseMin(edge.second).array() - tolerance;
        const Vec3d hi   = edge.first.cwiseMax(edge.second).array() + tolerance;
        const Box   box(Point(lo.x(), lo.y(), lo.z()), Point(hi.x(), hi.y(), hi.z()));
        auto&       tree = trees[edge.volume];
        candidates.clear();
        tree.query(bgi::intersects(box), std::back_inserter(candidates));
        for (const auto& entry : candidates) {
            const auto& other = edges[entry.second];
            if (other.owner == edge.owner)
                continue;
            ++comparisons;
            const Vec3d a = other.first - edge.first, b = other.second - edge.first;
            if (a.cross(axis).squaredNorm() > tolerance * tolerance || b.cross(axis).squaredNorm() > tolerance * tolerance)
                continue;
            const double x = a.dot(axis), y = b.dot(axis);
            if (std::min(length, std::max(x, y)) - std::max(0., std::min(x, y)) > tolerance)
                connect(edge.owner, other.owner);
        }
        tree.insert({box, i});
    }
    return comparisons;
}

std::vector<std::pair<Vec3d, Vec3d>> tracery_exposed_edges(const std::vector<TracerySurfaceEdge>& edges)
{
    auto indexed = edges;
    for (size_t i = 0; i < indexed.size(); ++i)
        indexed[i].owner = i;
    std::vector<std::vector<std::pair<double, double>>> covered(edges.size());
    const auto                                          overlap = [&](size_t a, size_t b) {
        const auto&  edge   = edges[a];
        const Vec3d  delta  = edge.second - edge.first;
        const double length = delta.squaredNorm();
        double       first  = (edges[b].first - edge.first).dot(delta) / length;
        double       second = (edges[b].second - edge.first).dot(delta) / length;
        if (second < first)
            std::swap(first, second);
        covered[a].emplace_back(std::max(0., first), std::min(1., second));
    };
    tracery_shared_edges(indexed, [&](size_t a, size_t b) {
        overlap(a, b);
        overlap(b, a);
    });
    std::vector<std::pair<Vec3d, Vec3d>> result;
    for (size_t i = 0; i < edges.size(); ++i) {
        auto& intervals = covered[i];
        std::sort(intervals.begin(), intervals.end());
        const auto& edge  = edges[i];
        const Vec3d delta = edge.second - edge.first;
        const auto  emit  = [&](double a, double b) {
            if ((b - a) * delta.norm() > 1e-5)
                result.emplace_back(edge.first + a * delta, edge.first + b * delta);
        };
        double end = 0.;
        for (const auto& interval : intervals) {
            emit(end, interval.first);
            end = std::max(end, interval.second);
        }
        emit(end, 1.);
    }
    return result;
}

std::vector<Vec3f> tracery_area_candidates(const std::vector<std::array<Vec3f, 3>>& triangles, size_t seed_budget)
{
    std::vector<double> cumulative;
    cumulative.reserve(triangles.size());
    double area = 0.;
    for (const auto& t : triangles) {
        const double value = (t[1].cast<double>() - t[0].cast<double>()).cross(t[2].cast<double>() - t[0].cast<double>()).norm();
        if (std::isfinite(value))
            area += value;
        cumulative.push_back(area);
    }
    if (!(area > 0.) || seed_budget == 0)
        return {};
    const size_t       count = std::max<size_t>(32, std::min<size_t>(seed_budget, 2048) * 16);
    std::vector<Vec3f> result;
    result.reserve(count);
    const auto radical_inverse = [](size_t index, size_t base) {
        double value = 0., fraction = 1.;
        while (index) {
            fraction /= base;
            value += (index % base) * fraction;
            index /= base;
        }
        return value;
    };
    for (size_t i = 0; i < count; ++i) {
        const double target = area * (double(i) + .5) / double(count);
        const size_t index  = std::min<size_t>(std::upper_bound(cumulative.begin(), cumulative.end(), target) - cumulative.begin(),
                                               triangles.size() - 1);
        const auto&  t      = triangles[index];
        const double root   = std::sqrt(radical_inverse(i + 1, 2));
        const double v      = radical_inverse(i + 1, 3);
        result.push_back(
            ((1. - root) * t[0].cast<double>() + root * (1. - v) * t[1].cast<double>() + root * v * t[2].cast<double>()).cast<float>());
    }
    return result;
}
} // namespace Slic3r
