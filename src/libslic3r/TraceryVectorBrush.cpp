#include "TriangleSelector.hpp"
#include "TraceryVectorSelection.hpp"
#include "TraceryVectorFill.hpp"
#include "TraceryPerformance.hpp"
#include <cmath>
#include <stdexcept>

namespace Slic3r {
namespace {
using Loop = std::vector<Vec3d>;
// Clip in the cursor's metric, retaining world/mesh positions until the final
// conversion. The shared original edge is never recursively subdivided.
Loop clip(Loop polygon, const Vec3d& n, double offset)
{
    Loop result;
    if (polygon.empty())
        return result;
    Vec3d  a  = polygon.back();
    double da = n.dot(a) - offset;
    for (const auto& b : polygon) {
        const double db = n.dot(b) - offset;
        if ((da <= 0) != (db <= 0))
            result.push_back(a + (b - a) * (da / (da - db)));
        if (db <= 0)
            result.push_back(b);
        a  = b;
        da = db;
    }
    return result;
}
int circle_segments(double radius, double tolerance)
{
    // 0.01 mm sagitta in the cursor metric. No interior sampling lattice.
    const double requested = std::ceil(M_PI / std::acos(std::clamp(1. - tolerance / radius, -1., 1.)));
    // Multiples of four keep opposite sides and quadrant extrema symmetric.
    return 4 * int(std::clamp(std::ceil(requested / 4.), 4., 128.));
}
Loop radial_clip(Loop polygon, const Vec3d& center, const Vec3d& normal, double radius, double tolerance)
{
    if (radius <= 1e-10)
        return {};
    const Vec3d  u = normal.unitOrthogonal(), v = normal.cross(u);
    const int    count = circle_segments(radius, tolerance);
    const double bound = radius * std::cos(M_PI / count);
    for (int i = 0; i < count && !polygon.empty(); ++i) {
        const double a = (2 * i + 1) * M_PI / count;
        const Vec3d  n = std::cos(a) * u + std::sin(a) * v;
        polygon        = clip(std::move(polygon), n, n.dot(center) + bound);
    }
    return polygon;
}
} // namespace

TriangleSelector::Tracery2DData TriangleSelector::project_cursor(int                start,
                                                                 const Cursor&      cursor,
                                                                 const Transform3d& transform,
                                                                 float              angle) const
{
    TraceryPerformanceScope perf("vector_brush_projection");
    Tracery2DData           result;
    if (start < 0 || start >= m_orig_size_indices || cursor.radius <= 0)
        return result;
    const auto* single = dynamic_cast<const SinglePointCursor*>(&cursor);
    const auto* sweep  = dynamic_cast<const DoublePointCursor*>(&cursor);
    if (!single && !sweep)
        throw std::runtime_error("Unsupported vector brush cursor");
    const bool sphere = dynamic_cast<const Sphere*>(&cursor) || dynamic_cast<const Capsule3D*>(&cursor);
    const bool height = dynamic_cast<const HeightRange*>(&cursor);
    if (height)
        throw std::runtime_error("Height range is not a stained-glass brush");
    const Vec3d                first     = single ? single->center.cast<double>() : sweep->first_center.cast<double>();
    const Vec3d                second    = single ? first : sweep->second_center.cast<double>();
    const Vec3d                direction = cursor.dir.cast<double>().normalized();
    const Transform3d          metric    = cursor.uniform_scaling ? Transform3d::Identity() : cursor.trafo.cast<double>();
    const Transform3d          inverse   = metric.inverse();
    const Matrix3d             normals   = transform.linear().inverse().transpose();
    const double               radius    = cursor.radius;
    const double               tolerance = cursor.uniform_scaling ? 0.01 / cursor.trafo.linear().col(0).norm() : 0.01;
    std::vector<unsigned char> seen(m_orig_size_indices, 0);
    std::vector<int>           queue{start};
    seen[start]       = 1;
    size_t candidates = 0;
    for (size_t q = 0; q < queue.size(); ++q) {
        const int id = queue[q];
        ++candidates;
        if (angle != 0 && (normals * m_face_normals[id].cast<double>()).normalized().z() >= -std::cos(double(angle) * M_PI / 180.))
            continue;
        Loop triangle;
        for (int v : m_mesh.its.indices[id])
            triangle.push_back(metric * m_mesh.its.vertices[v].cast<double>());
        Vec3d normal = (triangle[1] - triangle[0]).cross(triangle[2] - triangle[0]);
        if (normal.squaredNorm() == 0.)
            continue;
        normal.normalize();
        std::vector<Loop> pieces;
        if (!sphere) {
            // Capsule silhouette in the view-perpendicular plane; extrusion
            // along the view direction matches the ordinary circular cursor.
            Loop        p = triangle;
            const Vec3d u = direction.unitOrthogonal(), v = direction.cross(u);
            const int   count = circle_segments(radius, tolerance);
            for (int i = 0; i < count && !p.empty(); ++i) {
                const double a = (2 * i + 1) * M_PI / count;
                const Vec3d  n = std::cos(a) * u + std::sin(a) * v;
                p              = clip(std::move(p), n, std::max(n.dot(first), n.dot(second)) + radius * std::cos(M_PI / count));
            }
            // A capsule also has two straight sides. Angular radial planes
            // alone miss those side normals and can grow a long swept stroke.
            Vec3d side = direction.cross(second - first);
            if (side.squaredNorm() > 1e-20) {
                side.normalize();
                p = clip(std::move(p), side, side.dot(first) + radius);
                p = clip(std::move(p), -side, -side.dot(first) + radius);
            }
            pieces.push_back(std::move(p));
        } else {
            // A swept sphere is the union of two balls and the finite cylinder.
            // Its planar sections are circles/ellipses, not a dense triangle grid.
            for (const Vec3d& center : std::array<Vec3d, 2>{first, second}) {
                const double distance = normal.dot(center - triangle[0]);
                if (std::abs(distance) < radius)
                    pieces.push_back(radial_clip(triangle, center - normal * distance, normal,
                                                 std::sqrt(radius * radius - distance * distance), tolerance));
                if (single)
                    break;
            }
            const Vec3d delta = second - first;
            if (delta.norm() > 1e-10) {
                const Vec3d axis = delta.normalized();
                auto        p    = clip(clip(triangle, -axis, -axis.dot(first)), axis, axis.dot(second));
                pieces.push_back(radial_clip(std::move(p), first, axis, radius, tolerance));
            }
        }
        auto f = TraceryVectorSelection::frame(m_mesh, id);
        for (auto& p : pieces) {
            for (auto& v : p)
                v = inverse * v;
            if (cursor.clipping_plane.is_active())
                p = clip(std::move(p), cursor.clipping_plane.normal.cast<double>(), cursor.clipping_plane.offset);
            if (p.size() < 3)
                continue;
            TraceryRegion2D r;
            r.color = EnforcerBlockerType::ENFORCER;
            for (const auto& v : p) {
                const Vec3d d = v - f.origin;
                r.polygon.emplace_back(d.dot(f.axis_u) / f.axis_u.squaredNorm(), d.dot(f.axis_v) / f.axis_v.squaredNorm());
            }
            f.regions.push_back(std::move(r));
        }
        if (f.regions.empty())
            continue;
        result.push_back(std::move(f));
        for (int neighbor : m_neighbors[id])
            if (neighbor >= 0 && !seen[neighbor] && cursor.is_facet_visible(neighbor, m_face_normals)) {
                seen[neighbor] = 1;
                queue.push_back(neighbor);
            }
    }
    perf.count("candidate_facets", candidates);
    perf.count("touched_facets", result.size());
    perf.count("allocated_subdivision_nodes", 0);
    return result;
}

TriangleSelector::Tracery2DData TriangleSelector::seed_fill_regions() const
{
    Tracery2DData result;
    auto          visit = [&](auto&& self, int id) -> int {
        const auto& triangle = m_triangles[id];
        if (!triangle.valid())
            return -1;
        const size_t begin = result.size();
        int          state = triangle.is_split() ? self(self, triangle.children[0]) : int(triangle.is_selected_by_seed_fill());
        if (triangle.is_split()) {
            for (int i = 1; i <= triangle.number_of_split_sides(); ++i)
                if (self(self, triangle.children[i]) != state)
                    state = -1;
            if (state >= 0)
                result.resize(begin);
        }
        if (state == 1) {
            auto            f = TraceryVectorSelection::frame(m_mesh, triangle.source_triangle);
            TraceryRegion2D r;
            r.color = EnforcerBlockerType::ENFORCER;
            for (int v : triangle.verts_idxs) {
                const Vec3d d = m_vertices[v].v.cast<double>() - f.origin;
                r.polygon.emplace_back(d.dot(f.axis_u) / f.axis_u.squaredNorm(), d.dot(f.axis_v) / f.axis_v.squaredNorm());
            }
            f.regions.push_back(std::move(r));
            result.push_back(std::move(f));
        }
        return state;
    };
    for (int id = 0; id < m_orig_size_indices; ++id)
        visit(visit, id);
    return result;
}
} // namespace Slic3r

