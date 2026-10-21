#include "TraceryDocument.hpp"
#include "ClipperUtils.hpp"
#include "Tesselate.hpp"
#include "TraceryPerformance.hpp"
#include <nlohmann/json.hpp>
#include <numeric>
#include <stdexcept>

namespace Slic3r {
namespace {
using Regions    = TraceryDocument::Regions;
using Annotation = TraceryDocument::Annotation;
TriangleSelector::TraceryFacet2DData plane(const TriangleMesh& mesh, int id)
{
    TriangleSelector::TraceryFacet2DData p;
    const auto&                          t = mesh.its.indices.at(id);
    p.source_facet_id                      = id;
    p.origin                               = mesh.its.vertices[t[0]].cast<double>();
    const Vec3d a                          = mesh.its.vertices[t[1]].cast<double>() - p.origin;
    const Vec3d b                          = mesh.its.vertices[t[2]].cast<double>() - p.origin;
    if (a.cross(b).squaredNorm() == 0.)
        throw std::runtime_error("Degenerate stained-glass base facet");
    p.axis_u = a.normalized();
    p.axis_v = a.cross(b).normalized().cross(p.axis_u);
    // Keep sub-micron source facets representable in the persistent integer
    // plane too. Otherwise saving could turn their whole-face colour into an
    // empty polygon and erase would lose its original state after reopening.
    const double altitude      = a.cross(b).norm() / std::max({a.norm(), b.norm(), (a - b).norm()});
    const double magnification = std::clamp(100. * SCALING_FACTOR / altitude, 1., 1000000.);
    p.axis_u /= magnification;
    p.axis_v /= magnification;
    return p;
}
Point project(const TriangleSelector::TraceryFacet2DData& p, const Vec3d& v)
{
    const Vec3d d = v - p.origin;
    return {scale_(d.dot(p.axis_u) / p.axis_u.squaredNorm()), scale_(d.dot(p.axis_v) / p.axis_v.squaredNorm())};
}
Regions annotation_regions(const TriangleMesh&     base,
                           const std::vector<int>& sources,
                           const TriangleMesh&     current,
                           const Annotation&       annotation)
{
    TriangleSelector selector(current);
    selector.deserialize(annotation, false);
    Regions result;
    for (const auto& leaf : selector.painted_leaves(true)) {
        const int  source = sources.at(leaf.source_facet);
        const auto frame  = plane(base, source);
        Polygon    p;
        for (const auto& v : leaf.vertices)
            p.points.push_back(project(frame, v.cast<double>()));
        p.make_counter_clockwise();
        result[source][unsigned(leaf.state)].push_back(std::move(p));
    }
    for (auto& [id, states] : result)
        for (auto& [state, polys] : states)
            polys = union_(polys);
    return result;
}
void overlay(Regions& destination, const Regions& patch)
{
    for (const auto& [id, states] : patch) {
        Polygons mask;
        for (const auto& [state, polys] : states)
            append(mask, polys);
        mask         = union_(mask);
        auto& target = destination[id];
        for (auto& [state, polys] : target)
            polys = diff(polys, mask);
        for (const auto& [state, polys] : states) {
            append(target[state], polys);
            target[state] = union_(target[state]);
        }
    }
}
Regions delta(const Regions& before, const Regions& after)
{
    Regions result;
    for (const auto& [id, states] : after)
        for (const auto& [state, polys] : states) {
            auto        f       = before.find(id);
            const auto* old     = f == before.end() || !f->second.count(state) ? nullptr : &f->second.at(state);
            auto        changed = old ? diff(polys, *old) : polys;
            if (!changed.empty())
                result[id][state] = std::move(changed);
        }
    return result;
}
Regions paint_regions(const TriangleMesh& base, const std::vector<int>& sources, const TriangleSelector::Tracery2DData& input)
{
    Regions result;
    for (const auto& facet : input) {
        const int  source = sources.at(facet.source_facet_id);
        const auto frame  = plane(base, source);
        for (const auto& r : facet.regions) {
            Polygon p;
            for (const Vec2d& v : r.polygon)
                p.points.push_back(project(frame, facet.origin + v.x() * facet.axis_u + v.y() * facet.axis_v));
            p.make_counter_clockwise();
            result[source][unsigned(r.color)].push_back(std::move(p));
        }
    }
    for (auto& [id, states] : result)
        for (auto& [state, polys] : states)
            polys = union_(polys);
    return result;
}
TriangleSelector::Tracery2DData partition_regions(const TriangleMesh& base, const Regions& input, Annotation* defaults = nullptr)
{
    TriangleSelector::Tracery2DData result;
    TriangleSelector                selector(base);
    for (const auto& [id, states] : input) {
        auto     facet        = plane(base, id);
        unsigned background   = 0;
        double   largest_area = -1.;
        if (defaults) {
            for (const auto& [state, polys] : states) {
                double area = 0.;
                for (const auto& p : polys)
                    area += p.area();
                if (area > largest_area) {
                    largest_area = area;
                    background   = state;
                }
            }
            selector.set_facet(id, EnforcerBlockerType(background));
            const auto&  t     = base.its.indices[id];
            const Vec3d  a     = base.its.vertices[t[0]].cast<double>();
            const Vec3d  b     = base.its.vertices[t[1]].cast<double>() - a;
            const Vec3d  c     = base.its.vertices[t[2]].cast<double>() - a;
            const double width = b.cross(c).norm() / std::max({b.norm(), c.norm(), (b - c).norm()});
            // Sub-tolerance slivers cannot store separate colour boundaries
            // reliably in float coordinates. Keep their dominant state and
            // shared source-edge cuts; the authoritative polygons stay intact.
            if (width < TRACERY_GEOMETRY_TOLERANCE)
                continue;
        }
        for (const auto& [state, polys] : states) {
            // Assign the full facet's background directly. Re-projecting a
            // whole facet would introduce rounding seams at its source edges.
            if (state == background)
                continue;
            for (const auto& polygon : union_ex(polys)) {
                auto add = [&, colour = state](const std::vector<Vec2d>& points) {
                    TriangleSelector::TraceryRegion2D region;
                    region.color   = EnforcerBlockerType(colour);
                    region.polygon = points;
                    facet.regions.push_back(std::move(region));
                };
                if (polygon.holes.empty()) {
                    std::vector<Vec2d> points;
                    for (const auto& point : polygon.contour.points)
                        points.push_back(unscale(point));
                    add(points);
                } else {
                    auto triangles = triangulate_expolygons_2d({polygon});
                    for (size_t i = 0; i + 2 < triangles.size(); i += 3)
                        add({triangles[i], triangles[i + 1], triangles[i + 2]});
                }
            }
        }
        if (!facet.regions.empty())
            result.push_back(std::move(facet));
    }
    if (defaults)
        *defaults = selector.serialize();
    return result;
}
} // namespace
TraceryDocument TraceryDocument::capture(const TriangleMesh& mesh, const std::vector<Annotation>& annotations)
{
    if (annotations.size() != 8)
        throw std::invalid_argument("Stained glass requires eight annotation channels");
    TraceryDocument doc;
    doc.m_base    = mesh;
    doc.m_derived = mesh.its;
    doc.m_sources.resize(mesh.its.indices.size());
    std::iota(doc.m_sources.begin(), doc.m_sources.end(), 0);
    doc.m_last = annotations;
    for (const auto& annotation : annotations)
        doc.m_channels.push_back(annotation_regions(doc.m_base, doc.m_sources, mesh, annotation));
    return doc;
}
bool TraceryDocument::matches(const TriangleMesh& mesh) const
{
    return mesh.its.vertices == m_derived.vertices && mesh.its.indices == m_derived.indices;
}
TraceryPartitionResult TraceryDocument::rebuild(const TriangleMesh&                    current,
                                                const std::vector<Annotation>&         annotations,
                                                const TriangleSelector::Tracery2DData& paint,
                                                TraceryEdit                            edit)
{
    if (!matches(current) || annotations.size() != 8)
        throw std::runtime_error("Stained-glass base no longer matches this geometry; clear the effect before editing the changed mesh");
    TraceryPerformanceScope performance("stable_base_rebuild");
    performance.count("base_facets", m_base.its.indices.size());
    performance.count("derived_facets", current.its.indices.size());
    auto channels = m_channels;
    performance.stage("stable_base.annotation_deltas");
    for (size_t channel = 0; channel < 5; ++channel) {
        if (annotations[channel] == m_last[channel])
            continue;
        const auto changes = delta(annotation_regions(m_base, m_sources, current, m_last[channel]),
                                   annotation_regions(m_base, m_sources, current, annotations[channel]));
        overlay(channels[channel], changes);
        if (channel == 0)
            overlay(channels[5], changes); // ordinary paint survives erase
    }
    performance.stage("stable_base.mask_delta");
    channels[7] = channels[6];
    if (annotations[6] != m_last[6]) {
        const auto changes = delta(annotation_regions(m_base, m_sources, current, m_last[6]),
                                   annotation_regions(m_base, m_sources, current, annotations[6]));
        overlay(channels[6], changes);
        if (edit == TraceryEdit::Erase) {
            Regions restore;
            for (const auto& [id, states] : changes) {
                auto erased = states.find(0);
                if (erased == states.end())
                    continue;
                for (const auto& [state, polys] : channels[5].at(id)) {
                    auto clipped = intersection(polys, erased->second);
                    if (!clipped.empty())
                        restore[id][state] = std::move(clipped);
                }
            }
            overlay(channels[0], restore);
        }
    }
    if (edit == TraceryEdit::Regenerate)
        channels[0] = channels[5];
    performance.stage("stable_base.paint_regions");
    const auto incoming = paint_regions(m_base, m_sources, paint);
    if (edit == TraceryEdit::Erase) {
        // Explicit projected erase polygons also restore the back of a through
        // selection. Their display/placeholder color has no material meaning.
        Regions restore;
        for (const auto& [id, states] : incoming) {
            Polygons mask;
            for (const auto& [state, polys] : states)
                append(mask, polys);
            mask = union_(mask);
            for (const auto& [state, polys] : channels[5].at(id)) {
                auto clipped = intersection(polys, mask);
                if (!clipped.empty())
                    restore[id][state] = std::move(clipped);
            }
        }
        overlay(channels[0], restore);
    } else {
        overlay(channels[0], incoming);
    }
    channels[7] = channels[6]; // next stroke starts from the committed mask
    performance.stage("stable_base.partition");
    std::vector<TriangleSelector::Tracery2DData> regions;
    std::vector<Annotation>                      defaults(8);
    for (size_t channel = 0; channel < channels.size(); ++channel)
        regions.push_back(partition_regions(m_base, channels[channel], &defaults[channel]));
    auto result = build_exact_tracery_partition(m_base, defaults, {}, {}, regions);
    // Publish only after geometry validation succeeded. Failed rebuilds leave
    // both the immutable base and all authoritative editing data untouched.
    performance.stage("stable_base.publish");
    m_channels = std::move(channels);
    m_derived  = result.mesh;
    m_sources  = result.source_facets;
    m_last     = result.annotations;
    return result;
}

namespace {
using Json = nlohmann::json;
Json mesh_json(const indexed_triangle_set& mesh)
{
    Json vertices = Json::array(), faces = Json::array();
    for (const auto& v : mesh.vertices)
        vertices.push_back({v.x(), v.y(), v.z()});
    for (const auto& t : mesh.indices)
        faces.push_back({t[0], t[1], t[2]});
    return {vertices, faces};
}
indexed_triangle_set read_mesh(const Json& j)
{
    indexed_triangle_set mesh;
    for (const auto& v : j.at(0)) {
        Vec3f p(v.at(0).get<float>(), v.at(1).get<float>(), v.at(2).get<float>());
        if (!p.allFinite())
            throw std::runtime_error("Invalid stained-glass vertex");
        mesh.vertices.push_back(p);
    }
    for (const auto& t : j.at(1)) {
        Vec3i32 face(t.at(0).get<int>(), t.at(1).get<int>(), t.at(2).get<int>());
        for (int v : face)
            if (v < 0 || size_t(v) >= mesh.vertices.size())
                throw std::runtime_error("Invalid stained-glass face");
        mesh.indices.push_back(face);
    }
    return mesh;
}
Json annotation_json(const TraceryDocument::Annotation& annotation)
{
    Json faces = Json::array();
    for (const auto& t : annotation.triangles_to_split)
        faces.push_back({t.triangle_idx, t.bitstream_start_idx});
    return {faces, annotation.bitstream, annotation.used_states};
}
TraceryDocument::Annotation read_annotation(const Json& j, size_t facets)
{
    TraceryDocument::Annotation result;
    result.bitstream   = j.at(1).get<std::vector<bool>>();
    result.used_states = j.at(2).get<std::vector<bool>>();
    for (const auto& t : j.at(0)) {
        const int facet = t.at(0).get<int>(), offset = t.at(1).get<int>();
        if (facet < 0 || size_t(facet) >= facets || offset < 0 || size_t(offset) >= result.bitstream.size())
            throw std::runtime_error("Invalid stained-glass annotation");
        result.triangles_to_split.emplace_back(facet, offset);
    }
    return result;
}
} // namespace
std::string TraceryDocument::encode() const
{
    Json root;
    root["version"]  = 1;
    root["base"]     = mesh_json(m_base.its);
    root["derived"]  = mesh_json(m_derived);
    root["sources"]  = m_sources;
    root["channels"] = Json::array();
    for (const auto& channel : m_channels) {
        Json entries = Json::array();
        for (const auto& [facet, states] : channel)
            for (const auto& [state, polys] : states) {
                Json polygons = Json::array();
                for (const auto& p : polys) {
                    Json points = Json::array();
                    for (const auto& v : p.points)
                        points.push_back({v.x(), v.y()});
                    polygons.push_back(std::move(points));
                }
                if (!polygons.empty())
                    entries.push_back({facet, state, polygons});
            }
        root["channels"].push_back(std::move(entries));
    }
    root["last"] = Json::array();
    for (const auto& annotation : m_last)
        root["last"].push_back(annotation_json(annotation));
    return root.dump();
}
TraceryDocument TraceryDocument::decode(const std::string& data)
{
    const Json root = Json::parse(data);
    if (root.at("version").get<int>() != 1)
        throw std::runtime_error("Unsupported stained-glass editing data version");
    TraceryDocument result;
    result.m_base    = TriangleMesh(read_mesh(root.at("base")));
    result.m_derived = read_mesh(root.at("derived"));
    result.m_sources = root.at("sources").get<std::vector<int>>();
    if (result.m_sources.size() != result.m_derived.indices.size())
        throw std::runtime_error("Invalid stained-glass lineage");
    for (int id : result.m_sources)
        if (id < 0 || size_t(id) >= result.m_base.its.indices.size())
            throw std::runtime_error("Invalid stained-glass source facet");
    for (const auto& c : root.at("channels")) {
        Regions channel;
        for (const auto& entry : c) {
            const int      facet = entry.at(0).get<int>();
            const unsigned state = entry.at(1).get<unsigned>();
            if (facet < 0 || size_t(facet) >= result.m_base.its.indices.size() || state > unsigned(EnforcerBlockerType::ExtruderMax))
                throw std::runtime_error("Invalid stained-glass region");
            for (const auto& points : entry.at(2)) {
                Polygon p;
                for (const auto& v : points)
                    p.points.emplace_back(v.at(0).get<coord_t>(), v.at(1).get<coord_t>());
                if (p.points.size() < 3)
                    throw std::runtime_error("Invalid stained-glass polygon");
                channel[facet][state].push_back(std::move(p));
            }
        }
        result.m_channels.push_back(std::move(channel));
    }
    for (const auto& a : root.at("last"))
        result.m_last.push_back(read_annotation(a, result.m_sources.size()));
    if (result.m_channels.size() != 8 || result.m_last.size() != 8)
        throw std::runtime_error("Invalid stained-glass channels");
    return result;
}
TriangleSelector::Tracery2DData TraceryDocument::erased_regions(const TriangleMesh& mesh, const Annotation& before, const Annotation& after)
{
    std::vector<int> sources(mesh.its.indices.size());
    std::iota(sources.begin(), sources.end(), 0);
    const auto changes = delta(annotation_regions(mesh, sources, mesh, before), annotation_regions(mesh, sources, mesh, after));
    Regions    erased;
    for (const auto& [facet, states] : changes)
        if (auto found = states.find(0); found != states.end())
            erased[facet][1] = found->second;
    return partition_regions(mesh, erased);
}
void TraceryDocument::remap_materials(const EnforcerBlockerStateMap& mapping)
{
    const TriangleMesh current(m_derived);
    for (size_t channel : {size_t(0), size_t(5)}) {
        for (auto& [facet, states] : m_channels[channel]) {
            std::map<unsigned, Polygons> mapped;
            for (const auto& [state, polygons] : states)
                append(mapped[unsigned(mapping.at(state))], polygons);
            for (auto& [state, polygons] : mapped)
                polygons = union_(polygons);
            states = std::move(mapped);
        }
        TriangleSelector selector(current);
        selector.deserialize(m_last[channel], false);
        selector.remap_triangle_state(mapping);
        m_last[channel] = selector.serialize();
    }
}
} // namespace Slic3r
