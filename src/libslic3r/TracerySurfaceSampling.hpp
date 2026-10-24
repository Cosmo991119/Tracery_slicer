#ifndef slic3r_TracerySurfaceSampling_hpp_
#define slic3r_TracerySurfaceSampling_hpp_
#include "Point.hpp"
#include <array>
#include <functional>
#include <vector>

namespace Slic3r {
struct TracerySurfaceEdge
{
    size_t volume;
    size_t owner;
    Vec3d  first;
    Vec3d  second;
};
// Report positive-length collinear overlaps, including T junctions. Point
// contacts and nearby parallel edges do not connect surface regions.
size_t tracery_shared_edges(const std::vector<TracerySurfaceEdge>& edges, const std::function<void(size_t, size_t)>& connect);
std::vector<std::pair<Vec3d, Vec3d>> tracery_exposed_edges(const std::vector<TracerySurfaceEdge>& edges);
std::vector<Vec3f>                   tracery_area_candidates(const std::vector<std::array<Vec3f, 3>>& triangles, size_t seed_budget);
} // namespace Slic3r
#endif
