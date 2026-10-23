#ifndef slic3r_TraceryPolygonPreparation_hpp_
#define slic3r_TraceryPolygonPreparation_hpp_

#include "Polygon.hpp"
#include "TraceryDisjointSet.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <vector>

namespace Slic3r {

enum class TraceryReductionFailure : size_t {
    SmallBatch, EdgeBudget, Multiplicity, FewSharedEdges, BranchVertex, MissingEndpoint, IncomingConflict, InvalidCycle, Count
};

struct TraceryReductionStatistics {
    static constexpr size_t reason_count = size_t(TraceryReductionFailure::Count);
    std::array<size_t, reason_count> group_failures{};
    std::array<size_t, reason_count> group_input_polygons{};
    std::array<size_t, reason_count> local_failures{};
    std::array<size_t, reason_count> local_input_polygons{};
    size_t local_components = 0;
    size_t local_compacted_components = 0;
    size_t recovered_input_polygons = 0;
    size_t recovered_output_polygons = 0;
    size_t quarantined_polygons = 0;
    size_t retained_polygons = 0;
};

namespace tracery_polygon_detail {
constexpr size_t min_batch_polygons = 64;
constexpr size_t max_batch_edges = 2000000;
constexpr size_t max_component_polygons = 4096;

// Reduce a temporary set of same-material paths before its non-zero union.
// Exact opposite edges cancel in the winding field; no snapping, fitting or
// tolerance is used. Keep hole orientation. Ambiguous junctions/multiplicity
// fall back to the original paths, which the regular boolean engine handles.
inline bool try_cancel_internal_edges(Polygons& polygons, TraceryReductionFailure& failure, size_t& cancelled_edges)
{
    cancelled_edges = 0;
    const auto fail = [&](TraceryReductionFailure reason) { failure = reason; return false; };
    if (polygons.size() < min_batch_polygons)
        return fail(TraceryReductionFailure::SmallBatch);
    size_t edge_count = 0;
    for (const auto& polygon : polygons)
        edge_count += polygon.points.size();
    // Bound temporary memory. Large irregular batches still use the established
    // union path; this optimization must not make their memory use unbounded.
    if (edge_count > max_batch_edges)
        return fail(TraceryReductionFailure::EdgeBudget);
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
            return fail(TraceryReductionFailure::Multiplicity);
        if (direction != 0) {
            const auto edge = edges[first];
            edges[kept++] = direction > 0 ? Edge{edge.a, edge.b, 1} : Edge{edge.b, edge.a, 1};
        }
        first = end;
    }
    cancelled_edges = original_count - kept;
    if (kept == 0 || kept > original_count * 3 / 4)
        return fail(TraceryReductionFailure::FewSharedEdges);
    edges.resize(kept);
    std::sort(edges.begin(), edges.end(), [&](const Edge& a, const Edge& b) { return less(a.a, b.a); });
    for (size_t i = 1; i < edges.size(); ++i)
        if (edges[i - 1].a == edges[i].a)
            return fail(TraceryReductionFailure::BranchVertex);
    std::vector<size_t> next(edges.size());
    std::vector<bool> incoming(edges.size(), false);
    for (size_t i = 0; i < edges.size(); ++i) {
        const auto found = std::lower_bound(edges.begin(), edges.end(), edges[i].b,
                                           [&](const Edge& edge, const Point& point) { return less(edge.a, point); });
        if (found == edges.end() || found->a != edges[i].b)
            return fail(TraceryReductionFailure::MissingEndpoint);
        const size_t index = size_t(found - edges.begin());
        if (incoming[index])
            return fail(TraceryReductionFailure::IncomingConflict);
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
                return fail(TraceryReductionFailure::InvalidCycle);
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

} // namespace tracery_polygon_detail

// First try the cheap whole-group reduction. If a local ambiguity prevents it,
// retain those polygons and reduce bounded edge-connected patches independently.
// The caller still unions ALL returned paths together, so shared patch borders,
// overlapping components and holes retain the original non-zero winding field.
inline bool tracery_cancel_internal_edges(Polygons& polygons, TraceryReductionStatistics* statistics = nullptr)
{
    using namespace tracery_polygon_detail;
    TraceryReductionStatistics unused;
    auto& stats = statistics ? *statistics : unused;
    TraceryReductionFailure failure;
    size_t cancelled_edges = 0;
    if (try_cancel_internal_edges(polygons, failure, cancelled_edges))
        return true;
    ++stats.group_failures[size_t(failure)];
    stats.group_input_polygons[size_t(failure)] += polygons.size();
    if (failure == TraceryReductionFailure::SmallBatch || failure == TraceryReductionFailure::EdgeBudget ||
        (failure == TraceryReductionFailure::FewSharedEdges && cancelled_edges == 0)) {
        stats.retained_polygons += polygons.size();
        return false;
    }

    const auto less = [](const Point& a, const Point& b) {
        return a.x() < b.x() || (a.x() == b.x() && a.y() < b.y());
    };
    struct OwnedEdge {
        Point a, b;
        size_t owner;
        bool forward;
    };
    std::vector<OwnedEdge> edges;
    size_t edge_count = 0;
    for (const auto& polygon : polygons)
        edge_count += polygon.points.size();
    edges.reserve(edge_count);
    for (size_t owner = 0; owner < polygons.size(); ++owner) {
        const auto& points = polygons[owner].points;
        if (points.size() < 3)
            continue;
        for (size_t i = 0; i < points.size(); ++i) {
            const Point& a = points[i];
            const Point& b = points[(i + 1) % points.size()];
            if (a != b)
                edges.push_back(less(a, b) ? OwnedEdge{a, b, owner, true} : OwnedEdge{b, a, owner, false});
        }
    }
    std::sort(edges.begin(), edges.end(), [&](const OwnedEdge& a, const OwnedEdge& b) {
        if (a.a != b.a) return less(a.a, b.a);
        if (a.b != b.b) return less(a.b, b.b);
        return a.owner < b.owner;
    });
    // Do not connect across non-manifold or duplicate edges. Keep every owner
    // of such an edge intact instead of discarding duplicates or guessing sides.
    std::vector<bool> quarantined(polygons.size(), false);
    for (size_t first = 0; first < edges.size();) {
        size_t end = first + 1;
        while (end < edges.size() && edges[first].a == edges[end].a && edges[first].b == edges[end].b)
            ++end;
        if (end - first > 2 || (end - first == 2 && edges[first].forward == edges[first + 1].forward))
            for (size_t i = first; i < end; ++i)
                quarantined[edges[i].owner] = true;
        first = end;
    }
    TraceryDisjointSet components(polygons.size());
    std::vector<size_t> weights(polygons.size(), 1);
    for (size_t first = 0; first < edges.size();) {
        size_t end = first + 1;
        while (end < edges.size() && edges[first].a == edges[end].a && edges[first].b == edges[end].b)
            ++end;
        if (end - first == 2 && !quarantined[edges[first].owner] && !quarantined[edges[first + 1].owner]) {
            const size_t a = components.root(edges[first].owner);
            const size_t b = components.root(edges[first + 1].owner);
            const size_t weight = weights[a] + weights[b];
            if (a != b && weight <= max_component_polygons) {
                components.unite(a, b);
                weights[components.root(a)] = weight;
            }
        }
        first = end;
    }
    // Release the large edge index before copying any component's polygons.
    std::vector<OwnedEdge>().swap(edges);
    std::vector<std::pair<size_t, size_t>> members;
    members.reserve(polygons.size());
    for (size_t i = 0; i < polygons.size(); ++i) {
        if (quarantined[i])
            ++stats.quarantined_polygons;
        else
            members.emplace_back(components.root(i), i);
    }
    std::sort(members.begin(), members.end());
    Polygons reduced;
    std::vector<bool> consumed(polygons.size(), false);
    size_t recovered = 0;
    for (size_t first = 0; first < members.size();) {
        size_t end = first + 1;
        while (end < members.size() && members[first].first == members[end].first)
            ++end;
        const size_t count = end - first;
        ++stats.local_components;
        if (count < min_batch_polygons) {
            ++stats.local_failures[size_t(TraceryReductionFailure::SmallBatch)];
            stats.local_input_polygons[size_t(TraceryReductionFailure::SmallBatch)] += count;
        } else {
            Polygons part;
            part.reserve(count);
            for (size_t i = first; i < end; ++i)
                part.push_back(polygons[members[i].second]);
            if (try_cancel_internal_edges(part, failure, cancelled_edges)) {
                ++stats.local_compacted_components;
                stats.recovered_input_polygons += count;
                stats.recovered_output_polygons += part.size();
                recovered += count;
                for (size_t i = first; i < end; ++i)
                    consumed[members[i].second] = true;
                for (auto& polygon : part)
                    reduced.push_back(std::move(polygon));
            } else {
                ++stats.local_failures[size_t(failure)];
                stats.local_input_polygons[size_t(failure)] += count;
            }
        }
        first = end;
    }
    stats.retained_polygons += polygons.size() - recovered;
    if (recovered == 0)
        return false;
    for (size_t i = 0; i < polygons.size(); ++i)
        if (!consumed[i])
            reduced.push_back(std::move(polygons[i]));
    polygons = std::move(reduced);
    return true;
}

} // namespace Slic3r
#endif
