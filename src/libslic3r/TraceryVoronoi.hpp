#ifndef slic3r_TraceryVoronoi_hpp_
#define slic3r_TraceryVoronoi_hpp_

#include "Point.hpp"
#include "TraceryPerformance.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <vector>

namespace Slic3r {

struct TraceryVoronoiVertex {
    Vec3f local;
    Vec3f world;
    Vec3f warped;
};
using TraceryVoronoiTriangle = std::array<TraceryVoronoiVertex, 3>;

struct TraceryVoronoiStatistics {
    size_t triangles = 0;
    size_t potential_cells = 0;
    size_t contained_triangles = 0;
    size_t boundary_triangles = 0;
    size_t containment_planes = 0;
    size_t candidate_cells = 0;
    size_t tested_halfplanes = 0;
    size_t inside_planes = 0;
    size_t rejected_cells = 0;
    size_t clip_calls = 0;
    size_t output_polygons = 0;
    size_t output_vertices = 0;
};

// A color region's seeds and ordered half-planes are immutable for one stroke.
// Source triangles and seed IDs retain their original order. Reusing this CPU
// helper across bounded batches avoids per-triangle map lookups/allocations.
class TraceryVoronoiClipper
{
public:
    TraceryVoronoiClipper(const std::vector<Vec3f>& seeds, const std::vector<size_t>& indices)
    {
        m_cells.reserve(indices.size());
        for (size_t seed : indices) {
            Cell cell{seed, seeds.at(seed), {}};
            cell.planes.reserve(indices.size() - 1);
            for (size_t other : indices) {
                if (seed == other)
                    continue;
                const auto key = std::minmax(seed, other);
                const Vec3d first = seeds.at(key.first).cast<double>();
                const Vec3d second = seeds.at(key.second).cast<double>();
                const Vec3d direction = second - first;
                const double side = seed < other ? 1. : -1.;
                cell.planes.push_back({side * direction, side * direction.dot((first + second) * 0.5)});
            }
            m_cells.push_back(std::move(cell));
        }
        m_polygon.reserve(indices.size() + 3);
        m_scratch.reserve(indices.size() + 3);
        m_values.reserve(indices.size() + 3);
    }

    template<class Emit>
    void generate_batch(const std::vector<TraceryVoronoiTriangle>& triangles, Emit&& emit,
                        TraceryVoronoiStatistics& stats, TraceryPerformanceScope& performance)
    {
        stats.triangles += triangles.size();
        stats.potential_cells += triangles.size() * m_cells.size();
        std::vector<size_t> owners(triangles.size(), NO_CELL);
        {
            TraceryPerformanceScope::Sample sample(performance, "voronoi.classify_triangles");
            for (size_t i = 0; i < triangles.size(); ++i) {
                owners[i] = containing_cell(triangles[i], stats);
                if (owners[i] != NO_CELL)
                    ++stats.contained_triangles;
                else
                    ++stats.boundary_triangles;
            }
        }
        struct Fragment {
            size_t cell;
            std::vector<TraceryVoronoiVertex> polygon;
        };
        std::vector<std::vector<Fragment>> boundary(triangles.size());
        {
            TraceryPerformanceScope::Sample sample(performance, "voronoi.clip_boundaries");
            for (size_t i = 0; i < triangles.size(); ++i) {
                if (owners[i] != NO_CELL)
                    continue;
                for (size_t cell = 0; cell < m_cells.size(); ++cell) {
                    ++stats.candidate_cells;
                    m_polygon.assign(triangles[i].begin(), triangles[i].end());
                    for (const auto& plane : m_cells[cell].planes) {
                        clip(plane, stats);
                        if (m_polygon.empty()) {
                            ++stats.rejected_cells;
                            break;
                        }
                    }
                    if (m_polygon.size() >= 3)
                        boundary[i].push_back({cell, m_polygon});
                }
            }
        }
        {
            TraceryPerformanceScope::Sample sample(performance, "voronoi.emit_polygons");
            for (size_t i = 0; i < triangles.size(); ++i) {
                if (owners[i] != NO_CELL) {
                    emit(i, m_cells[owners[i]].id, triangles[i]);
                    ++stats.output_polygons;
                    stats.output_vertices += 3;
                } else {
                    for (const auto& fragment : boundary[i]) {
                        emit(i, m_cells[fragment.cell].id, fragment.polygon);
                        ++stats.output_polygons;
                        stats.output_vertices += fragment.polygon.size();
                    }
                }
            }
        }
    }

private:
    struct Plane { Vec3d direction; double limit; };
    struct Cell { size_t id; Vec3f position; std::vector<Plane> planes; };
    static constexpr size_t NO_CELL = std::numeric_limits<size_t>::max();
    std::vector<Cell> m_cells;
    std::vector<TraceryVoronoiVertex> m_polygon, m_scratch;
    std::vector<double> m_values;

