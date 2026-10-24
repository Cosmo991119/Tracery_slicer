#include "TraceryVectorSelection.hpp"
#include "ClipperUtils.hpp"
#include "Tesselate.hpp"
#include "TraceryPerformance.hpp"
#include "TraceryPolygonPreparation.hpp"
#include <stdexcept>

namespace Slic3r {
namespace {
Point project(const TriangleSelector::TraceryFacet2DData& f, const Vec3d& p)
{
    const Vec3d d = p - f.origin;
    return {scale_(d.dot(f.axis_u) / f.axis_u.squaredNorm()), scale_(d.dot(f.axis_v) / f.axis_v.squaredNorm())};
}
void emit(const TriangleMesh& mesh, int id, const Polygons& polygons, EnforcerBlockerType color, TraceryVectorSelection::Data& result)
{
    auto       f        = TraceryVectorSelection::frame(mesh, id);
    const auto vertices = triangulate_expolygons_2d(union_ex(polygons));
    for (size_t i = 0; i + 2 < vertices.size(); i += 3) {
        TriangleSelector::TraceryRegion2D region;
        region.color   = color;
        region.polygon = {vertices[i], vertices[i + 1], vertices[i + 2]};
        f.regions.push_back(std::move(region));
    }
    if (!f.regions.empty())
        result.push_back(std::move(f));
}
} // namespace

TriangleSelector::TraceryFacet2DData TraceryVectorSelection::frame(const TriangleMesh& mesh, int id)
{
    const auto&                          t = mesh.its.indices.at(id);
    TriangleSelector::TraceryFacet2DData f;
    f.source_facet_id = id;
    f.origin          = mesh.its.vertices[t[0]].cast<double>();
    const Vec3d a     = mesh.its.vertices[t[1]].cast<double>() - f.origin;
    const Vec3d b     = mesh.its.vertices[t[2]].cast<double>() - f.origin;
    if (a.cross(b).squaredNorm() == 0.)
        throw std::runtime_error("Degenerate vector selection facet");
    f.axis_u                   = a.normalized();
    f.axis_v                   = a.cross(b).normalized().cross(f.axis_u);
    const double altitude      = a.cross(b).norm() / std::max({a.norm(), b.norm(), (a - b).norm()});
    const double magnification = std::clamp(100. * SCALING_FACTOR / altitude, 1., 1000000.);
    f.axis_u /= magnification;
    f.axis_v /= magnification;
    return f;
}

TraceryVectorSelection TraceryVectorSelection::from_data(const TriangleMesh& mesh, const Data& data)
{
    TraceryVectorSelection result;
    for (const auto& f : data) {
        auto target                              = frame(mesh, f.source_facet_id);
        result.m_area_factors[f.source_facet_id] = target.axis_u.cross(target.axis_v).norm();
        for (const auto& region : f.regions) {
            Polygon p;
            for (const auto& v : region.polygon) {
                const Vec3d point = f.origin + v.x() * f.axis_u + v.y() * f.axis_v;
                if (!point.allFinite())
                    throw std::runtime_error("Invalid vector selection point");
                p.points.push_back(project(target, point));
            }
            if (p.points.size() >= 3) {
                p.make_counter_clockwise();
                result.m_regions[f.source_facet_id].push_back(std::move(p));
            }
        }
    }
    for (auto it = result.m_regions.begin(); it != result.m_regions.end();) {
        const auto f = frame(mesh, it->first);
        Polygon    bounds;
        for (int v : mesh.its.indices[it->first])
            bounds.points.push_back(project(f, mesh.its.vertices[v].cast<double>()));
        bounds.make_counter_clockwise();
        tracery_cancel_internal_edges(it->second);
        it->second = intersection(union_(it->second), Polygons{bounds});
        if (it->second.empty())
            it = result.m_regions.erase(it);
        else
            ++it;
    }
    return result;
}

TraceryVectorSelection TraceryVectorSelection::from_annotation(const TriangleMesh&                            mesh,
                                                               const TriangleSelector::TriangleSplittingData& annotation)
{
    TriangleSelector selector(mesh);
    selector.deserialize(annotation, false);
    Data data;
    for (const auto& leaf : selector.coalesced_painted_leaves()) {
        auto                              f = frame(mesh, leaf.source_facet);
        TriangleSelector::TraceryRegion2D r;
        r.color = EnforcerBlockerType::ENFORCER;
        for (const auto& v : leaf.vertices) {
            const Vec3d d = v.cast<double>() - f.origin;
            r.polygon.emplace_back(d.dot(f.axis_u) / f.axis_u.squaredNorm(), d.dot(f.axis_v) / f.axis_v.squaredNorm());
        }
        f.regions.push_back(std::move(r));
        data.push_back(std::move(f));
    }
    return from_data(mesh, data);
}

void TraceryVectorSelection::apply(const TraceryVectorSelection& stroke, bool erase)
{
    TraceryPerformanceScope perf("vector_selection_boolean");
    perf.count("touched_facets", stroke.m_regions.size());
    for (const auto& [id, paths] : stroke.m_regions) {
        auto found = m_regions.find(id);
        if (erase && found == m_regions.end())
            continue;
        auto& current      = m_regions[id];
        m_area_factors[id] = stroke.m_area_factors.at(id);
        if (erase)
            current = diff(current, paths);
        else {
            append(current, paths);
            current = union_(current);
        }
        if (current.empty())
            m_regions.erase(id);
    }
}
TraceryVectorSelection TraceryVectorSelection::difference(const TraceryVectorSelection& other) const
{
    auto copy = *this;
    copy.apply(other, true);
    return copy;
}
void TraceryVectorSelection::restrict_to(const TraceryVectorSelection& allowed)
{
    for (auto it = m_regions.begin(); it != m_regions.end();) {
        const auto found = allowed.m_regions.find(it->first);
        if (found == allowed.m_regions.end())
            it = m_regions.erase(it);
        else {
            it->second = intersection(it->second, found->second);
            if (it->second.empty())
                it = m_regions.erase(it);
            else
                ++it;
        }
    }
}
TraceryVectorSelection::Data TraceryVectorSelection::triangulated(const TriangleMesh& mesh) const
{
    Data result;
    for (const auto& [id, paths] : m_regions)
        emit(mesh, id, paths, EnforcerBlockerType::ENFORCER, result);
    return result;
}
TraceryVectorSelection::Data TraceryVectorSelection::colored(const TriangleMesh&                            mesh,
                                                             const TriangleSelector::TriangleSplittingData& paint,
                                                             const std::set<EnforcerBlockerType>&           blocked) const
{
    TraceryPerformanceScope perf("vector_selection_colors");
    if (m_regions.empty())
        return {};
    TriangleSelector selector(mesh);
    selector.deserialize(paint, false);
    std::vector<int> facets;
    for (const auto& [id, paths] : m_regions)
        facets.push_back(id);
    std::map<int, std::map<EnforcerBlockerType, Polygons>> colors;
    for (const auto& leaf : selector.coalesced_painted_leaves(true, &facets)) {
        if (blocked.count(leaf.state))
            continue;
        const auto f = frame(mesh, leaf.source_facet);
        Polygon    p;
        for (const auto& v : leaf.vertices)
            p.points.push_back(project(f, v.cast<double>()));
        p.make_counter_clockwise();
        colors[leaf.source_facet][leaf.state].push_back(std::move(p));
    }
    Data   result;
    size_t count = 0;
    for (auto& [id, states] : colors)
        for (auto& [color, paths] : states) {
            tracery_cancel_internal_edges(paths);
            emit(mesh, id, intersection(m_regions.at(id), union_(paths)), color, result);
        }
    for (const auto& f : result)
        count += f.regions.size();
    perf.count("temporary_triangles", count);
    return result;
}
double TraceryVectorSelection::area() const
{
    double result = 0;
    for (const auto& [id, paths] : m_regions)
        for (const auto& p : paths)
            result += p.area() * SCALING_FACTOR * SCALING_FACTOR * m_area_factors.at(id);
    return result;
}
bool TraceryVectorSelection::operator==(const TraceryVectorSelection& other) const
{
    if (m_regions.size() != other.m_regions.size())
        return false;
    for (const auto& [id, paths] : m_regions) {
        const auto found = other.m_regions.find(id);
        if (found == other.m_regions.end() || paths.size() != found->second.size())
            return false;
        for (size_t i = 0; i < paths.size(); ++i)
            if (paths[i].points != found->second[i].points)
                return false;
    }
    return true;
}
} // namespace Slic3r
