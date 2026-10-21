#include "TraceryPartition.hpp"
#include "TraceryRegionMapping.hpp"
#include "TraceryPerformance.hpp"
#include "ClipperUtils.hpp"
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
#include <CGAL/Constrained_Delaunay_triangulation_2.h>
#include <CGAL/Triangulation_vertex_base_with_info_2.h>
#include <algorithm>
#include <array>
#include <map>
#include <numeric>
#include <cmath>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>

namespace Slic3r {
namespace {
constexpr double weld_tolerance = TRACERY_GEOMETRY_TOLERANCE;
using Edge                      = std::pair<int, int>;
using States                    = std::vector<EnforcerBlockerType>;

struct FacetFrame
{
    Vec3d  origin, u, v;
    double vertex_tolerance = scale_(weld_tolerance) + 2.;
    double coordinate_scale = 1.;
    explicit FacetFrame(const indexed_triangle_set& mesh, int facet)
    {
        const auto& t = mesh.indices.at(facet);
        origin        = mesh.vertices[t[0]].cast<double>();
        const Vec3d b = mesh.vertices[t[1]].cast<double>() - origin;
        const Vec3d c = mesh.vertices[t[2]].cast<double>() - origin;
        if (b.cross(c).squaredNorm() == 0.)
            throw std::runtime_error("Degenerate source triangle in exact tracery partition");
        u = b.normalized();
        v = b.cross(c).normalized().cross(u);
        const double altitude = b.cross(c).norm() / std::max({b.norm(), c.norm(), (b - c).norm()});
        if (altitude < 20. * weld_tolerance) {
            // Keep at least ~100 grid units across a narrow facet and do not
            // snap across more than 5% of its height. Ordinary facets retain
            // their existing coordinate system and welding behaviour.
            coordinate_scale = std::clamp(100. * SCALING_FACTOR / altitude, 1., 1000.);
            vertex_tolerance = scale_(altitude * 0.05) * coordinate_scale;
        }
    }
    Point project(const Vec3d& point) const
    {
        const Vec3d delta = point - origin;
        return Point(scale_(delta.dot(u)) * coordinate_scale, scale_(delta.dot(v)) * coordinate_scale);
    }
    Vec3d lift(const Point& point) const
    {
        return origin + (unscale<double>(point.x()) * u + unscale<double>(point.y()) * v) / coordinate_scale;
    }
};

struct Cell
{
    ExPolygon polygon;
    States    states;
};
struct FacetPartition
{
    FacetFrame        frame;
    std::vector<Cell> cells;
    bool              needs_partition = false;
    bool              protect_corners = false;
    FacetPartition(const indexed_triangle_set& mesh, int facet, size_t channels) : frame(mesh, facet)
    {
        Polygon contour;
        for (int index : mesh.indices[facet])
            contour.points.push_back(frame.project(mesh.vertices[index].cast<double>()));
        contour.make_counter_clockwise();
        cells.push_back({ExPolygon(std::move(contour)), States(channels, EnforcerBlockerType::NONE)});
    }
};

Polygon projected_polygon(const FacetFrame& frame, const std::vector<Vec3f>& vertices)
{
    Polygon polygon;
    for (const Vec3f& vertex : vertices) {
        const Point point = frame.project(vertex.cast<double>());
        if (polygon.points.empty() || polygon.points.back() != point)
            polygon.points.push_back(point);
    }
    if (polygon.points.size() > 1 && polygon.points.front() == polygon.points.back())
        polygon.points.pop_back();
    if (polygon.points.size() < 3)
        return {};
    polygon.make_counter_clockwise();
    return polygon;
}

void overlay_region(std::vector<Cell>& cells, const Polygons& region, size_t channel, EnforcerBlockerType state)
{
    if (region.empty())
        return;
    std::vector<Cell> result;
    for (Cell& cell : cells) {
        const ExPolygons original{cell.polygon};
        ExPolygons       inside = intersection_ex(original, region);
        if (inside.empty()) {
            result.push_back(std::move(cell));
            continue;
        }
        for (ExPolygon& polygon : diff_ex(original, region))
            result.push_back({std::move(polygon), cell.states});
        States painted   = cell.states;
        painted[channel] = state;
        for (ExPolygon& polygon : inside)
            result.push_back({std::move(polygon), painted});
    }
    cells = std::move(result);
}

void overlay_annotation(std::vector<FacetPartition>&                   facets,
                        const TriangleMesh&                            mesh,
                        const TriangleSelector::TriangleSplittingData& annotation,
                        size_t                                         channel)
{
    if (annotation.triangles_to_split.empty())
        return;
    TriangleSelector selector(mesh);
    selector.deserialize(annotation, false);
    std::map<std::pair<int, EnforcerBlockerType>, Polygons> regions;
    for (const auto& leaf : selector.painted_leaves()) {
        const auto& triangle    = mesh.its.indices[leaf.source_facet];
        bool        whole_facet = true;
        for (int i = 0; i < 3; ++i)
            whole_facet = whole_facet && leaf.vertices[i] == mesh.its.vertices[triangle[i]];
        if (whole_facet) {
            for (Cell& cell : facets[leaf.source_facet].cells)
                cell.states[channel] = leaf.state;
            continue;
        }
        facets[leaf.source_facet].needs_partition = true;
        const FacetFrame& frame                   = facets.at(leaf.source_facet).frame;
        regions[{leaf.source_facet, leaf.state}].push_back(projected_polygon(frame, {leaf.vertices.begin(), leaf.vertices.end()}));
    }
    for (auto& [key, polygons] : regions)
        overlay_region(facets[key.first].cells, union_(polygons), channel, key.second);
}

void overlay_tracery(std::vector<FacetPartition>& facets, const TriangleSelector::Tracery2DData& regions, size_t channel = 0, bool precise = false)
{
    for (const auto& input : regions) {
        FacetPartition& facet = facets.at(input.source_facet_id);
        facet.protect_corners = facet.protect_corners || !input.regions.empty();
        // Group before overlaying: work-triangle seams within one fragment are
        // not material boundaries and must not survive as tiny sliver regions.
        std::map<std::pair<size_t, EnforcerBlockerType>, Polygons> grouped;
        for (const auto& region : input.regions) {
            Polygon polygon;
            for (const Vec2d& p : region.polygon) {
                const Vec3d point = input.origin + p.x() * input.axis_u + p.y() * input.axis_v;
                polygon.points.push_back(facet.frame.project(precise ? point : point.cast<float>().cast<double>()));
            }
            polygon.make_counter_clockwise();
            if (!polygon.empty())
                grouped[{region.fragment_id, region.color}].push_back(std::move(polygon));
        }
        for (auto& [key, polygons] : grouped)
            overlay_region(facet.cells, union_(polygons), channel, key.second);
        facet.needs_partition = facet.needs_partition || facet.cells.size() > 1;
    }
}

// All edge cuts are keyed by original vertex IDs, never merely by position:
// disconnected coincident shells must remain disconnected.
struct EdgeCuts
{
    std::map<Edge, std::vector<double>> parameters;
    std::map<Edge, std::vector<int>>    vertices;
    std::map<Edge, double>             tolerances;
};

bool on_segment(const Vec3d& point, const Vec3d& a, const Vec3d& b, double& t, double distance_tolerance = weld_tolerance)
{
    const Vec3d delta = b - a;
    if (delta.squaredNorm() < 1e-24)
        return false;
    t                      = (point - a).dot(delta) / delta.squaredNorm();
    const double tolerance = distance_tolerance / delta.norm();
    return t >= -tolerance && t <= 1. + tolerance && (point - a - t * delta).norm() <= distance_tolerance;
}

void collect_edge_cuts(const indexed_triangle_set& mesh, const std::vector<FacetPartition>& facets, EdgeCuts& cuts)
{
    for (size_t i = 0; i < facets.size(); ++i) {
        const auto& triangle = mesh.indices[i];
        const auto& frame = facets[i].frame;
        const double edge_tolerance = std::min(weld_tolerance, unscale<double>(frame.vertex_tolerance) / frame.coordinate_scale);
        for (int e = 0; e < 3; ++e) {
            const Edge edge = std::minmax(triangle[e], triangle[(e + 1) % 3]);
            const auto [it, inserted] = cuts.tolerances.emplace(edge, edge_tolerance);
            if (!inserted)
                it->second = std::min(it->second, edge_tolerance);
        }
        // An untouched facet has no new cuts. Projecting and lifting its
        // existing corners can manufacture near-endpoint cuts, especially when
        // an adjacent sliver requires a much smaller edge tolerance.
        if (!facets[i].needs_partition)
            continue;
        for (const Cell& cell : facets[i].cells) {
            for (const Point& point : to_points(ExPolygons{cell.polygon})) {
                const Vec3d world = facets[i].frame.lift(point);
                for (int e = 0; e < 3; ++e) {
                    const Edge edge = std::minmax(triangle[e], triangle[(e + 1) % 3]);
                    double     t;
                    if (on_segment(world, mesh.vertices[edge.first].cast<double>(), mesh.vertices[edge.second].cast<double>(), t,
                                   edge_tolerance))
                        cuts.parameters[edge].push_back(std::clamp(t, 0., 1.));
                }
            }
        }
    }
}

void allocate_edge_vertices(const indexed_triangle_set& source, EdgeCuts& cuts, indexed_triangle_set& output)
{
    output.vertices = source.vertices;
    // Keep each cut attached to its original edge. Cuts on opposite sides of
    // a sub-micron sliver may have equal float coordinates without being the
    // same topological vertex. Collapse actual zero-length edges only after
    // triangulation has established which vertices are connected.
    for (auto& [edge, parameters] : cuts.parameters) {
        parameters.push_back(0.);
        parameters.push_back(1.);
        std::sort(parameters.begin(), parameters.end());
        const Vec3d         a         = source.vertices[edge.first].cast<double>();
        const Vec3d         delta     = source.vertices[edge.second].cast<double>() - a;
        const double        tolerance = cuts.tolerances.at(edge) / delta.norm();
        std::vector<double> unique;
        for (double t : parameters) {
            t = t < tolerance ? 0. : (t > 1. - tolerance ? 1. : t);
            if (unique.empty() || t - unique.back() > tolerance)
                unique.push_back(t);
        }
        parameters = std::move(unique);
        auto& ids  = cuts.vertices[edge];
        for (double t : parameters) {
            const Vec3f position = (a + t * delta).cast<float>();
            // A nonzero double parameter may still round to an existing float
            // endpoint. Resolve that identity, including the far endpoint
            // which has not been visited yet.
            int index = position == source.vertices[edge.first] ? edge.first :
                        (position == source.vertices[edge.second] ? edge.second : -1);
            if (index < 0) {
                index = int(output.vertices.size());
                output.vertices.push_back(position);
            }
            if (ids.empty() || ids.back() != index)
                ids.push_back(index);
        }
    }
}

void insert_collinear_points(Polygon& polygon, const Points& candidates, double maximum_tolerance, bool protect_corners)
{
    Points result;
    for (size_t i = 0; i < polygon.points.size(); ++i) {
        const Point& a       = polygon.points[i];
        const Point& b       = polygon.points[(i + 1) % polygon.points.size()];
        const Vec2d  delta   = (b - a).cast<double>();
        const double length2 = delta.squaredNorm();
        if (length2 == 0.)
            continue;
        const double                          end_tolerance = 1.5 / std::sqrt(length2);
        std::vector<std::pair<double, Point>> along{{0., a}};
        for (const Point& p : candidates) {
            const double t = (p - a).cast<double>().dot(delta) / length2;
            if (t <= end_tolerance || t >= 1. - end_tolerance)
                continue;
            const double distance2 = ((p - a).cast<double>() - t * delta).squaredNorm();
            if (distance2 > maximum_tolerance * maximum_tolerance)
                continue;
            // Near an acute corner a point on one edge may also be within the
            // welding tolerance of the other edge. Inserting it into both
            // folds the contour back over itself and union cleanup removes
            // the corner. A cut belongs only to its closest contour edge.
            bool closer_edge = false;
            for (size_t j = 0; protect_corners && j < polygon.points.size(); ++j) {
                if (j == i)
                    continue;
                const Vec2d q = (p - polygon.points[j]).cast<double>();
                const Vec2d edge = (polygon.points[(j + 1) % polygon.points.size()] - polygon.points[j]).cast<double>();
                const double edge_length2 = edge.squaredNorm();
                const double parameter = edge_length2 > 0. ? std::clamp(q.dot(edge) / edge_length2, 0., 1.) : 0.;
                if ((q - parameter * edge).squaredNorm() + 1e-12 < distance2) {
                    closer_edge = true;
                    break;
                }
            }
            if (!closer_edge)
                along.emplace_back(t, p);
        }
        std::sort(along.begin(), along.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
        for (const auto& entry : along)
            if (result.empty() || result.back() != entry.second)
                result.push_back(entry.second);
    }
    polygon.points = std::move(result);
}

void clean_partition_cells(std::vector<Cell>& cells)
{
    // Hole contours and nested islands must be cleaned together by annotation.
    std::map<States, Polygons> groups;
    for (const Cell& cell : cells) {
        Polygons polygons = to_polygons(ExPolygons{cell.polygon});
        auto&    group    = groups[cell.states];
        group.insert(group.end(), std::make_move_iterator(polygons.begin()), std::make_move_iterator(polygons.end()));
    }
    std::vector<Cell> cleaned;
    for (const auto& [states, polygons] : groups) {
        ClipperLib::Clipper clipper;
        clipper.StrictlySimple(true);
        clipper.PreserveCollinear(true);
        clipper.AddPaths(ClipperUtils::PolygonsProvider(polygons), ClipperLib::ptSubject, true);
        ClipperLib::PolyTree tree;
        clipper.Execute(ClipperLib::ctUnion, tree, ClipperLib::pftNonZero, ClipperLib::pftNonZero);
        ExPolygons simple;
        traverse_pt(tree.Childs, &simple);
        for (ExPolygon& polygon : simple)
            cleaned.push_back({std::move(polygon), states});
    }
    cells = std::move(cleaned);
}

void allocate_partition_vertex(const Point& point, const FacetFrame& frame, std::map<Point, int>& vertices, indexed_triangle_set& output)
{
    if (vertices.count(point))
        return;
    const Vec3f position = frame.lift(point).cast<float>();
    // Planar proximity is conditioned on this facet's width. Equal float
    // positions alone cannot identify vertices on opposite sides of a sliver.
    int    nearest  = -1;
    double distance = std::pow(frame.vertex_tolerance, 2);
    for (const auto& [candidate, id] : vertices) {
        const double d = (candidate - point).cast<double>().squaredNorm();
        if (d <= distance) {
            distance = d;
            nearest  = id;
        }
    }
    if (nearest < 0) {
        nearest = int(output.vertices.size());
        output.vertices.push_back(position);
    }
    vertices[point] = nearest;
}

std::map<Point, int> synchronize_facet_vertices(
    const indexed_triangle_set& source, size_t facet_id, FacetPartition& facet, const EdgeCuts& cuts, indexed_triangle_set& output)
{
    std::map<Point, int> vertices;
    const auto&          triangle = source.indices[facet_id];
    for (int e = 0; e < 3; ++e) {
        const Edge edge  = std::minmax(triangle[e], triangle[(e + 1) % 3]);
        const auto found = cuts.vertices.find(edge);
        if (found == cuts.vertices.end())
            continue;
        for (int id : found->second)
            vertices[facet.frame.project(output.vertices[id].cast<double>())] = id;
    }
    // Snap numerical discrepancies at a source edge to the canonical edge cut.
    for (Cell& cell : facet.cells) {
        auto snap = [&](Polygon& polygon) {
            for (Point& point : polygon.points) {
                auto   nearest  = vertices.end();
                double distance = std::pow(facet.frame.vertex_tolerance, 2);
                for (auto it = vertices.begin(); it != vertices.end(); ++it) {
                    const double d = (it->first - point).cast<double>().squaredNorm();
                    if (d <= distance) {
                        nearest  = it;
                        distance = d;
                    }
                }
                if (nearest != vertices.end())
                    point = nearest->first;
                else {
                    vertices[point] = int(output.vertices.size());
                    output.vertices.push_back(facet.frame.lift(point).cast<float>());
                }
            }
        };
        snap(cell.polygon.contour);
        for (Polygon& hole : cell.polygon.holes)
            snap(hole);
    }
    if (!facet.protect_corners)
        clean_partition_cells(facet.cells);
    for (const Cell& cell : facet.cells)
        for (const Point& p : to_points(ExPolygons{cell.polygon}))
            allocate_partition_vertex(p, facet.frame, vertices, output);
    Points points;
    for (const auto& [point, id] : vertices)
        points.push_back(point);
    const Vec3d a = source.vertices[triangle[0]].cast<double>();
    const Vec3d b = source.vertices[triangle[1]].cast<double>() - a;
    const Vec3d c = source.vertices[triangle[2]].cast<double>() - a;
    const double altitude = b.cross(c).norm() / std::max({b.norm(), c.norm(), (b - c).norm()});
    // Keep opposite edges of an already subdivided, narrow source facet apart.
    const double insertion_tolerance =
        scale_(altitude < 20. * weld_tolerance ? altitude * 0.05 : weld_tolerance) * facet.frame.coordinate_scale;
    const bool protect_corners = facet.protect_corners;
    for (Cell& cell : facet.cells) {
        insert_collinear_points(cell.polygon.contour, points, insertion_tolerance, protect_corners);
        for (Polygon& hole : cell.polygon.holes)
            insert_collinear_points(hole, points, insertion_tolerance, protect_corners);
        auto& holes = cell.polygon.holes;
        holes.erase(std::remove_if(holes.begin(), holes.end(), [](const Polygon& hole) { return hole.area() == 0.; }), holes.end());
    }
    // Snapping may collapse a tiny clipped corner to a single vertex.
    // Such a cell has no area or material contribution; do not pass its empty
    // contour to face classification (projection_onto requires a first point).
    facet.cells.erase(std::remove_if(facet.cells.begin(), facet.cells.end(), [](const Cell& cell) {
        return cell.polygon.contour.points.size() < 3 || cell.polygon.contour.area() == 0.;
    }), facet.cells.end());
    if (!facet.protect_corners)
        clean_partition_cells(facet.cells);
    for (const Cell& cell : facet.cells)
        for (const Point& p : to_points(ExPolygons{cell.polygon}))
            allocate_partition_vertex(p, facet.frame, vertices, output);
    return vertices;
}

using PartitionKernel        = CGAL::Exact_predicates_exact_constructions_kernel;
using PartitionVertex        = CGAL::Triangulation_vertex_base_with_info_2<int, PartitionKernel>;
using PartitionFace          = CGAL::Constrained_triangulation_face_base_2<PartitionKernel>;
using PartitionData          = CGAL::Triangulation_data_structure_2<PartitionVertex, PartitionFace>;
using PartitionTriangulation = CGAL::Constrained_Delaunay_triangulation_2<PartitionKernel, PartitionData, CGAL::Exact_intersections_tag>;

PartitionTriangulation triangulate_shared_boundaries(const FacetPartition& facet,
                                                     std::map<Point, int>& vertices,
                                                     indexed_triangle_set& output,
                                                     const Vec3i32& original,
                                                     EdgeCuts& cuts,
                                                     std::array<PartitionKernel::Point_2, 3>& domain)
{
    // One triangulation owns all colors, so their shared edges cannot overlap.
    PartitionTriangulation                               triangulation;
    std::map<int, PartitionTriangulation::Vertex_handle> handles;
    std::map<PartitionKernel::Point_2, int>              ids;
    struct BoundaryConstraint
    {
        Edge edge;
        PartitionKernel::Point_2 a, b;
    };
    std::vector<BoundaryConstraint> boundary_constraints;
    const auto projected = [&](int id) {
        const Vec3d delta = output.vertices[id].cast<double>() - facet.frame.origin;
        return PartitionKernel::Point_2(scale_(delta.dot(facet.frame.u)) * facet.frame.coordinate_scale,
                                        scale_(delta.dot(facet.frame.v)) * facet.frame.coordinate_scale);
    };
    // Integer clipping coordinates can alias an original corner and a nearby
    // cut from a thin neighbour. The CDT uses exact projected coordinates, so
    // retain all boundary identities even if the contour lookup kept only one.
    std::set<int> vertex_ids{original[0], original[1], original[2]};
    for (const auto& [point, id] : vertices)
        vertex_ids.insert(id);
    for (int e = 0; e < 3; ++e) {
        const auto found = cuts.vertices.find(std::minmax(original[e], original[(e + 1) % 3]));
        if (found != cuts.vertices.end())
            vertex_ids.insert(found->second.begin(), found->second.end());
    }
    for (int id : vertex_ids) {
        PartitionKernel::Point_2 position = projected(id);
        if (id != original[0] && id != original[1] && id != original[2]) {
            for (int e = 0; e < 3; ++e) {
                const Edge edge = std::minmax(original[e], original[(e + 1) % 3]);
                const auto found = cuts.vertices.find(edge);
                if (found == cuts.vertices.end() || std::find(found->second.begin(), found->second.end(), id) == found->second.end())
                    continue;
                const Vec3d a = output.vertices[edge.first].cast<double>();
                const Vec3d delta = output.vertices[edge.second].cast<double>() - a;
                const double t = std::clamp((output.vertices[id].cast<double>() - a).dot(delta) / delta.squaredNorm(), 0., 1.);
                const auto start = projected(edge.first), end = projected(edge.second);
                // Construct the cut on the exact source edge, not on the
                // slightly bent polyline produced by float storage.
                position = start + PartitionKernel::FT(t) * (end - start);
                break;
            }
        }
        handles[id]   = triangulation.insert(position);
        ids[position] = id;
    }
    for (const Cell& cell : facet.cells) {
        for (const Polygon& polygon : to_polygons(ExPolygons{cell.polygon})) {
            for (size_t i = 0; i < polygon.points.size(); ++i) {
                const auto a = handles.at(vertices.at(polygon.points[i]));
                const auto b = handles.at(vertices.at(polygon.points[(i + 1) % polygon.points.size()]));
                if (a != b) {
                    const int ia = vertices.at(polygon.points[i]);
                    const int ib = vertices.at(polygon.points[(i + 1) % polygon.points.size()]);
                    for (int e = 0; e < 3; ++e) {
                        const Edge edge = std::minmax(original[e], original[(e + 1) % 3]);
                        const auto cut = cuts.vertices.find(edge);
                        if (cut != cuts.vertices.end() &&
                            std::find(cut->second.begin(), cut->second.end(), ia) != cut->second.end() &&
                            std::find(cut->second.begin(), cut->second.end(), ib) != cut->second.end())
                            boundary_constraints.push_back({edge, a->point(), b->point()});
                    }
                    triangulation.insert_constraint(a, b);
                }
            }
        }
    }
    // Constrain every source-edge interval explicitly. Interior color
    // constraints must not replace the facet boundary or expand its domain.
    for (int e = 0; e < 3; ++e) {
        const Edge edge = std::minmax(original[e], original[(e + 1) % 3]);
        const auto found = cuts.vertices.find(edge);
        if (found == cuts.vertices.end())
            continue;
        auto boundary = found->second;
        const Vec3d origin = output.vertices[edge.first].cast<double>();
        const Vec3d direction = output.vertices[edge.second].cast<double>() - origin;
        std::sort(boundary.begin(), boundary.end(), [&](int lhs, int rhs) {
            return (output.vertices[lhs].cast<double>() - origin).dot(direction) <
                   (output.vertices[rhs].cast<double>() - origin).dot(direction);
        });
        boundary.erase(std::unique(boundary.begin(), boundary.end()), boundary.end());
        if (original[e] != edge.first)
            std::reverse(boundary.begin(), boundary.end());
        for (size_t i = 1; i < boundary.size(); ++i)
            if (handles.count(boundary[i - 1]) && handles.count(boundary[i]) && handles.at(boundary[i - 1]) != handles.at(boundary[i])) {
                const auto a = handles.at(boundary[i - 1]), b = handles.at(boundary[i]);
                boundary_constraints.push_back({edge, a->point(), b->point()});
                triangulation.insert_constraint(a, b);
            }
    }
    for (int e = 0; e < 3; ++e)
        domain[e] = projected(original[e]);
    for (auto vertex = triangulation.finite_vertices_begin(); vertex != triangulation.finite_vertices_end(); ++vertex) {
        const auto found = ids.find(vertex->point());
        if (found != ids.end())
            vertex->info() = found->second;
        else {
            // Every distinct CDT intersection keeps its own identity. Rounding
            // back into the integer contour lookup may alias another exact
            // intersection and fold an otherwise valid triangulation.
            const double x = CGAL::to_double(vertex->point().x()) * SCALING_FACTOR / facet.frame.coordinate_scale;
            const double y = CGAL::to_double(vertex->point().y()) * SCALING_FACTOR / facet.frame.coordinate_scale;
            vertex->info() = int(output.vertices.size());
            output.vertices.push_back((facet.frame.origin + x * facet.frame.u + y * facet.frame.v).cast<float>());
            // Keep the provenance of new constraint intersections before
            // lifting them back to float coordinates. On very narrow facets,
            // float rounding can move a boundary point farther than the
            // facet-conditioned weld tolerance.
            for (const auto& boundary : boundary_constraints) {
                if (PartitionKernel::Segment_2(boundary.a, boundary.b).has_on(vertex->point()))
                    cuts.vertices[boundary.edge].push_back(vertex->info());
            }
        }
    }
    return triangulation;
}

const States& partition_states_at(const FacetPartition& facet, const Point& point)
{
    const Cell* nearest  = nullptr;
    double      distance = std::numeric_limits<double>::max();
    for (const Cell& cell : facet.cells) {
        if (cell.polygon.contour.points.size() < 3 || cell.polygon.contour.area() == 0.)
            continue;
        if (cell.polygon.contains(point))
            return cell.states;
        for (const Polygon& polygon : to_polygons(ExPolygons{cell.polygon})) {
            if (polygon.points.empty())
                continue;
            const double d = point.distance_to(point.projection_onto(polygon));
            if (d < distance) {
                distance = d;
                nearest  = &cell;
            }
        }
    }
    if (!nearest || distance > scale_(weld_tolerance) * 2. * facet.frame.coordinate_scale)
        throw std::runtime_error("Uncovered region in exact tracery partition: cells=" + std::to_string(facet.cells.size()) +
                                 " distance=" + std::to_string(distance));
    return nearest->states;
}

void triangulate_facet_partition(const indexed_triangle_set& source,
                                 size_t                      id,
                                 FacetPartition&             facet,
                                 EdgeCuts&                   cuts,
                                 TraceryPartitionResult&     result,
                                 std::vector<States>&        states,
                                 TraceryPerformanceScope&    performance)
{
    // Unchanged, uniformly annotated facets keep their original geometry.
    // This also preserves sub-micron slivers across subsequent brush strokes.
    if (!facet.needs_partition) {
        result.mesh.indices.push_back(source.indices[id]);
        result.source_facets.push_back(int(id));
        states.push_back(facet.cells.front().states);
        return;
    }
    auto vertices = [&] {
        TraceryPerformanceScope::Sample sample(performance, "geometry.facet_vertex_sync");
        return synchronize_facet_vertices(source, id, facet, cuts, result.mesh);
    }();
    std::array<PartitionKernel::Point_2, 3> domain;
    auto triangulation = [&] {
        TraceryPerformanceScope::Sample sample(performance, "geometry.constrained_triangulation");
        return triangulate_shared_boundaries(facet, vertices, result.mesh, source.indices[id], cuts, domain);
    }();
    TraceryPerformanceScope::Sample classify(performance, "geometry.emit_and_classify_faces");
    const Vec3i32 original      = source.indices[id];
    const Vec3d   normal        = (source.vertices[original[1]].cast<double>() - source.vertices[original[0]].cast<double>())
                                      .cross(source.vertices[original[2]].cast<double>() - source.vertices[original[0]].cast<double>());
    double        area          = 0.;
    std::set<std::array<int, 3>> emitted;
    const PartitionKernel::Triangle_2 domain_triangle(domain[0], domain[1], domain[2]);
    for (auto face = triangulation.finite_faces_begin(); face != triangulation.finite_faces_end(); ++face) {
        const auto center2 = CGAL::centroid(face->vertex(0)->point(), face->vertex(1)->point(), face->vertex(2)->point());
        if (domain_triangle.has_on_unbounded_side(center2))
            continue;
        Vec3i32            t(face->vertex(0)->info(), face->vertex(1)->info(), face->vertex(2)->info());
        std::array<int, 3> key{t[0], t[1], t[2]};
        std::sort(key.begin(), key.end());
        if (!emitted.insert(key).second)
            continue;
        const Vec3d a     = result.mesh.vertices[t[0]].cast<double>();
        const Vec3d b     = result.mesh.vertices[t[1]].cast<double>();
        const Vec3d c     = result.mesh.vertices[t[2]].cast<double>();
        const Vec3d cross = (b - a).cross(c - a);
        // The CDT winding follows the source frame. Float storage can flip
        // the apparent normal of a sliver; reversing that face independently
        // would give it the same directed edge as its neighbour.
        const Point center = facet.frame.project((a + b + c) / 3.);
        try {
            states.push_back(partition_states_at(facet, center));
        } catch (const std::runtime_error& error) {
            throw std::runtime_error(std::string(error.what()) + " source_facet=" + std::to_string(id));
        }
        result.mesh.indices.push_back(t);
        result.source_facets.push_back(int(id));
        area += cross.norm();
    }
    double perimeter = 0.;
    for (int e = 0; e < 3; ++e)
        perimeter += (source.vertices[original[e]] - source.vertices[original[(e + 1) % 3]]).norm();
    // Integer clipping and canonical edge welding have an absolute positional
    // tolerance. An area-relative-only test incorrectly rejects thin facets.
    if (std::abs(area - normal.norm()) > std::max(1e-7, 2. * weld_tolerance * perimeter))
        throw std::runtime_error("Exact tracery partition failed area conservation: facet=" + std::to_string(id) +
                                 " expected=" + std::to_string(normal.norm()) + " actual=" + std::to_string(area));
}

void build_partition_annotations(TraceryPartitionResult& result, const std::vector<States>& states, size_t channels,
                                 TraceryPerformanceScope& performance)
{
    const TriangleMesh mesh(result.mesh);
    for (size_t channel = 0; channel < channels; ++channel) {
        auto selector = [&] {
            TraceryPerformanceScope::Sample sample(performance, "geometry.annotation_selector_construction");
            return TriangleSelector(mesh);
        }();
        {
            TraceryPerformanceScope::Sample sample(performance, "geometry.annotation_assign_states");
            for (size_t triangle = 0; triangle < states.size(); ++triangle)
                if (states[triangle][channel] != EnforcerBlockerType::NONE)
                    selector.set_facet(int(triangle), states[triangle][channel]);
        }
        {
            TraceryPerformanceScope::Sample sample(performance, "geometry.annotation_serialize");
            result.annotations.push_back(selector.serialize());
        }
    }
}

void unify_shared_vertex_ids(const indexed_triangle_set& source,
                             EdgeCuts&                   cuts,
                             TraceryPartitionResult&     result,
                             bool                        resolve_rounded_boundary = false)
{
    // Projection/triangulation may rediscover a canonical cut under another ID.
    // Join only identical stored positions incident to the same source edge;
    // disconnected coincident shells must not be welded together.
    using Position = std::array<float, 3>;
    std::map<std::pair<Edge, Position>, int> shared;
    std::vector<int>                         parent(result.mesh.vertices.size());
    std::iota(parent.begin(), parent.end(), 0);
    const auto root = [&](int id) {
        while (parent[id] != id) {
            parent[id] = parent[parent[id]];
            id         = parent[id];
        }
        return id;
    };
    // Only exact boundary provenance establishes incidence. A geometric
    // tolerance test can assign a sliver's interior or opposite-edge point to
    // this edge, even when its float position happens to be identical.
    for (const auto& [edge, ids] : cuts.vertices) {
        for (int vertex : ids) {
            const Vec3f& position     = result.mesh.vertices[vertex];
            const auto [it, inserted] = shared.emplace(std::make_pair(edge, Position{position.x(), position.y(), position.z()}), vertex);
            if (!inserted) {
                const int a = root(vertex), b = root(it->second);
                parent[std::max(a, b)] = std::min(a, b);
            }
        }
    }
    // A constraint intersection may round exactly onto a corner of its own
    // source facet. Retain that source identity even if the exact CDT placed
    // the intersection just across the boundary and emitted no connecting edge.
    for (size_t i = 0; i < result.mesh.indices.size(); ++i) {
        const auto& original = source.indices[result.source_facets[i]];
        for (int vertex : result.mesh.indices[i]) {
            if (size_t(vertex) < source.vertices.size())
                continue;
            for (int corner : original)
                if (result.mesh.vertices[vertex] == source.vertices[corner]) {
                    const int a = root(vertex), b = root(corner);
                    parent[std::max(a, b)] = std::min(a, b);
                }
        }
    }
    if (resolve_rounded_boundary) {
        // Likewise, an interior CDT vertex may round onto a known boundary cut.
        // Resolve only an unambiguous boundary identity within its owning source
        // facet. Coincident cuts on opposite edges of a sliver remain distinct.
        std::map<int, std::map<int, unsigned>> incidence;
        for (const auto& triangle : result.mesh.indices)
            for (int e = 0; e < 3; ++e) {
                const int a = root(triangle[e]), b = root(triangle[(e + 1) % 3]);
                if (a != b) {
                    ++incidence[a][b];
                    ++incidence[b][a];
                }
            }
        std::map<int, std::map<Position, std::set<int>>> facet_boundaries;
        for (size_t i = 0; i < result.mesh.indices.size(); ++i) {
            const int source_id    = result.source_facets[i];
            auto [entry, inserted] = facet_boundaries.try_emplace(source_id);
            auto& positions        = entry->second;
            if (inserted) {
                const auto& original = source.indices[source_id];
                auto        add      = [&](int id) {
                    const Vec3f& p = result.mesh.vertices[id];
                    positions[{p.x(), p.y(), p.z()}].insert(root(id));
                };
                for (int corner : original)
                    add(corner);
                for (int e = 0; e < 3; ++e) {
                    auto found = cuts.vertices.find(std::minmax(original[e], original[(e + 1) % 3]));
                    if (found != cuts.vertices.end())
                        for (int id : found->second)
                            add(id);
                }
            }
            for (int vertex : result.mesh.indices[i]) {
                if (size_t(vertex) < source.vertices.size())
                    continue;
                const Vec3f& p     = result.mesh.vertices[vertex];
                const auto   found = positions.find({p.x(), p.y(), p.z()});
                if (found != positions.end() && found->second.size() == 1) {
                    const int a = std::min(root(vertex), root(*found->second.begin()));
                    const int b = std::max(root(vertex), root(*found->second.begin()));
                    if (a == b || incidence[a].count(b))
                        continue;
                    bool manifold = true;
                    for (const auto& [neighbor, owners] : incidence[b]) {
                        const auto existing = incidence[a].find(neighbor);
                        if (existing != incidence[a].end() && owners + existing->second > 2) {
                            manifold = false;
                            break;
                        }
                    }
                    if (!manifold)
                        continue;
                    for (const auto& [neighbor, owners] : incidence[b]) {
                        incidence[a][neighbor] += owners;
                        incidence[neighbor][a] += owners;
                        incidence[neighbor].erase(b);
                    }
                    incidence.erase(b);
                    parent[b] = a;
                }
            }
        }
    }
    for (auto& triangle : result.mesh.indices)
        for (int& vertex : triangle)
            vertex = root(vertex);
    for (auto& [edge, ids] : cuts.vertices) {
        for (int& vertex : ids)
            vertex = root(vertex);
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    }
    for (auto& [edge, ids] : cuts.vertices) {
        const Vec3d a     = source.vertices[edge.first].cast<double>();
        const Vec3d delta = source.vertices[edge.second].cast<double>() - a;
        std::sort(ids.begin(), ids.end(), [&](int lhs, int rhs) {
            const double l = (result.mesh.vertices[lhs].cast<double>() - a).dot(delta);
            const double r = (result.mesh.vertices[rhs].cast<double>() - a).dot(delta);
            return l == r ? lhs < rhs : l < r;
        });
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    }
}

// Float conversion may collapse a real CDT edge. Merge only its connected
// endpoints, then remove the repeated-index faces together with their states.
void collapse_zero_length_edges(TraceryPartitionResult& result, EdgeCuts& cuts, std::vector<States>& states)
{
    std::vector<int> parent(result.mesh.vertices.size());
    std::iota(parent.begin(), parent.end(), 0);
    const auto root = [&](int vertex) {
        while (parent[vertex] != vertex) {
            parent[vertex] = parent[parent[vertex]];
            vertex         = parent[vertex];
        }
        return vertex;
    };
    for (const auto& triangle : result.mesh.indices) {
        for (int e = 0; e < 3; ++e) {
            const int a = root(triangle[e]), b = root(triangle[(e + 1) % 3]);
            if (result.mesh.vertices[a] == result.mesh.vertices[b])
                parent[std::max(a, b)] = std::min(a, b);
        }
    }
    for (auto& triangle : result.mesh.indices)
        for (int& vertex : triangle)
            vertex = root(vertex);
    for (auto& [edge, ids] : cuts.vertices) {
        for (int& vertex : ids)
            vertex = root(vertex);
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    }
    size_t out = 0;
    for (size_t i = 0; i < result.mesh.indices.size(); ++i) {
        const Vec3i32 triangle = result.mesh.indices[i];
        if (triangle[0] == triangle[1] || triangle[1] == triangle[2] || triangle[0] == triangle[2])
            continue;
        result.mesh.indices[out]  = triangle;
        result.source_facets[out] = result.source_facets[i];
        if (out != i)
            states[out] = std::move(states[i]);
        ++out;
    }
    result.mesh.indices.resize(out);
    result.source_facets.resize(out);
    states.resize(out);
}

void enforce_source_edge_cuts(const indexed_triangle_set& source,
                              const EdgeCuts&             cuts,
                              TraceryPartitionResult&     result,
                              std::vector<States>&        states)
{
    std::vector<Vec3i32>                   triangles;
    std::vector<int>                       sources;
    std::vector<States>                    colors;
    std::map<std::pair<int, Edge>, size_t> edge_owners;
    for (size_t i = 0; i < result.mesh.indices.size(); ++i)
        for (int e = 0; e < 3; ++e)
            ++edge_owners[{result.source_facets[i], std::minmax(result.mesh.indices[i][e], result.mesh.indices[i][(e + 1) % 3])}];
    for (size_t i = 0; i < result.mesh.indices.size(); ++i) {
        const auto&          original = source.indices[result.source_facets[i]];
        std::vector<Vec3i32> pieces{result.mesh.indices[i]};
        for (int e = 0; e < 3; ++e) {
            const auto found = cuts.vertices.find(std::minmax(original[e], original[(e + 1) % 3]));
            if (found == cuts.vertices.end())
                continue;
            const auto&          ids = found->second;
            std::vector<Vec3i32> split;
            for (const Vec3i32& triangle : pieces) {
                bool divided = false;
                for (int side = 0; side < 3 && !divided; ++side) {
                    const auto owners = edge_owners.find({result.source_facets[i], std::minmax(triangle[side], triangle[(side + 1) % 3])});
                    if (owners != edge_owners.end() && owners->second > 1)
                        continue;
                    const auto a = std::find(ids.begin(), ids.end(), triangle[side]);
                    const auto b = std::find(ids.begin(), ids.end(), triangle[(side + 1) % 3]);
                    if (a == ids.end() || b == ids.end() || std::abs(b - a) <= 1)
                        continue;
                    // Collapsing a zero-length connection can put all three
                    // corners on this edge chain. The surviving wedge still
                    // joins its two sides: fanning through its own opposite
                    // corner would replace it with repeated-index faces and
                    // leave a hole. Truly collinear wedges are handled below
                    // by remove_collapsed_faces, which transfers their cuts.
                    if (std::find(ids.begin(), ids.end(), triangle[(side + 2) % 3]) != ids.end())
                        continue;
                    const int step = a < b ? 1 : -1;
                    for (auto point = a; point != b; point += step)
                        split.emplace_back(*point, *(point + step), triangle[(side + 2) % 3]);
                    divided = true;
                }
                if (!divided)
                    split.push_back(triangle);
            }
            pieces = std::move(split);
        }
        for (const Vec3i32& triangle : pieces) {
            triangles.push_back(triangle);
            sources.push_back(result.source_facets[i]);
            colors.push_back(states[i]);
        }
    }
    result.mesh.indices  = std::move(triangles);
    result.source_facets = std::move(sources);
    states               = std::move(colors);
}

void remove_collapsed_faces(TraceryPartitionResult& result, std::vector<States>& states)
{
    // A collinear face represents an edge split, not an area to discard.
    // Transfer its middle vertex to the adjacent face before removing it;
    // otherwise dropping the face leaves a T-junction in the stored mesh.
    std::map<Edge, std::vector<int>> splits;
    std::vector<bool>                collapsed(result.mesh.indices.size(), false);
    for (size_t i = 0; i < result.mesh.indices.size(); ++i) {
        const auto& t = result.mesh.indices[i];
        const Vec3d a = result.mesh.vertices[t[0]].cast<double>();
        const Vec3d b = result.mesh.vertices[t[1]].cast<double>();
        const Vec3d c = result.mesh.vertices[t[2]].cast<double>();
        if ((b - a).cross(c - a).squaredNorm() != 0.)
            continue;
        collapsed[i] = true;
        if (t[0] == t[1] || t[1] == t[2] || t[2] == t[0])
            continue;
        int    longest = 0;
        double length  = 0.;
        for (int e = 0; e < 3; ++e) {
            const double candidate = (result.mesh.vertices[t[e]].cast<double>() - result.mesh.vertices[t[(e + 1) % 3]].cast<double>())
                                         .squaredNorm();
            if (candidate > length) {
                length  = candidate;
                longest = e;
            }
        }
        splits[std::minmax(t[longest], t[(longest + 1) % 3])].push_back(t[(longest + 2) % 3]);
    }
    std::vector<Vec3i32> triangles;
    std::vector<int>     sources;
    std::vector<States>  colors;
    for (size_t i = 0; i < result.mesh.indices.size(); ++i) {
        if (collapsed[i])
            continue;
        std::vector<Vec3i32> pending{result.mesh.indices[i]};
        while (!pending.empty()) {
            const Vec3i32 triangle = pending.back();
            pending.pop_back();
            bool divided = false;
            for (int e = 0; e < 3 && !divided; ++e) {
                const auto split = splits.find(std::minmax(triangle[e], triangle[(e + 1) % 3]));
                if (split == splits.end())
                    continue;
                const Vec3d a      = result.mesh.vertices[triangle[e]].cast<double>();
                const Vec3d delta  = result.mesh.vertices[triangle[(e + 1) % 3]].cast<double>() - a;
                auto        points = split->second;
                std::sort(points.begin(), points.end(), [&](int lhs, int rhs) {
                    return (result.mesh.vertices[lhs].cast<double>() - a).dot(delta) <
                           (result.mesh.vertices[rhs].cast<double>() - a).dot(delta);
                });
                points.erase(std::unique(points.begin(), points.end()), points.end());
                int previous = triangle[e];
                for (int point : points) {
                    const double parameter = (result.mesh.vertices[point].cast<double>() - a).dot(delta);
                    if (parameter <= 0. || parameter >= delta.squaredNorm())
                        throw std::runtime_error("Collapsed tracery face has coincident edge endpoints");
                    pending.emplace_back(previous, point, triangle[(e + 2) % 3]);
                    previous = point;
                }
                pending.emplace_back(previous, triangle[(e + 1) % 3], triangle[(e + 2) % 3]);
                divided = true;
            }
            if (!divided) {
                triangles.push_back(triangle);
                sources.push_back(result.source_facets[i]);
                colors.push_back(states[i]);
            }
        }
    }
    result.mesh.indices  = std::move(triangles);
    result.source_facets = std::move(sources);
    states               = std::move(colors);
}

// Collapsing a float-degenerate strip can make two CDT faces coincide. Cancel
// opposite orientations within the SAME source facet; never weld disconnected
// shells or discard a single unpaired boundary face.
void cancel_folded_faces(TraceryPartitionResult& result, std::vector<States>& states)
{
    using Key = std::pair<int, std::array<int, 3>>;
    std::map<Key, std::vector<size_t>> pending;
    std::vector<bool>                  removed(result.mesh.indices.size(), false);
    auto                               orientation = [](const Vec3i32& t) { return ((t[0] > t[1]) + (t[0] > t[2]) + (t[1] > t[2])) % 2; };
    for (size_t i = 0; i < result.mesh.indices.size(); ++i) {
        const auto&        t = result.mesh.indices[i];
        std::array<int, 3> ids{t[0], t[1], t[2]};
        std::sort(ids.begin(), ids.end());
        auto& matches  = pending[{result.source_facets[i], ids}];
        auto  opposite = std::find_if(matches.begin(), matches.end(),
                                      [&](size_t j) { return orientation(result.mesh.indices[j]) != orientation(t); });
        if (opposite != matches.end()) {
            removed[i] = removed[*opposite] = true;
            matches.erase(opposite);
        } else {
            matches.push_back(i);
        }
    }
    size_t out = 0;
    for (size_t i = 0; i < result.mesh.indices.size(); ++i) {
        if (removed[i])
            continue;
        result.mesh.indices[out]  = result.mesh.indices[i];
        result.source_facets[out] = result.source_facets[i];
        if (out != i)
            states[out] = std::move(states[i]);
        ++out;
    }
    result.mesh.indices.resize(out);
    result.source_facets.resize(out);
    states.resize(out);
}

// When a whole tip of a thin facet collapses, the surviving neighbours can
// describe the same boundary as A--B and A--C--B. Match those chains using
// existing, topologically shared endpoints only. No proximity weld is involved.
void conform_collapsed_boundary_chains(TraceryPartitionResult& result, std::vector<States>& states)
{
    for (unsigned pass = 0; pass < 64; ++pass) {
        std::map<Edge, std::pair<unsigned, size_t>> owners;
        for (size_t i = 0; i < result.mesh.indices.size(); ++i) {
            const auto& t = result.mesh.indices[i];
            for (int e = 0; e < 3; ++e) {
                auto& owner = owners[std::minmax(t[e], t[(e + 1) % 3])];
                ++owner.first;
                owner.second = i;
            }
        }
        std::map<int, std::vector<int>> boundary;
        for (const auto& [edge, count] : owners)
            if (count.first == 1) {
                boundary[edge.first].push_back(edge.second);
                boundary[edge.second].push_back(edge.first);
            }
        std::map<Edge, std::set<int>> splits;
        for (const auto& [origin, ends] : boundary) {
            const Vec3d a = result.mesh.vertices[origin].cast<double>();
            for (size_t i = 0; i < ends.size(); ++i)
                for (size_t j = i + 1; j < ends.size(); ++j) {
                    const Vec3d u = result.mesh.vertices[ends[i]].cast<double>() - a;
                    const Vec3d v = result.mesh.vertices[ends[j]].cast<double>() - a;
                    if (owners.at(std::minmax(origin, ends[i])).second == owners.at(std::minmax(origin, ends[j])).second)
                        continue;
                    if (u.dot(v) <= 0. || u.squaredNorm() == v.squaredNorm())
                        continue;
                    if (u.cross(v).squaredNorm() != 0.)
                        continue;
                    const int near = u.squaredNorm() < v.squaredNorm() ? ends[i] : ends[j];
                    const int far  = u.squaredNorm() < v.squaredNorm() ? ends[j] : ends[i];
                    splits[std::minmax(origin, far)].insert(near);
                }
        }
        if (splits.empty())
            return;
        std::vector<Vec3i32> triangles;
        std::vector<int>     sources;
        std::vector<States>  colours;
        for (size_t i = 0; i < result.mesh.indices.size(); ++i) {
            std::vector<Vec3i32> pending{result.mesh.indices[i]};
            while (!pending.empty()) {
                const auto triangle = pending.back();
                pending.pop_back();
                bool divided = false;
                for (int e = 0; e < 3 && !divided; ++e) {
                    auto found = splits.find(std::minmax(triangle[e], triangle[(e + 1) % 3]));
                    if (found == splits.end())
                        continue;
                    const Vec3d      a = result.mesh.vertices[triangle[e]].cast<double>();
                    const Vec3d      d = result.mesh.vertices[triangle[(e + 1) % 3]].cast<double>() - a;
                    std::vector<int> points(found->second.begin(), found->second.end());
                    std::sort(points.begin(), points.end(), [&](int l, int r) {
                        return (result.mesh.vertices[l].cast<double>() - a).dot(d) < (result.mesh.vertices[r].cast<double>() - a).dot(d);
                    });
                    int previous = triangle[e];
                    for (int point : points) {
                        pending.emplace_back(previous, point, triangle[(e + 2) % 3]);
                        previous = point;
                    }
                    pending.emplace_back(previous, triangle[(e + 1) % 3], triangle[(e + 2) % 3]);
                    divided = true;
                }
                if (!divided) {
                    triangles.push_back(triangle);
                    sources.push_back(result.source_facets[i]);
                    colours.push_back(states[i]);
                }
            }
        }
        result.mesh.indices  = std::move(triangles);
        result.source_facets = std::move(sources);
        states               = std::move(colours);
    }
    throw std::runtime_error("Exact tracery boundary chain synchronization did not converge");
}

void validate_partition_topology(const indexed_triangle_set& source, const TraceryPartitionResult& result)
{
    std::map<Edge, size_t> original_edges;
    for (const Vec3i32& triangle : source.indices)
        for (int e = 0; e < 3; ++e)
            ++original_edges[std::minmax(triangle[e], triangle[(e + 1) % 3])];
    std::map<Edge, std::vector<int>>    edges;
    std::map<std::pair<int, Edge>, int> internal_directions;
    std::map<std::array<int, 3>, int>   unique_faces;
    for (size_t i = 0; i < result.mesh.indices.size(); ++i) {
        const Vec3i32& triangle  = result.mesh.indices[i];
        const int      source_id = result.source_facets[i];
        const Vec3d    a         = result.mesh.vertices[triangle[0]].cast<double>();
        const Vec3d    b         = result.mesh.vertices[triangle[1]].cast<double>();
        const Vec3d    c         = result.mesh.vertices[triangle[2]].cast<double>();
        const Vec3d    normal    = (b - a).cross(c - a);
        if (normal.squaredNorm() == 0.)
            throw std::runtime_error("Exact tracery partition produced a collapsed triangle: source_facet=" + std::to_string(source_id));
        std::array<int, 3> face{triangle[0], triangle[1], triangle[2]};
        std::sort(face.begin(), face.end());
        const auto [previous, inserted] = unique_faces.emplace(face, source_id);
        if (!inserted)
            throw std::runtime_error(
                "Exact tracery partition produced a duplicate triangle: source_facets=" + std::to_string(previous->second) + "," +
                std::to_string(source_id) + " vertices=" + std::to_string(face[0]) + "," + std::to_string(face[1]) + "," +
                std::to_string(face[2]));
        for (int e = 0; e < 3; ++e) {
            const int  from = triangle[e], to = triangle[(e + 1) % 3];
            const Edge edge = std::minmax(from, to);
            edges[edge].push_back(source_id);
            internal_directions[{source_id, edge}] += from < to ? 1 : -1;
        }
    }
    for (const auto& [edge, facets] : edges) {
        if (facets.size() == 2) {
            if (facets[0] == facets[1] && internal_directions.at({facets[0], edge}) != 0)
                throw std::runtime_error("Exact tracery partition produced inconsistent internal winding");
            continue;
        }
        bool           preserved_boundary = false;
        const Vec3i32& original           = source.indices[facets.front()];
        for (int e = 0; e < 3; ++e) {
            const Edge  old_edge = std::minmax(original[e], original[(e + 1) % 3]);
            const Vec3d a        = source.vertices[old_edge.first].cast<double>();
            const Vec3d b        = source.vertices[old_edge.second].cast<double>();
            double      t;
            if (original_edges.at(old_edge) == facets.size() && on_segment(result.mesh.vertices[edge.first].cast<double>(), a, b, t) &&
                on_segment(result.mesh.vertices[edge.second].cast<double>(), a, b, t))
                preserved_boundary = true;
        }
        if (!preserved_boundary)
            throw std::runtime_error(
                "Exact tracery partition introduced an unmatched mesh edge: source_facet=" + std::to_string(facets.front()) +
                " edge=" + std::to_string(edge.first) + "," + std::to_string(edge.second) + " owners=" + std::to_string(facets.size()) +
                " length=" + std::to_string((result.mesh.vertices[edge.first] - result.mesh.vertices[edge.second]).norm()));
    }
}
} // namespace

TraceryPartitionResult build_exact_tracery_partition(const TriangleMesh&                                         source,
                                                     const std::vector<TriangleSelector::TriangleSplittingData>& annotations,
                                                     const TriangleSelector::Tracery2DData&                      regions,
                                                     const std::function<void()>&                                cancel,
                                                     const std::vector<TriangleSelector::Tracery2DData>&         channel_regions)
{
    TraceryPerformanceScope performance("exact_partition");
    performance.count("input_facets", source.its.indices.size());
    performance.count("annotation_channels", annotations.size());
    performance.count("region_source_facets", regions.size());
    if (performance.enabled()) {
        size_t count = 0;
        for (const auto& facet : regions)
            count += facet.regions.size();
        performance.count("input_regions", count);
    }
    performance.stage("geometry.initialize_facets");
    if (annotations.empty())
        throw std::invalid_argument("Exact tracery partition requires a paint channel");
    std::vector<FacetPartition> facets;
    for (size_t i = 0; i < source.its.indices.size(); ++i) {
        if (cancel)
            cancel();
        facets.emplace_back(source.its, int(i), annotations.size());
    }
    performance.stage("geometry.overlay_annotations");
    for (size_t channel = 0; channel < annotations.size(); ++channel) {
        if (cancel)
            cancel();
        overlay_annotation(facets, source, annotations[channel], channel);
    }
    performance.stage("geometry.overlay_tracery");
    overlay_tracery(facets, regions);
    if (channel_regions.size() > annotations.size())
        throw std::invalid_argument("Too many stained-glass annotation channels");
    for (size_t channel = 0; channel < channel_regions.size(); ++channel)
        overlay_tracery(facets, channel_regions[channel], channel, true);
    if (performance.enabled()) {
        size_t partitioned = 0, cells = 0;
        for (const auto& facet : facets) {
            partitioned += facet.needs_partition;
            cells += facet.cells.size();
        }
        performance.count("partitioned_facets", partitioned);
        performance.count("overlay_cells", cells);
    }
    performance.stage("geometry.collect_edge_cuts_initial");
    EdgeCuts cuts;
    collect_edge_cuts(source.its, facets, cuts);
    TraceryPartitionResult result;
    performance.stage("geometry.allocate_edge_vertices_initial");
    allocate_edge_vertices(source.its, cuts, result.mesh);
    performance.stage("geometry.presynchronize_vertices");
    // Cleaning legacy paint can introduce new intersections on a source edge.
    // Collect those cuts before the final cross-facet vertex allocation.
    for (size_t i = 0; i < facets.size(); ++i) {
        if (cancel)
            cancel();
        if (facets[i].needs_partition)
            synchronize_facet_vertices(source.its, i, facets[i], cuts, result.mesh);
    }
    performance.stage("geometry.collect_edge_cuts_final");
    cuts = {};
    collect_edge_cuts(source.its, facets, cuts);
    performance.stage("geometry.allocate_edge_vertices_final");
    allocate_edge_vertices(source.its, cuts, result.mesh);
    performance.stage("geometry.triangulate_facets_total");
    std::vector<States> states;
    for (size_t i = 0; i < facets.size(); ++i) {
        if (cancel)
            cancel();
        triangulate_facet_partition(source.its, i, facets[i], cuts, result, states, performance);
    }
    performance.stage("geometry.enforce_edge_cuts");
    unify_shared_vertex_ids(source.its, cuts, result);
    collapse_zero_length_edges(result, cuts, states);
    enforce_source_edge_cuts(source.its, cuts, result, states);
    collapse_zero_length_edges(result, cuts, states);
    remove_collapsed_faces(result, states);
    cancel_folded_faces(result, states);
    unify_shared_vertex_ids(source.its, cuts, result, true);
    collapse_zero_length_edges(result, cuts, states);
    remove_collapsed_faces(result, states);
    cancel_folded_faces(result, states);
    conform_collapsed_boundary_chains(result, states);
    performance.stage("geometry.validate_topology");
    validate_partition_topology(source.its, result);
    performance.stage("geometry.rebuild_annotations");
    build_partition_annotations(result, states, annotations.size(), performance);
    performance.count("output_facets", result.mesh.indices.size());
    performance.stage("geometry.cleanup");
    return result;
}

std::vector<Vec3f> clip_tracery_polygon_to_facet(const indexed_triangle_set& mesh, int facet, const std::vector<Vec3f>& polygon)
{
    const FacetPartition frame(mesh, facet, 1);
    const Polygon        projected = projected_polygon(frame.frame, polygon);
    if (projected.empty())
        return {};
    const ExPolygons clipped = intersection_ex(ExPolygons{frame.cells.front().polygon}, Polygons{projected});
    if (clipped.empty())
        return {};
    if (clipped.size() != 1 || !clipped.front().holes.empty())
        throw std::runtime_error("Expected a convex retained tracery fragment");
    std::vector<Vec3f> result;
    for (const Point& point : clipped.front().contour.points)
        result.push_back(frame.frame.lift(point).cast<float>());
    return result;
}
namespace detail {
struct TraceryFacetClipper::Impl
{
    FacetPartition facet;
    Impl(const indexed_triangle_set& mesh, int index) : facet(mesh, index, 1) {}
};
TraceryFacetClipper::TraceryFacetClipper(const indexed_triangle_set& mesh, int facet)
    : m_impl(std::make_unique<Impl>(mesh, facet)) {}
TraceryFacetClipper::~TraceryFacetClipper() = default;
size_t TraceryFacetClipper::estimated_bytes() const
{
    return sizeof(Impl) + sizeof(Cell) + 3 * sizeof(Point) + sizeof(EnforcerBlockerType);
}
std::vector<Vec3f> TraceryFacetClipper::clip(const std::vector<Vec3f>& polygon, TraceryMappingStatistics& statistics) const
{
    const auto& facet = m_impl->facet;
    const Polygon projected = projected_polygon(facet.frame, polygon);
    if (projected.empty()) return {};
    const auto& triangle = facet.cells.front().polygon.contour;
    // Inclusive containment is safe only for a nondegenerate convex input.
    // Check every supporting edge, not only successive turn signs: a self-
    // crossing polygon must still go through the legacy clipping/validation.
    bool convex = projected.area() > 0.;
    for (size_t i = 0; convex && i < projected.points.size(); ++i) {
        const Point& a = projected.points[i];
        const Point& b = projected.points[(i + 1) % projected.points.size()];
        for (const Point& p : projected.points)
            if (int128::orient(a, b, p) < 0) {
                convex = false;
                break;
            }
    }
    const auto inside = [](const Polygon& boundary, const Point& p) {
        for (size_t i = 0; i < boundary.points.size(); ++i)
            if (int128::orient(boundary.points[i], boundary.points[(i + 1) % boundary.points.size()], p) < 0)
                return false;
        return true;
    };
    const Polygon* inherited = nullptr;
    if (convex && triangle.area() > 0.) {
        const auto separated = [](const Polygon& boundary, const Polygon& other) {
            for (size_t i = 0; i < boundary.points.size(); ++i) {
                const auto& a = boundary.points[i];
                const auto& b = boundary.points[(i + 1) % boundary.points.size()];
                if (std::all_of(other.points.begin(), other.points.end(), [&](const Point& p) {
                        return int128::orient(a, b, p) < 0;
                    }))
                    return true;
            }
            return false;
        };
        if (separated(triangle, projected) || separated(projected, triangle)) {
            ++statistics.separated_candidates;
            return {};
        }
        if (std::all_of(projected.points.begin(), projected.points.end(), [&](const Point& p) { return inside(triangle, p); })) {
            inherited = &projected;
            ++statistics.inherited_regions;
        } else if (std::all_of(triangle.points.begin(), triangle.points.end(), [&](const Point& p) { return inside(projected, p); })) {
            inherited = &triangle;
            ++statistics.inherited_facets;
        }
    }
    ExPolygons clipped;
    if (!inherited) {
        ++statistics.clip_calls;
        clipped = intersection_ex(ExPolygons{facet.cells.front().polygon}, Polygons{projected});
        if (clipped.empty()) return {};
        if (clipped.size() != 1 || !clipped.front().holes.empty())
            throw std::runtime_error("Expected a convex retained tracery fragment");
        inherited = &clipped.front().contour;
    }
    std::vector<Vec3f> result;
    result.reserve(inherited->points.size());
    for (const Point& point : inherited->points)
        result.push_back(facet.frame.lift(point).cast<float>());
    return result;
}
} // namespace detail

std::vector<unsigned int> tracery_retained_paint_materials(const TriangleMesh& source,
    const TriangleSelector::TriangleSplittingData& paint, const TriangleSelector::Tracery2DData& replacement)
{
    // Use the same clipping / annotation path as the real bake, on a copy.
    // NONE marks only the pending replacement; its recipe is budgeted separately.
    auto cleared = replacement;
    for (auto& facet : cleared)
        for (auto& region : facet.regions)
            region.color = EnforcerBlockerType::NONE;
    const auto remaining = cleared.empty() ? paint : build_exact_tracery_partition(source, {paint}, cleared).annotations.front();
    std::vector<unsigned int> result;
    for (size_t state = 1; state < remaining.used_states.size(); ++state)
        if (remaining.used_states[state])
            result.push_back(unsigned(state));
    return result;
}
} // namespace Slic3r
