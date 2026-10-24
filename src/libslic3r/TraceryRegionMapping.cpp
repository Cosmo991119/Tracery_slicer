#include "TraceryRegionMapping.hpp"
#include "AABBTreeIndirect.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>

namespace Slic3r {
bool tracery_has_identity_mapping(const indexed_triangle_set&   source,
                                  const TraceryPartitionResult& partition,
                                  const std::vector<int>&       previous_sources)
{
    if (source.vertices != partition.mesh.vertices || source.indices != partition.mesh.indices ||
        previous_sources.size() != source.indices.size() || partition.source_facets.size() != source.indices.size())
        return false;
    for (size_t facet = 0; facet < source.indices.size(); ++facet)
        if (previous_sources[facet] != int(facet) || partition.source_facets[facet] != int(facet))
            return false;
    return true;
}

namespace {
using Tree = AABBTreeIndirect::Tree<2, double>;
using Box = Tree::BoundingBox;
struct ChildBox
{
    size_t id;
    Box bounds;
    Tree::VectorType center;
    size_t idx() const { return id; }
    const Box& bbox() const { return bounds; }
    const Tree::VectorType& centroid() const { return center; }
};
struct SourceChildren
{
    Vec3d origin, u, v;
    double maximum_radius = 0.;
    double maximum_normal_tangent = 0.;
    std::vector<ChildBox> children;
    Tree tree;
    explicit SourceChildren(const indexed_triangle_set& source, int facet)
    {
        const auto& t = source.indices.at(facet);
        origin = source.vertices[t[0]].cast<double>();
        const Vec3d ab = source.vertices[t[1]].cast<double>() - origin;
        const Vec3d ac = source.vertices[t[2]].cast<double>() - origin;
        if (ab.cross(ac).squaredNorm() == 0.)
            throw std::runtime_error("Degenerate source facet while indexing stained glass");
        u = ab.normalized();
        v = ab.cross(ac).normalized().cross(u);
    }
    Vec2d project(const Vec3d& p) const
    {
        const Vec3d d = p - origin;
        return {d.dot(u), d.dot(v)};
    }
};
} // namespace
struct TraceryRegionMapper::Impl
{
    const TraceryPartitionResult& partition;
    std::map<int, SourceChildren> groups;
    std::vector<std::unique_ptr<detail::TraceryFacetClipper>> clippers;
    Impl(const indexed_triangle_set& source, const TraceryPartitionResult& value) : partition(value)
    {
        if (value.source_facets.size() != value.mesh.indices.size())
            throw std::invalid_argument("Invalid stained-glass source mapping");
        clippers.resize(value.mesh.indices.size());
        for (size_t i = 0; i < value.source_facets.size(); ++i) {
            const int old = value.source_facets[i];
            auto [it, inserted] = groups.try_emplace(old, source, old);
            auto& group = it->second;
            const auto& t = value.mesh.indices[i];
            ChildBox child;
            child.id = i;
            for (int vertex : t) {
                const Vec3d p = value.mesh.vertices[vertex].cast<double>();
                child.bounds.extend(group.project(p));
                group.maximum_radius = std::max(group.maximum_radius, (p - group.origin).norm());
            }
            const Vec3d a = value.mesh.vertices[t[0]].cast<double>();
            const Vec3d ab = value.mesh.vertices[t[1]].cast<double>() - a;
            const Vec3d ac = value.mesh.vertices[t[2]].cast<double>() - a;
            if (ab.cross(ac).squaredNorm() == 0.)
                throw std::runtime_error("Degenerate child facet while indexing stained glass");
            const Vec3d n = ab.cross(ac).normalized();
            group.maximum_normal_tangent = std::max(group.maximum_normal_tangent, std::hypot(n.dot(group.u), n.dot(group.v)));
            child.center = child.bounds.center();
            group.children.push_back(std::move(child));
        }
        for (auto& [id, group] : groups)
            if (group.children.size() > 16) {
                auto nodes = group.children;
                group.tree.build(std::move(nodes));
            }
    }
};
TraceryRegionMapper::TraceryRegionMapper(const indexed_triangle_set& source, const TraceryPartitionResult& partition)
    : m_impl(std::make_unique<Impl>(source, partition)) {}
TraceryRegionMapper::~TraceryRegionMapper() = default;
size_t TraceryRegionMapper::estimated_bytes() const
{
    size_t bytes = sizeof(Impl) + m_impl->clippers.capacity() * sizeof(m_impl->clippers.front());
    for (const auto& [id, group] : m_impl->groups)
        bytes += sizeof(group) + group.children.capacity() * sizeof(ChildBox) + group.tree.nodes().capacity() * sizeof(Tree::Node);
    for (const auto& clipper : m_impl->clippers)
        if (clipper) bytes += clipper->estimated_bytes();
    return bytes;
}
std::vector<TraceryMappedRegion> TraceryRegionMapper::map(int source_facet, const std::vector<Vec3f>& polygon,
                                                       TraceryMappingStatistics& statistics)
{
    const auto found = m_impl->groups.find(source_facet);
    if (found == m_impl->groups.end())
        throw std::runtime_error("Lost source facet while remapping stained glass");
    const auto& group = found->second;
    statistics.candidate_pairs += group.children.size();
    if (polygon.size() < 3) return {};
    Box bounds;
    double radius = 0.;
    for (const Vec3f& p : polygon) {
        if (!p.allFinite())
            throw std::invalid_argument("Non-finite stained-glass region");
        bounds.extend(group.project(p.cast<double>()));
        radius = std::max(radius, (p.cast<double>() - group.origin).norm());
    }
    // The exact clipper projects onto each child's plane. Float-stored children
    // may tilt slightly from their source plane. Bound the possible in-plane
    // displacement of that projection, including off-plane input, rather than
    // assuming all polygons are perfectly coplanar. Integer clipping contributes
    // at most a few base grid units (narrow-facet scaling only reduces that).
    const double reach = radius + group.maximum_radius;
    const double padding = group.maximum_normal_tangent * reach +
                           8. * SCALING_FACTOR + 64. * std::numeric_limits<double>::epsilon() * (reach + 1.);
    bounds.min().array() -= padding;
    bounds.max().array() += padding;
    std::vector<int> candidates;
    if (!std::isfinite(padding)) {
        for (const auto& child : group.children) candidates.push_back(int(child.id));
    } else if (group.tree.empty()) {
        for (const auto& child : group.children)
            if (bounds.intersects(child.bounds)) candidates.push_back(int(child.id));
    } else {
        AABBTreeIndirect::traverse(group.tree, AABBTreeIndirect::intersecting(bounds), [&](const Tree::Node& node) {
            candidates.push_back(int(node.idx));
            return true;
        });
        // Match legacy source/output order regardless of BVH traversal order.
        std::sort(candidates.begin(), candidates.end());
    }
    statistics.spatial_candidates += candidates.size();
    std::vector<TraceryMappedRegion> result;
    for (int facet : candidates) {
        auto& clipper = m_impl->clippers[facet];
        if (!clipper) clipper = std::make_unique<detail::TraceryFacetClipper>(m_impl->partition.mesh, facet);
        auto clipped = clipper->clip(polygon, statistics);
        if (clipped.size() >= 3) {
            ++statistics.nonempty_results;
            result.push_back({facet, std::move(clipped)});
        }
    }
    return result;
}
} // namespace Slic3r
