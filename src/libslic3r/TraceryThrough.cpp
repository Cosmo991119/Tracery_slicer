#include "TraceryThrough.hpp"
#include "AABBTreeIndirect.hpp"
#include "ClipperUtils.hpp"
#include "Geometry/ConvexHull.hpp"
#include "TriangleMeshSlicer.hpp"
#include "TraceryPerformance.hpp"
#include <stdexcept>

namespace Slic3r {
namespace {
using Ring = std::vector<Vec3d>;
struct SweptPolygon
{
    Ring   front, back;
    double min_z = 0, max_z = 0;
    SweptPolygon(const Ring& polygon, const Vec3d& sweep, const Transform3d& transform)
    {
        min_z = std::numeric_limits<double>::max();
        max_z = -min_z;
        for (const auto& point : polygon) {
            front.emplace_back(transform * point);
            back.emplace_back(transform * (point + sweep));
            min_z = std::min({min_z, front.back().z(), back.back().z()});
            max_z = std::max({max_z, front.back().z(), back.back().z()});
        }
    }
    // A plane section of a convex swept polygon is convex. Intersect its edges
    // directly, avoiding a temporary closed mesh and coplanar mesh-slicing seams.
    Polygon section(double z) const
    {
        if (front.size() < 3 || z < min_z || z > max_z)
            return {};
        Points     points;
        const auto edge = [&](const Vec3d& a, const Vec3d& b) {
            if ((a.z() < z && b.z() < z) || (a.z() > z && b.z() > z))
                return;
            const auto add = [&](const Vec3d& p) { points.emplace_back(scale_(p.x()), scale_(p.y())); };
            if (std::abs(a.z() - z) < 1e-9)
                add(a);
            if (std::abs(b.z() - z) < 1e-9)
                add(b);
            if ((a.z() < z && b.z() > z) || (a.z() > z && b.z() < z))
                add(a + (b - a) * ((z - a.z()) / (b.z() - a.z())));
        };
        for (size_t i = 0; i < front.size(); ++i) {
            const size_t j = (i + 1) % front.size();
            edge(front[i], front[j]);
            edge(back[i], back[j]);
            edge(front[i], back[i]);
        }
        if (points.size() < 3)
            return {};
        auto polygon = Geometry::convex_hull(std::move(points));
        polygon.make_counter_clockwise();
        return polygon;
    }
};

Ring clip_after_entry(const Ring& polygon, const Vec3d& origin, const Vec3d& direction)
{
    Ring             result;
    constexpr double epsilon = 1e-7;
    for (size_t i = 0; i < polygon.size(); ++i) {
        const auto&  a  = polygon[i];
        const auto&  b  = polygon[(i + 1) % polygon.size()];
        const double da = direction.dot(a - origin) - epsilon;
        const double db = direction.dot(b - origin) - epsilon;
        if (da >= 0)
            result.push_back(a);
        if ((da < 0) != (db < 0))
            result.emplace_back(a + (b - a) * (da / (da - db)));
    }
    return result;
}
} // namespace

TraceryThroughLayers slice_tracery_through(const indexed_triangle_set&                 mesh,
                                           const std::vector<TraceryThroughOperation>& operations,
                                           const Transform3d&                          transform,
                                           const std::vector<float>&                   zs,
                                           const std::function<void()>&                cancel)
{
    TraceryPerformanceScope timing("through_geometry");
    TraceryThroughLayers    result(zs.size());
    if (operations.empty() || zs.empty() || mesh.indices.empty())
        return result;
    timing.stage("through.target_slices");
    MeshSlicingParamsEx params;
    params.trafo  = transform;
    auto oriented = mesh;
    if (transform.linear().determinant() < 0)
        its_flip_triangles(oriented);
    const auto solid = slice_mesh_ex(oriented, zs, params, cancel);
    timing.stage("through.spatial_index");
    const auto   tree       = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(mesh.vertices, mesh.indices);
    const double length     = 2. * tree.nodes().front().bbox.diagonal().norm() + 1.;
    size_t       candidates = 0, patches = 0, sections = 0;
    for (const auto& operation : operations) {
        TraceryThroughLayers incoming(zs.size());
        timing.stage("through.sections");
        for (const auto& patch : operation.patches) {
            cancel();
            if (patch.entry.size() < 3 || !patch.direction.allFinite() || std::abs(patch.direction.norm() - 1.) > 1e-5)
                throw std::runtime_error("Invalid full-depth stained-glass entry");
            ++patches;
            const Vec3d         sweep = patch.direction * length;
            const SweptPolygon  channel(patch.entry, sweep, transform);
            Eigen::AlignedBox3f bounds;
            for (const auto& point : patch.entry) {
                bounds.extend(point.cast<float>());
                bounds.extend((point + sweep).cast<float>());
            }
            bounds.min().array() -= 1e-5f;
            bounds.max().array() += 1e-5f;
            std::vector<SweptPolygon> shadows;
            // Only outward crossings after the entry cast a shadow. Removing
            // these shadows stops the channel at its FIRST exit, including a
            // cavity or a gap before another disconnected solid.
            AABBTreeIndirect::traverse(tree, AABBTreeIndirect::intersecting(bounds), [&](const auto& node) {
                const size_t id = node.idx;
                if ((++candidates & 255) == 0)
                    cancel();
                const auto& face = mesh.indices[id];
                const Vec3d a    = mesh.vertices[face[0]].cast<double>();
                const Vec3d b    = mesh.vertices[face[1]].cast<double>();
                const Vec3d c    = mesh.vertices[face[2]].cast<double>();
                if ((b - a).cross(c - a).dot(patch.direction) <= 1e-12)
                    return true;
                const Ring exit = clip_after_entry({a, b, c}, patch.entry.front(), patch.direction);
                if (exit.size() >= 3)
                    shadows.emplace_back(exit, sweep, transform);
                return true;
            });
            if (shadows.empty())
                throw std::runtime_error("Full-depth stained glass could not find a solid exit");
            for (size_t layer = 0; layer < zs.size(); ++layer) {
                if ((layer & 63) == 0)
                    cancel();
                auto boundary = channel.section(zs[layer]);
                if (boundary.points.size() < 3 || solid[layer].empty())
                    continue;
                ++sections;
                ExPolygons mask = intersection_ex(Polygons{boundary}, solid[layer]);
                if (mask.empty())
                    continue;
                Polygons occluded;
                for (const auto& shadow : shadows) {
                    auto section = shadow.section(zs[layer]);
                    if (section.points.size() >= 3)
                        occluded.push_back(std::move(section));
                }
                if (!occluded.empty())
                    mask = diff_ex(mask, union_ex(occluded));
                append(incoming[layer][patch.material], std::move(mask));
            }
        }
        timing.stage("through.operation_overlay");
        for (size_t layer = 0; layer < zs.size(); ++layer) {
            cancel();
            ExPolygons footprint;
            for (auto& [material, polygons] : incoming[layer]) {
                polygons = union_ex(polygons);
                append(footprint, polygons);
            }
            footprint = union_ex(footprint);
            if (footprint.empty())
                continue;
            for (auto& [material, polygons] : result[layer])
                polygons = diff_ex(polygons, footprint);
            if (!operation.erase)
                for (auto& [material, polygons] : incoming[layer]) {
                    append(result[layer][material], std::move(polygons));
                    result[layer][material] = union_ex(result[layer][material]);
                }
        }
    }
    for (auto& layer : result)
        for (auto it = layer.begin(); it != layer.end();)
            if (it->second.empty())
                it = layer.erase(it);
            else
                ++it;
    timing.count("entry_patches", patches);
    timing.count("exit_candidates", candidates);
    timing.count("nonempty_sections", sections);
    return result;
}
} // namespace Slic3r
