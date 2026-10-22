#ifndef slic3r_TraceryPolygonPreparation_hpp_
#define slic3r_TraceryPolygonPreparation_hpp_

#include "Polygon.hpp"
#include <algorithm>
#include <cstdlib>
#include <vector>

namespace Slic3r {

// Reduce a temporary set of same-material paths before its non-zero union.
// Exact opposite edges cancel in the winding field; no snapping, fitting or
// tolerance is used. Keep hole orientation. Ambiguous junctions/multiplicity
// fall back to the original paths, which the regular boolean engine handles.
inline bool tracery_cancel_internal_edges(Polygons& polygons)
{
    if (polygons.size() < 64)
        return false;
    size_t edge_count = 0;
    for (const auto& polygon : polygons)
        edge_count += polygon.points.size();
    // Bound temporary memory. Large irregular batches still use the established
    // union path; this optimization must not make their memory use unbounded.
    if (edge_count > 2000000)
        return false;
    const auto less = [](const Point& a, const Point& b) {
        return a.x() < b.x() || (a.x() == b.x() && a.y() < b.y());
    };
    struct Edge {
        Point a, b;
        int direction;
    };
    std::vector<Edge> edges;
    edges.reserve(edge_count);
    for (const auto& polygon : polygons) {
        if (polygon.points.size() < 3)
            continue;
        for (size_t i = 0; i < polygon.points.size(); ++i) {
            const Point& a = polygon.points[i];
            const Point& b = polygon.points[(i + 1) % polygon.points.size()];
            if (a != b)
                edges.push_back(less(a, b) ? Edge{a, b, 1} : Edge{b, a, -1});
        }
    }
    std::sort(edges.begin(), edges.end(), [&](const Edge& a, const Edge& b) {
        return less(a.a, b.a) || (a.a == b.a && less(a.b, b.b));
    });
    size_t kept = 0;
    const size_t original_count = edges.size();
    for (size_t first = 0; first < edges.size();) {
        size_t end = first + 1;
        int direction = edges[first].direction;
        while (end < edges.size() && edges[first].a == edges[end].a && edges[first].b == edges[end].b)
            direction += edges[end++].direction;
        if (std::abs(direction) > 1)
            return false;
        if (direction != 0) {
            const auto edge = edges[first];
            edges[kept++] = direction > 0 ? Edge{edge.a, edge.b, 1} : Edge{edge.b, edge.a, 1};
        }
        first = end;
    }
    if (kept == 0 || kept > original_count * 3 / 4)
        return false;
    edges.resize(kept);
    std::sort(edges.begin(), edges.end(), [&](const Edge& a, const Edge& b) { return less(a.a, b.a); });
    for (size_t i = 1; i < edges.size(); ++i)
        if (edges[i - 1].a == edges[i].a)
            return false;
    std::vector<size_t> next(edges.size());
    std::vector<bool> incoming(edges.size(), false);
    for (size_t i = 0; i < edges.size(); ++i) {
        const auto found = std::lower_bound(edges.begin(), edges.end(), edges[i].b,
                                           [&](const Edge& edge, const Point& point) { return less(edge.a, point); });
        if (found == edges.end() || found->a != edges[i].b)
            return false;
        const size_t index = size_t(found - edges.begin());
        if (incoming[index])
            return false;
        incoming[index] = true;
        next[i] = index;
    }
    Polygons reduced;
    std::vector<bool> visited(edges.size(), false);
    for (size_t first = 0; first < edges.size(); ++first) {
        if (visited[first])
            continue;
        Polygon polygon;
        size_t index = first;
        do {
            if (visited[index])
                return false;
            visited[index] = true;
            polygon.points.push_back(edges[index].a);
            index = next[index];
        } while (index != first);
        if (polygon.points.size() >= 3)
            reduced.push_back(std::move(polygon));
    }
    polygons = std::move(reduced);
    return true;
}

} // namespace Slic3r
#endif
