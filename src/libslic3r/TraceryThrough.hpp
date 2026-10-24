#ifndef slic3r_TraceryThrough_hpp_
#define slic3r_TraceryThrough_hpp_
#include "TriangleMesh.hpp"
#include "ExPolygon.hpp"
#include <functional>
#include <map>
namespace Slic3r {
// Convex entry patches in unchanged volume-local coordinates. No solid mesh is generated.
struct TraceryThroughPatch
{
    std::vector<Vec3d> entry;
    Vec3d              direction;
    unsigned           material = 0;
};
struct TraceryThroughOperation
{
    bool                             erase = false;
    std::vector<TraceryThroughPatch> patches;
};
using TraceryThroughLayers = std::vector<std::map<unsigned, ExPolygons>>;
TraceryThroughLayers slice_tracery_through(
    const indexed_triangle_set&                 mesh,
    const std::vector<TraceryThroughOperation>& operations,
    const Transform3d&                          transform,
    const std::vector<float>&                   zs,
    const std::function<void()>&                cancel = [] {});
} // namespace Slic3r
#endif
