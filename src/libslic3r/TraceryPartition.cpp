#include "TraceryPartition.hpp"
#include "ClipperUtils.hpp"
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Constrained_Delaunay_triangulation_2.h>
#include <CGAL/Triangulation_vertex_base_with_info_2.h>
#include <algorithm>
#include <map>
#include <cmath>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>

namespace Slic3r {
namespace {
constexpr double weld_tolerance = 0.00001;
using Edge                      = std::pair<int, int>;
using States                    = std::vector<EnforcerBlockerType>;

struct FacetFrame
{
    Vec3d origin, u, v;
    explicit FacetFrame(const indexed_triangle_set& mesh, int facet)
    {
        const auto& t = mesh.indices.at(facet);
        origin        = mesh.vertices[t[0]].cast<double>();
        const Vec3d b = mesh.vertices[t[1]].cast<double>() - origin;
        const Vec3d c = mesh.vertices[t[2]].cast<double>() - origin;
        if (b.cross(c).norm() < 1e-12)
            throw std::runtime_error("Degenerate source triangle in exact tracery partition");
        u = b.normalized();
        v = b.cross(c).normalized().cross(u);
    }
    Point project(const Vec3d& point) const
    {
        const Vec3d delta = point - origin;
        return Point(scale_(delta.dot(u)), scale_(delta.dot(v)));
    }
    Vec3d lift(const Point& point) const { return origin + unscale<double>(point.x()) * u + unscale<double>(point.y()) * v; }
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

void overlay_tracery(std::vector<FacetPartition>& facets, const TriangleSelector::Tracery2DData& regions)
{
    for (const auto& input : regions) {
        FacetPartition& facet = facets.at(input.source_facet_id);
        // Group before overlaying: work-triangle seams within one fragment are
        // not material boundaries and must not survive as tiny sliver regions.
        std::map<std::pair<size_t, EnforcerBlockerType>, Polygons> grouped;
        for (const auto& region : input.regions) {
            std::vector<Vec3f> points;
            for (const Vec2d& p : region.polygon)
                points.push_back((input.origin + p.x() * input.axis_u + p.y() * input.axis_v).cast<float>());
            Polygon polygon = projected_polygon(facet.frame, points);
            if (!polygon.empty())
                grouped[{region.fragment_id, region.color}].push_back(std::move(polygon));
        }
        for (auto& [key, polygons] : grouped)
            overlay_region(facet.cells, union_(polygons), 0, key.second);
        facet.needs_partition = facet.needs_partition || facet.cells.size() > 1;
    }
}

// All edge cuts are keyed by original vertex IDs, never merely by position:
// disconnected coincident shells must remain disconnected.
struct EdgeCuts
{
    std::map<Edge, std::vector<double>> parameters;
    std::map<Edge, std::vector<int>>    vertices;
};

bool on_segment(const Vec3d& point, const Vec3d& a, const Vec3d& b, double& t)
{
    const Vec3d delta = b - a;
    if (delta.squaredNorm() < 1e-24)
        return false;
    t                      = (point - a).dot(delta) / delta.squaredNorm();
    const double tolerance = weld_tolerance / delta.norm();
    return t >= -tolerance && t <= 1. + tolerance && (point - a - t * delta).norm() <= weld_tolerance;
}

void collect_edge_cuts(const indexed_triangle_set& mesh, const std::vector<FacetPartition>& facets, EdgeCuts& cuts)
{
    for (size_t i = 0; i < facets.size(); ++i) {
        const auto& triangle = mesh.indices[i];
        for (const Cell& cell : facets[i].cells) {
            for (const Point& point : to_points(ExPolygons{cell.polygon})) {
                const Vec3d world = facets[i].frame.lift(point);
                for (int e = 0; e < 3; ++e) {
                    const Edge edge = std::minmax(triangle[e], triangle[(e + 1) % 3]);
                    double     t;
                    if (on_segment(world, mesh.vertices[edge.first].cast<double>(), mesh.vertices[edge.second].cast<double>(), t))
                        cuts.parameters[edge].push_back(std::clamp(t, 0., 1.));
                }
            }
        }
    }
}

void allocate_edge_vertices(const indexed_triangle_set& source, EdgeCuts& cuts, indexed_triangle_set& output)
{
    output.vertices = source.vertices;
    for (auto& [edge, parameters] : cuts.parameters) {
        parameters.push_back(0.);
        parameters.push_back(1.);
        std::sort(parameters.begin(), parameters.end());
        const Vec3d         a         = source.vertices[edge.first].cast<double>();
        const Vec3d         delta     = source.vertices[edge.second].cast<double>() - a;
        const double        tolerance = weld_tolerance / delta.norm();
        std::vector<double> unique;
        for (double t : parameters) {
            t = t < tolerance ? 0. : (t > 1. - tolerance ? 1. : t);
            if (unique.empty() || t - unique.back() > tolerance)
                unique.push_back(t);
        }
        parameters = std::move(unique);
        auto& ids  = cuts.vertices[edge];
        for (double t : parameters) {
            const int index = t == 0. ? edge.first : (t == 1. ? edge.second : int(output.vertices.size()));
            if (t != 0. && t != 1.)
                output.vertices.push_back((a + t * delta).cast<float>());
            ids.push_back(index);
        }
    }
}

void insert_collinear_points(Polygon& polygon, const Points& candidates)
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
            if (t > end_tolerance && t < 1. - end_tolerance && ((p - a).cast<double>() - t * delta).norm() <= scale_(weld_tolerance))
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
    int    nearest  = -1;
    double distance = std::pow(scale_(weld_tolerance) + 2., 2);
    for (const auto& [candidate, id] : vertices) {
        const double d = (candidate - point).cast<double>().squaredNorm();
        if (d <= distance) {
            distance = d;
            nearest  = id;
        }
    }
    if (nearest < 0) {
        nearest = int(output.vertices.size());
        output.vertices.push_back(frame.lift(point).cast<float>());
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
                double distance = std::pow(scale_(weld_tolerance) + 2., 2);
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
    clean_partition_cells(facet.cells);
    for (const Cell& cell : facet.cells)
        for (const Point& p : to_points(ExPolygons{cell.polygon}))
            allocate_partition_vertex(p, facet.frame, vertices, output);
    Points points;
    for (const auto& [point, id] : vertices)
        points.push_back(point);
    for (Cell& cell : facet.cells) {
        insert_collinear_points(cell.polygon.contour, points);
        for (Polygon& hole : cell.polygon.holes)
            insert_collinear_points(hole, points);
        auto& holes = cell.polygon.holes;
        holes.erase(std::remove_if(holes.begin(), holes.end(), [](const Polygon& hole) { return hole.area() == 0.; }), holes.end());
    }
    clean_partition_cells(facet.cells);
    for (const Cell& cell : facet.cells)
        for (const Point& p : to_points(ExPolygons{cell.polygon}))
            allocate_partition_vertex(p, facet.frame, vertices, output);
    return vertices;
}

using PartitionKernel        = CGAL::Exact_predicates_inexact_constructions_kernel;
using PartitionVertex        = CGAL::Triangulation_vertex_base_with_info_2<int, PartitionKernel>;
using PartitionFace          = CGAL::Constrained_triangulation_face_base_2<PartitionKernel>;
using PartitionData          = CGAL::Triangulation_data_structure_2<PartitionVertex, PartitionFace>;
using PartitionTriangulation = CGAL::Constrained_Delaunay_triangulation_2<PartitionKernel, PartitionData, CGAL::Exact_predicates_tag>;

PartitionTriangulation triangulate_shared_boundaries(const FacetPartition& facet,
                                                     std::map<Point, int>& vertices,
                                                     indexed_triangle_set& output)
{
    // One triangulation owns all colors, so their shared edges cannot overlap.
    PartitionTriangulation                               triangulation;
    std::map<int, PartitionTriangulation::Vertex_handle> handles;
    std::map<PartitionKernel::Point_2, int>              ids;
    for (const auto& [point, id] : vertices) {
        if (handles.count(id))
            continue;
        const Vec3d                    delta = output.vertices[id].cast<double>() - facet.frame.origin;
        const PartitionKernel::Point_2 position(scale_(delta.dot(facet.frame.u)), scale_(delta.dot(facet.frame.v)));
        handles[id]   = triangulation.insert(position);
        ids[position] = id;
    }
    for (const Cell& cell : facet.cells) {
        for (const Polygon& polygon : to_polygons(ExPolygons{cell.polygon})) {
            for (size_t i = 0; i < polygon.points.size(); ++i) {
                const auto a = handles.at(vertices.at(polygon.points[i]));
                const auto b = handles.at(vertices.at(polygon.points[(i + 1) % polygon.points.size()]));
                if (a != b)
                    triangulation.insert_constraint(a, b);
            }
        }
    }
    for (auto vertex = triangulation.finite_vertices_begin(); vertex != triangulation.finite_vertices_end(); ++vertex) {
        const auto found = ids.find(vertex->point());
        if (found != ids.end())
            vertex->info() = found->second;
        else {
            const Point point(std::llround(vertex->point().x()), std::llround(vertex->point().y()));
            allocate_partition_vertex(point, facet.frame, vertices, output);
            vertex->info() = vertices.at(point);
        }
    }
    return triangulation;
}

const States& partition_states_at(const FacetPartition& facet, const Point& point)
{
    const Cell* nearest  = nullptr;
    double      distance = std::numeric_limits<double>::max();
    for (const Cell& cell : facet.cells) {
        if (cell.polygon.contains(point))
            return cell.states;
        for (const Polygon& polygon : to_polygons(ExPolygons{cell.polygon})) {
            const double d = point.distance_to(point.projection_onto(polygon));
            if (d < distance) {
                distance = d;
                nearest  = &cell;
            }
        }
    }
    if (!nearest || distance > scale_(weld_tolerance) * 2.)
        throw std::runtime_error("Uncovered region in exact tracery partition: cells=" + std::to_string(facet.cells.size()) +
                                 " distance=" + std::to_string(distance));
    return nearest->states;
}

void triangulate_facet_partition(const indexed_triangle_set& source,
                                 size_t                      id,
                                 FacetPartition&             facet,
                                 const EdgeCuts&             cuts,
                                 TraceryPartitionResult&     result,
                                 std::vector<States>&        states)
{
    // Unchanged, uniformly annotated facets keep their original geometry.
    // This also preserves sub-micron slivers across subsequent brush strokes.
    if (!facet.needs_partition) {
        result.mesh.indices.push_back(source.indices[id]);
        result.source_facets.push_back(int(id));
        states.push_back(facet.cells.front().states);
        return;
    }
    auto          vertices      = synchronize_facet_vertices(source, id, facet, cuts, result.mesh);
    auto          triangulation = triangulate_shared_boundaries(facet, vertices, result.mesh);
    const Vec3i32 original      = source.indices[id];
    const Vec3d   normal        = (source.vertices[original[1]].cast<double>() - source.vertices[original[0]].cast<double>())
                                      .cross(source.vertices[original[2]].cast<double>() - source.vertices[original[0]].cast<double>());
    double        area          = 0.;
    std::set<std::array<int, 3>> emitted;
    for (auto face = triangulation.finite_faces_begin(); face != triangulation.finite_faces_end(); ++face) {
        Vec3i32            t(face->vertex(0)->info(), face->vertex(1)->info(), face->vertex(2)->info());
        std::array<int, 3> key{t[0], t[1], t[2]};
        std::sort(key.begin(), key.end());
        if (!emitted.insert(key).second)
            continue;
        const Vec3d a     = result.mesh.vertices[t[0]].cast<double>();
        const Vec3d b     = result.mesh.vertices[t[1]].cast<double>();
        const Vec3d c     = result.mesh.vertices[t[2]].cast<double>();
        const Vec3d cross = (b - a).cross(c - a);
        if (cross.norm() <= 1e-12)
            continue;
        if (cross.dot(normal) < 0.)
            std::swap(t[1], t[2]);
        const Point center = facet.frame.project((a + b + c) / 3.);
        states.push_back(partition_states_at(facet, center));
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

void build_partition_annotations(TraceryPartitionResult& result, const std::vector<States>& states, size_t channels)
{
    const TriangleMesh mesh(result.mesh);
    for (size_t channel = 0; channel < channels; ++channel) {
        TriangleSelector selector(mesh);
        for (size_t triangle = 0; triangle < states.size(); ++triangle)
            if (states[triangle][channel] != EnforcerBlockerType::NONE)
                selector.set_facet(int(triangle), states[triangle][channel]);
        result.annotations.push_back(selector.serialize());
    }
}

void enforce_source_edge_cuts(const indexed_triangle_set& source,
                              const EdgeCuts&             cuts,
                              TraceryPartitionResult&     result,
                              std::vector<States>&        states)
{
    std::vector<Vec3i32> triangles;
    std::vector<int>     sources;
    std::vector<States>  colors;
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
                if (std::all_of(triangle.data(), triangle.data() + 3,
                                [&](int v) { return std::find(ids.begin(), ids.end(), v) != ids.end(); }))
                    continue;
                bool divided = false;
                for (int side = 0; side < 3 && !divided; ++side) {
                    const auto a = std::find(ids.begin(), ids.end(), triangle[side]);
                    const auto b = std::find(ids.begin(), ids.end(), triangle[(side + 1) % 3]);
                    if (a == ids.end() || b == ids.end() || std::abs(b - a) <= 1)
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

void validate_partition_topology(const indexed_triangle_set& source, const TraceryPartitionResult& result)
{
    std::map<Edge, size_t> original_edges;
    for (const Vec3i32& triangle : source.indices)
        for (int e = 0; e < 3; ++e)
            ++original_edges[std::minmax(triangle[e], triangle[(e + 1) % 3])];
    std::map<Edge, std::vector<int>> edges;
    for (size_t i = 0; i < result.mesh.indices.size(); ++i) {
        const Vec3i32& triangle = result.mesh.indices[i];
        for (int e = 0; e < 3; ++e)
            edges[std::minmax(triangle[e], triangle[(e + 1) % 3])].push_back(result.source_facets[i]);
    }
    for (const auto& [edge, facets] : edges) {
        if (facets.size() == 2)
            continue;
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
            throw std::runtime_error("Exact tracery partition introduced an unmatched mesh edge");
    }
}
} // namespace

TraceryPartitionResult build_exact_tracery_partition(const TriangleMesh&                                         source,
                                                     const std::vector<TriangleSelector::TriangleSplittingData>& annotations,
                                                     const TriangleSelector::Tracery2DData&                      regions,
                                                     const std::function<void()>&                                cancel)
{
    if (annotations.empty())
        throw std::invalid_argument("Exact tracery partition requires a paint channel");
    std::vector<FacetPartition> facets;
    for (size_t i = 0; i < source.its.indices.size(); ++i) {
        if (cancel)
            cancel();
        facets.emplace_back(source.its, int(i), annotations.size());
    }
    for (size_t channel = 0; channel < annotations.size(); ++channel) {
        if (cancel)
            cancel();
        overlay_annotation(facets, source, annotations[channel], channel);
    }
    overlay_tracery(facets, regions);
    EdgeCuts cuts;
    collect_edge_cuts(source.its, facets, cuts);
    TraceryPartitionResult result;
    allocate_edge_vertices(source.its, cuts, result.mesh);
    // Cleaning legacy paint can introduce new intersections on a source edge.
    // Collect those cuts before the final cross-facet vertex allocation.
    for (size_t i = 0; i < facets.size(); ++i) {
        if (cancel)
            cancel();
        if (facets[i].needs_partition)
            synchronize_facet_vertices(source.its, i, facets[i], cuts, result.mesh);
    }
    cuts = {};
    collect_edge_cuts(source.its, facets, cuts);
    allocate_edge_vertices(source.its, cuts, result.mesh);
    std::vector<States> states;
    for (size_t i = 0; i < facets.size(); ++i) {
        if (cancel)
            cancel();
        triangulate_facet_partition(source.its, i, facets[i], cuts, result, states);
    }
    enforce_source_edge_cuts(source.its, cuts, result, states);
    validate_partition_topology(source.its, result);
    build_partition_annotations(result, states, annotations.size());
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
} // namespace Slic3r