namespace Slic3r {

bool TraceryVectorFill::clear_preview()
{
    if (!m_preview)
        return false;
    for (int id : m_selected)
        m_source.m_triangles[id].unselect_by_seed_fill();
    m_preview = false;
    m_preview_dirty = true;
    return true;
}

bool TraceryVectorFill::show_preview()
{
    if (!m_preview && !m_selected.empty()) {
        for (int id : m_selected)
            m_source.m_triangles[id].select_by_seed_fill();
        m_preview = true;
        m_preview_dirty = true;
    }
    return std::exchange(m_preview_dirty, false);
}

const TriangleSelector::Tracery2DData& TraceryVectorFill::select(const Vec3f& hit, int facet,
                                                               const TriangleSelector::ClippingPlane& clp,
                                                               float angle, bool connected)
{
    TraceryPerformanceScope perf("vector_fill.selection");
    perf.count("connected", connected);
    perf.stage("fill.hit_lookup");
    const int start = facet >= 0 && facet < m_source.m_orig_size_indices ? m_source.select_unsplit_triangle(hit, facet) : -1;
    const bool same_query = m_valid && m_angle == angle && m_connected == connected && m_clip.normal == clp.normal &&
                            m_clip.offset == clp.offset && m_blocked == m_source.m_unselectable_states;
    // With clipping, the legacy flood rule admits a partial seed face but
    // excludes partial neighbours. Only reuse that asymmetric query at its seed.
    const bool selected = start >= 0 && (connected ? (size_t(start) < m_members.size() && m_members[start] == m_epoch) :
                                                    (!m_selected.empty() && start == m_selected.front()));
    if (same_query && selected && (!clp.is_active() || start == m_selected.front())) {
        perf.count("cache_hit", 1);
        perf.count("selected_leaves", m_selected.size());
        perf.count("compact_regions", m_regions.size());
        return m_regions;
    }
    perf.count("cache_hit", 0);
    perf.stage("fill.clear_previous");
    clear_preview();
    ++m_revision;
    m_valid = false;
    m_selected.clear();
    m_regions.clear();
    m_angle = angle;
    m_connected = connected;
    m_clip = clp;
    m_blocked = m_source.m_unselectable_states;
    if (start < 0 || m_blocked.count(m_source.m_triangles[start].get_state()))
        return m_regions;
    if (connected && m_members.empty())
        m_members.resize(m_source.m_triangles.size(), 0);
    if (++m_epoch == 0) {
        std::fill(m_members.begin(), m_members.end(), 0);
        ++m_epoch;
    }

    // The source selector is immutable for this cache's lifetime. Normal paint
    // subdivisions are only read; stained glass never adds or recolours nodes.
    perf.stage("fill.adjacency");
    const bool unsplit = m_source.m_triangles.size() == size_t(m_source.m_orig_size_indices);
    if (connected && !unsplit && m_neighbors.empty()) {
        auto neighbors = m_source.precompute_all_neighbors();
        m_neighbors = std::move(neighbors.first);
        m_propagated = std::move(neighbors.second);
        perf.count("adjacency_rebuilt", 1);
    } else
        perf.count("adjacency_rebuilt", 0);
    perf.stage("fill.connected_search");
    const auto color = m_source.m_triangles[start].get_state();
    const double limit = (angle < 0.f ? -1.f : std::cos(double(angle) * M_PI / 180.)) - EPSILON;
    if (connected)
        m_members[start] = m_epoch;
    m_selected.push_back(start);
    size_t candidates = 0;
    if (connected) {
        for (size_t q = 0; q < m_selected.size(); ++q) {
            const int id = m_selected[q];
            const auto& triangle = m_source.m_triangles[id];
            const auto& vertices = triangle.verts_idxs;
            auto consider = [&](int neighbor) {
                ++candidates;
                if (neighbor < 0 || m_members[neighbor] == m_epoch)
                    return;
                const auto& next = m_source.m_triangles[neighbor];
                if (next.get_state() != color || m_source.is_facet_clipped(neighbor, clp))
                    return;
                const auto& n1 = m_source.m_face_normals[triangle.source_triangle];
                const auto& n2 = m_source.m_face_normals[next.source_triangle];
                if (angle >= -EPSILON && std::clamp(n1.dot(n2), 0.f, 1.f) < limit)
                    return;
                // Mark on enqueue so several adjacent faces cannot enqueue the
                // same leaf repeatedly. No full-tree visited-array reset.
                m_members[neighbor] = m_epoch;
                m_selected.push_back(neighbor);
            };
            if (unsplit) {
                for (int neighbor : m_source.m_neighbors[id])
                    consider(neighbor);
            } else {
                std::vector<int> touching;
                for (int edge = 0; edge < 3; ++edge)
                    m_source.append_touching_subtriangles(m_neighbors[id][edge], vertices[(edge + 1) % 3], vertices[edge], touching);
                for (int neighbor : m_propagated[id])
                    if (neighbor >= 0 && !m_source.m_triangles[neighbor].is_split())
                        touching.push_back(neighbor);
                for (int neighbor : touching)
                    consider(neighbor);
            }
        }
    }
    perf.count("candidate_edges", candidates);
    perf.count("selected_leaves", m_selected.size());
    perf.stage("fill.compact_selected");
    std::vector<int> roots;
    if (connected && !unsplit) {
        roots.reserve(m_selected.size());
        for (int id : m_selected)
            roots.push_back(m_source.m_triangles[id].source_triangle);
        std::sort(roots.begin(), roots.end());
        roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    }
    std::vector<int> compact;
    size_t visited = 0;
    auto collect = [&](auto&& self, int id) -> bool {
        ++visited;
        const auto& t = m_source.m_triangles[id];
        if (!t.valid())
            return false;
        if (!t.is_split()) {
            if (m_members[id] != m_epoch)
                return false;
            compact.push_back(id);
            return true;
        }
        const size_t begin = compact.size();
        bool all = true;
        for (int child = 0; child <= t.number_of_split_sides(); ++child)
            if (!self(self, t.children[child]))
                all = false;
        if (all) {
            compact.resize(begin);
            compact.push_back(id);
        }
        return all;
    };
    // Face paint has exactly one leaf: do not inspect the rest of its tree.
    if (!connected || unsplit)
        compact = m_selected;
    else
        for (int root : roots)
            collect(collect, root);
    perf.count("export_visited_nodes", visited);
    perf.count("touched_source_facets", roots.empty() ? compact.size() : roots.size());
    perf.count("compact_regions", compact.size());
    perf.count("allocated_subdivision_nodes", 0);
    perf.stage("fill.export_vectors");
    m_regions.reserve(compact.size());
    for (int id : compact) {
        const auto& t = m_source.m_triangles[id];
        auto frame = TraceryVectorSelection::frame(m_source.m_mesh, t.source_triangle);
        TriangleSelector::TraceryRegion2D region;
        region.color = EnforcerBlockerType::ENFORCER;
        region.polygon.reserve(4);
        auto append = [&](const Vec3d& point) {
            const Vec3d d = point - frame.origin;
            region.polygon.emplace_back(d.dot(frame.axis_u) / frame.axis_u.squaredNorm(), d.dot(frame.axis_v) / frame.axis_v.squaredNorm());
        };
        if (clp.is_active()) {
            Loop polygon;
            for (int vertex : t.verts_idxs)
                polygon.push_back(m_source.m_vertices[vertex].v.cast<double>());
            polygon = clip(std::move(polygon), clp.normal.cast<double>(), clp.offset);
            if (polygon.size() < 3)
                continue;
            for (const auto& point : polygon)
                append(point);
        } else {
            for (int vertex : t.verts_idxs)
                append(m_source.m_vertices[vertex].v.cast<double>());
        }
        frame.regions.push_back(std::move(region));
        m_regions.push_back(std::move(frame));
    }
    m_valid = true;
    return m_regions;
}

} // namespace Slic3r