    size_t containing_cell(const TraceryVoronoiTriangle& triangle, TraceryVoronoiStatistics& stats) const
    {
        // A nearest seed is only a candidate. Validate ALL its half-planes at
        // ALL source vertices before bypassing clipping. Ties/duplicate seeds
        // and points near a bisector always take the reference clipping path.
        size_t candidate = NO_CELL;
        double distance = std::numeric_limits<double>::infinity();
        for (size_t i = 0; i < m_cells.size(); ++i) {
            const double d = (triangle[0].warped.cast<double>() - m_cells[i].position.cast<double>()).squaredNorm();
            if (d < distance) {
                candidate = i;
                distance = d;
            }
        }
        if (candidate == NO_CELL)
            return NO_CELL;
        Vec3d magnitude = Vec3d::Zero();
        for (const auto& vertex : triangle)
            magnitude = magnitude.cwiseMax(vertex.warped.cast<double>().cwiseAbs());
        for (const auto& plane : m_cells[candidate].planes) {
            ++stats.containment_planes;
            // Be conservative around float interpolation error in the original
            // clipping path. This guard changes eligibility, never coordinates.
            const double guard = 64. * std::numeric_limits<float>::epsilon() *
                                 (plane.direction.cwiseAbs().dot(magnitude) + std::abs(plane.limit) + 1.);
            for (const auto& vertex : triangle)
                if (!(vertex.warped.cast<double>().dot(plane.direction) - plane.limit < -guard))
                    return NO_CELL;
        }
        return candidate;
    }

    void clip(const Plane& plane, TraceryVoronoiStatistics& stats)
    {
        ++stats.tested_halfplanes;
        m_values.resize(m_polygon.size());
        bool inside = false, outside = false;
        for (size_t i = 0; i < m_polygon.size(); ++i) {
            m_values[i] = m_polygon[i].warped.cast<double>().dot(plane.direction) - plane.limit;
            if (m_values[i] <= 0.) inside = true;
            else outside = true;
        }
        if (!inside) {
            m_polygon.clear();
            return;
        }
        if (!outside) {
            ++stats.inside_planes;
            return;
        }
        ++stats.clip_calls;
        m_scratch.clear();
        for (size_t i = 0; i < m_polygon.size(); ++i) {
            const size_t next = (i + 1) % m_polygon.size();
            const auto& first = m_polygon[i];
            const auto& second = m_polygon[next];
            const double a = m_values[i], b = m_values[next];
            if (a <= 0.) m_scratch.push_back(first);
            if ((a <= 0.) != (b <= 0.)) {
                const float fraction = float(std::clamp(a / (a - b), 0., 1.));
                m_scratch.push_back({first.local + fraction * (second.local - first.local),
                                     first.world + fraction * (second.world - first.world),
                                     first.warped + fraction * (second.warped - first.warped)});
            }
        }
        m_polygon.swap(m_scratch);
    }
};

} // namespace Slic3r
#endif
