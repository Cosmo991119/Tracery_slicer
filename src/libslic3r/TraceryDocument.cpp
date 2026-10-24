#include "TraceryDocument.hpp"
#include "Model.hpp"
#include "ClipperUtils.hpp"
#include "Tesselate.hpp"
#include "TraceryPerformance.hpp"
#include "TraceryPolygonPreparation.hpp"
#include <nlohmann/json.hpp>
#include <numeric>
#include <set>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <list>
#include <stdexcept>

namespace Slic3r {
namespace {
struct TraceryCacheEntry {
    size_t hash;
    std::string data;
    std::shared_ptr<const TraceryDocument> document;
};
using TraceryCacheEntryPtr = std::shared_ptr<const TraceryCacheEntry>;
thread_local std::list<TraceryCacheEntryPtr> document_cache;
thread_local size_t document_cache_bytes = 0;
constexpr size_t max_source_bytes = 32 * 1024 * 1024;
constexpr size_t max_entries = 16;
} // namespace

struct TraceryDocument::ReadCacheScope::State {
    inline static thread_local State* active = nullptr;
    State* previous = active;
    std::vector<TraceryCacheEntryPtr> entries;
    size_t source_bytes = 0;
    State() { active = this; }
    ~State() { active = previous; }
    void retain(const TraceryCacheEntryPtr& entry)
    {
        // A nested validation callback must not release the working set before
        // the enclosing scene update has finished using the same documents.
        if (previous) {
            previous->retain(entry);
            return;
        }
        entries.push_back(entry);
        source_bytes += entry->data.size();
    }
};

TraceryDocument::ReadCacheScope::ReadCacheScope() : m_state(std::make_unique<State>()) {}
TraceryDocument::ReadCacheScope::~ReadCacheScope()
{
    TraceryPerformanceScope performance("document_cache_release");
    performance.count("retained_documents", m_state->entries.size());
    performance.count("retained_source_bytes", m_state->source_bytes);
    m_state.reset();
}

std::shared_ptr<const TraceryDocument> TraceryDocument::decode_cached(const std::string& data)
{
    // The bounded idle cache serves isolated readers. A commit scope retains its
    // whole working set until all UI callbacks finish, avoiding cyclic eviction.
    // Entries are shared, so retaining them does not copy their large strings.
    auto& cache = document_cache;
    TraceryPerformanceScope performance("document_cache");
    performance.stage("document_cache.lookup");
    performance.count("source_bytes", data.size());
    const size_t hash = std::hash<std::string>{}(data);
    performance.count("content_hash", hash);
    auto* active = ReadCacheScope::State::active;
    for (auto* scope = active; scope; scope = scope->previous) {
        for (const auto& entry : scope->entries) {
            if (entry->hash == hash && entry->data == data) {
                performance.count("cache_hit", 1);
                performance.count("scope_hit", 1);
                return entry->document;
            }
        }
    }
    for (auto it = cache.begin(); it != cache.end(); ++it) {
        const auto& entry = *it;
        if (entry->hash == hash && entry->data == data) {
            performance.count("cache_hit", 1);
            if (active)
                active->retain(entry);
            auto document = entry->document;
            cache.splice(cache.begin(), cache, it);
            return document;
        }
    }
    performance.count("cache_hit", 0);
    performance.count("over_idle_budget", data.size() > max_source_bytes ? 1 : 0);
    performance.stage("document_cache.decode");
    auto document = std::make_shared<const TraceryDocument>(decode(data));
    performance.stage("document_cache.store");
    cache_document(data, document);
    return document;
}

void TraceryDocument::cache_document(const std::string& data, std::shared_ptr<const TraceryDocument> document)
{
    auto* active = ReadCacheScope::State::active;
    if (!active && data.size() > max_source_bytes)
        return;
    auto entry = std::make_shared<const TraceryCacheEntry>(TraceryCacheEntry{
        std::hash<std::string>{}(data), data, std::move(document)});
    if (active)
        active->retain(entry);
    if (data.size() <= max_source_bytes) {
        while (!document_cache.empty() &&
               (document_cache.size() >= max_entries || document_cache_bytes + data.size() > max_source_bytes)) {
            document_cache_bytes -= document_cache.back()->data.size();
            document_cache.pop_back();
        }
        document_cache.push_front(std::move(entry));
        document_cache_bytes += data.size();
    }
}

std::string TraceryDocument::encode_cached() &&
{
    TraceryPerformanceScope performance("document_publish_snapshot");
    performance.stage("document_publish.validate");
    // Publishing a live snapshot skips decode(), but must retain its structural
    // checks. Inspect the native data directly without parsing another JSON tree.
    const auto validate_mesh = [](const indexed_triangle_set& mesh) {
        for (const auto& vertex : mesh.vertices)
            if (!vertex.allFinite())
                throw std::runtime_error("Invalid stained-glass vertex");
        for (const auto& face : mesh.indices)
            for (int vertex : face)
                if (vertex < 0 || size_t(vertex) >= mesh.vertices.size())
                    throw std::runtime_error("Invalid stained-glass face");
    };
    validate_mesh(m_base.its);
    validate_mesh(m_derived);
    if (m_sources.size() != m_derived.indices.size())
        throw std::runtime_error("Invalid stained-glass lineage");
    for (int id : m_sources)
        if (id < 0 || size_t(id) >= m_base.its.indices.size())
            throw std::runtime_error("Invalid stained-glass source facet");
    if (m_channels.size() != 8 || m_last.size() != 8)
        throw std::runtime_error("Invalid stained-glass channels");
    for (const auto& channel : m_channels)
        for (const auto& [facet, states] : channel)
            for (const auto& [state, polygons] : states) {
                if (facet < 0 || size_t(facet) >= m_base.its.indices.size() || state > unsigned(EnforcerBlockerType::ExtruderMax))
                    throw std::runtime_error("Invalid stained-glass region");
                for (const auto& polygon : polygons)
                    if (polygon.points.size() < 3)
                        throw std::runtime_error("Invalid stained-glass polygon");
            }
    const auto validate_annotation = [&](const Annotation& annotation) {
        for (const auto& triangle : annotation.triangles_to_split)
            if (triangle.triangle_idx < 0 || size_t(triangle.triangle_idx) >= m_sources.size() ||
                triangle.bitstream_start_idx < 0 || size_t(triangle.bitstream_start_idx) >= annotation.bitstream.size())
                throw std::runtime_error("Invalid stained-glass annotation");
    };
    for (const auto& annotation : m_last)
        validate_annotation(annotation);
    validate_annotation(m_selection_reference);
    performance.stage("document_publish.encode");
    auto data = encode();
    performance.stage("document_publish.retain");
    cache_document(data, std::make_shared<const TraceryDocument>(std::move(*this)));
    return data;
}

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
                           const Annotation&       annotation,
                           const std::vector<int>* changed_facets = nullptr)
{
    TraceryPerformanceScope performance("vector_annotation_regions");
    performance.stage("annotation.deserialize");
    TriangleSelector selector(current);
    selector.deserialize(annotation, false);
    performance.stage("annotation.extract_leaves");
    const auto leaves = selector.coalesced_painted_leaves(true, changed_facets);
    performance.count("exported_patches", leaves.size());
    performance.count("requested_facets", changed_facets ? changed_facets->size() : current.its.indices.size());
    performance.stage("annotation.project_leaves");
    Regions result;
    for (const auto& leaf : leaves) {
        const int  source = sources.at(leaf.source_facet);
        const auto frame  = plane(base, source);
        Polygon    p;
        for (const auto& v : leaf.vertices)
            p.points.push_back(project(frame, v.cast<double>()));
        p.make_counter_clockwise();
        result[source][unsigned(leaf.state)].push_back(std::move(p));
    }
    performance.count("source_facets", result.size());
    performance.stage("annotation.union_polygons");
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

Regions annotation_delta(const TriangleMesh& base, const std::vector<int>& sources, const TriangleMesh& current,
                         const Annotation& before, const Annotation& after)
{
    TraceryPerformanceScope performance("vector_annotation_delta");
    performance.stage("annotation_delta.compare_roots");
    using Range = std::pair<size_t, size_t>;
    std::map<int, std::array<Range, 2>> ranges;
    auto collect = [&](const Annotation& annotation, size_t side) {
        const auto& entries = annotation.triangles_to_split;
        for (size_t i = 0; i < entries.size(); ++i) {
            const int facet = entries[i].triangle_idx;
            const size_t begin = size_t(entries[i].bitstream_start_idx);
            const size_t end = i + 1 < entries.size() ? size_t(entries[i + 1].bitstream_start_idx) : annotation.bitstream.size();
            if (facet < 0 || size_t(facet) >= current.its.indices.size() || begin > end || end > annotation.bitstream.size())
                throw std::runtime_error("Invalid annotation facet range");
            ranges[facet][side] = {begin, end};
        }
    };
    collect(before, 0);
    collect(after, 1);
    std::vector<int> changed;
    for (const auto& [facet, pair] : ranges) {
        const auto& a = pair[0];
        const auto& b = pair[1];
        // Compare content, not offsets: editing one root shifts all later roots.
        // Missing roots mean unpainted; explicit NONE trees are conservatively
        // included and their geometric difference will be empty.
        if (a.second - a.first != b.second - b.first ||
            !std::equal(before.bitstream.begin() + a.first, before.bitstream.begin() + a.second,
                        after.bitstream.begin() + b.first))
            changed.push_back(facet);
    }
    performance.count("compared_facets", ranges.size());
    performance.count("changed_facets", changed.size());
    if (changed.empty())
        return {};
    performance.stage("annotation_delta.export_before");
    const auto old_regions = annotation_regions(base, sources, current, before, &changed);
    performance.stage("annotation_delta.export_after");
    const auto new_regions = annotation_regions(base, sources, current, after, &changed);
    performance.stage("annotation_delta.difference");
    return delta(old_regions, new_regions);
}
Regions paint_regions(const TriangleMesh& base, const std::vector<int>& sources, const TriangleSelector::Tracery2DData& input)
{
    TraceryPerformanceScope performance("vector_paint_regions");
    performance.stage("paint.project_polygons");
    Regions result;
    size_t polygon_count = 0;
    size_t input_vertices = 0;
    std::map<int, TriangleSelector::TraceryFacet2DData> frames;
    for (const auto& facet : input) {
        const int  source = sources.at(facet.source_facet_id);
        auto [entry, inserted] = frames.try_emplace(source);
        if (inserted)
            entry->second = plane(base, source);
        const auto& frame = entry->second;
        for (const auto& r : facet.regions) {
            ++polygon_count;
            input_vertices += r.polygon.size();
            Polygon p;
            p.points.reserve(r.polygon.size());
            for (const Vec2d& v : r.polygon)
                p.points.push_back(project(frame, facet.origin + v.x() * facet.axis_u + v.y() * facet.axis_v));
            p.make_counter_clockwise();
            result[source][unsigned(r.color)].push_back(std::move(p));
        }
    }
    performance.count("input_facets", input.size());
    performance.count("input_polygons", polygon_count);
    performance.count("input_vertices", input_vertices);
    performance.stage("paint.union_and_clip");
    size_t rejected = 0, compacted = 0, union_inputs = 0, union_vertices = 0;
    size_t contained_groups = 0, clipped_groups = 0, output_polygons = 0, output_vertices = 0;
    TraceryReductionStatistics reduction;
    for (auto& [id, states] : result) {
        const auto& frame = frames.at(id);
        Polygon boundary;
        for (int vertex : base.its.indices[id]) boundary.points.push_back(project(frame, base.its.vertices[vertex].cast<double>()));
        boundary.make_counter_clockwise();
        const BoundingBox bounds(boundary.points);
        for (auto& [state, polys] : states) {
            {
                TraceryPerformanceScope::Sample sample(performance, "paint.filter_bounds");
                const size_t before = polys.size();
                polys.erase(std::remove_if(polys.begin(), polys.end(), [&](const Polygon& p) {
                    if (p.points.size() < 3)
                        return true;
                    const BoundingBox box(p.points);
                    return box.max.x() < bounds.min.x() || box.min.x() > bounds.max.x() ||
                           box.max.y() < bounds.min.y() || box.min.y() > bounds.max.y();
                }), polys.end());
                rejected += before - polys.size();
            }
            if (polys.empty())
                continue;
            {
                TraceryPerformanceScope::Sample sample(performance, "paint.cancel_shared_edges");
                if (tracery_cancel_internal_edges(polys, &reduction))
                    ++compacted;
            }
            union_inputs += polys.size();
            for (const auto& p : polys)
                union_vertices += p.points.size();
            {
                TraceryPerformanceScope::Sample sample(performance, "paint.union_polygons");
                polys = union_(polys);
            }
            bool contained = int128::orient(boundary.points[0], boundary.points[1], boundary.points[2]) > 0;
            {
                TraceryPerformanceScope::Sample sample(performance, "paint.classify_containment");
                // The source boundary is convex. Exact integer predicates avoid
                // tolerance-dependent leaks and are needed only after union,
                // when most of the interior vertices have already disappeared.
                for (const auto& p : polys) {
                    if (!contained)
                        break;
                    for (const auto& v : p.points) {
                        for (size_t edge = 0; edge < 3; ++edge)
                            if (int128::orient(boundary.points[edge], boundary.points[(edge + 1) % 3], v) < 0) {
                                contained = false;
                                break;
                            }
                        if (!contained)
                            break;
                    }
                }
            }
            if (contained) {
                ++contained_groups;
            } else {
                ++clipped_groups;
                TraceryPerformanceScope::Sample sample(performance, "paint.clip_boundary");
                polys = intersection(polys, Polygons{boundary});
            }
            output_polygons += polys.size();
            for (const auto& p : polys)
                output_vertices += p.points.size();
        }
    }
    performance.count("source_facets", result.size());
    performance.count("bounds_rejected_polygons", rejected);
    performance.count("compacted_groups", compacted);
    performance.count("union_input_polygons", union_inputs);
    performance.count("union_input_vertices", union_vertices);
    performance.count("contained_groups", contained_groups);
    performance.count("clipped_groups", clipped_groups);
    performance.count("output_polygons", output_polygons);
    performance.count("output_vertices", output_vertices);
    performance.count("reduction_local_components", reduction.local_components);
    performance.count("reduction_local_compacted_components", reduction.local_compacted_components);
    performance.count("reduction_recovered_input_polygons", reduction.recovered_input_polygons);
    performance.count("reduction_recovered_output_polygons", reduction.recovered_output_polygons);
    performance.count("reduction_quarantined_polygons", reduction.quarantined_polygons);
    performance.count("reduction_retained_polygons", reduction.retained_polygons);
    if (performance.enabled()) {
        constexpr std::array<const char*, TraceryReductionStatistics::reason_count> reasons{
            "small_batch", "edge_budget", "multiplicity", "few_shared_edges", "branch_vertex",
            "missing_endpoint", "incoming_conflict", "invalid_cycle"};
        // Whole-group failures are diagnostic triggers, not final fallbacks:
        // recovered_input_polygons counts the portion rescued by local reduction.
        for (size_t i = 0; i < reasons.size(); ++i) {
            const std::string prefix = std::string("reduction_") + reasons[i];
            performance.count((prefix + "_group_failures").c_str(), reduction.group_failures[i]);
            performance.count((prefix + "_group_polygons").c_str(), reduction.group_input_polygons[i]);
            performance.count((prefix + "_local_failures").c_str(), reduction.local_failures[i]);
            performance.count((prefix + "_local_polygons").c_str(), reduction.local_input_polygons[i]);
        }
    }
    return result;
}
TriangleSelector::Tracery2DData partition_regions(const TriangleMesh& base, const Regions& input, Annotation* defaults = nullptr)
{
    TriangleSelector::Tracery2DData result;
    TriangleSelector                selector(base);
    for (const auto& [id, states] : input) {
        auto     facet        = plane(base, id);
        unsigned background   = std::numeric_limits<unsigned>::max();
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
    if (m_vector) return apply_vector_edit(current, annotations, paint, edit);
    if (!matches(current) || annotations.size() != 8)
        throw std::runtime_error("Stained-glass base no longer matches this geometry; clear the effect before editing the changed mesh");
    if (edit == TraceryEdit::Erase) {
        if (const char* directory = std::getenv("TRACERY_REPLAY_DIR"); directory && *directory) {
            static std::atomic<unsigned long long> sequence{0};
            const auto                             timestamp = std::chrono::system_clock::now().time_since_epoch().count();
            const std::string                      path      = std::string(directory) + "/erase-" + std::to_string(timestamp) + "-" +
                                     std::to_string(sequence.fetch_add(1)) + ".json";
            const std::string temporary = path + ".tmp";
            std::ofstream     stream(temporary, std::ios::binary | std::ios::trunc);
            stream << encode_replay(annotations, paint, edit);
            stream.flush();
            if (!stream)
                throw std::runtime_error("Unable to save stained-glass diagnostic capture");
            stream.close();
            if (std::rename(temporary.c_str(), path.c_str()) != 0)
                throw std::runtime_error("Unable to publish stained-glass diagnostic capture");
        }
    }
    TraceryPerformanceScope performance("stable_base_rebuild");
    performance.count("base_facets", m_base.its.indices.size());
    performance.count("derived_facets", current.its.indices.size());
    auto channels = updated_channels(current, annotations, paint, edit);
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

std::vector<TraceryDocument::Regions> TraceryDocument::updated_channels(const TriangleMesh&                    current,
                                                                        const std::vector<Annotation>&         annotations,
                                                                        const TriangleSelector::Tracery2DData& paint,
                                                                        TraceryEdit                            edit,
                                                                        Regions*                               slicing,
                                                                        bool                                   full_through) const
{
    TraceryPerformanceScope performance("vector_update_channels");
    performance.stage("vector.copy_channels");
    auto channels = m_channels;
    performance.stage("vector.annotation_deltas");
    for (size_t channel = 0; channel < 5; ++channel) {
        if (annotations[channel] == m_last[channel])
            continue;
        const auto changes = annotation_delta(m_base, m_sources, current, m_last[channel], annotations[channel]);
        overlay(channels[channel], changes);
        if (channel == 0 && slicing)
            overlay(*slicing, changes);
        if (channel == 0)
            overlay(channels[5], changes); // ordinary paint survives erase
    }
    performance.stage("vector.mask_delta");
    channels[7] = channels[6];
    if (annotations[6] != m_last[6]) {
        const auto changes = annotation_delta(m_base, m_sources, current, m_last[6], annotations[6]);
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
            if (slicing)
                overlay(*slicing, restore);
        }
    }
    if (edit == TraceryEdit::Regenerate) {
        channels[0] = channels[5];
        if (slicing)
            *slicing = channels[5];
    }
    performance.stage("vector.project_paint");
    const auto incoming = paint_regions(m_base, m_sources, paint);
    performance.count("paint_facets", paint.size());
    performance.count("incoming_facets", incoming.size());
    performance.stage("vector.overlay_paint");
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
        if (slicing)
            overlay(*slicing, restore);
    } else {
        overlay(channels[0], incoming);
        if (slicing && !full_through)
            overlay(*slicing, incoming);
    }
    channels[7] = channels[6]; // next stroke starts from the committed mask
    return channels;
}

TraceryPartitionResult TraceryDocument::apply_vector_edit(const TriangleMesh&                    current,
                                                          const std::vector<Annotation>&         annotations,
                                                          const TriangleSelector::Tracery2DData& paint,
                                                          TraceryEdit                            edit,
                                                          const TriangleSelector::Tracery2DData* through_entry)
{
    TraceryPerformanceScope performance("vector_apply_edit");
    performance.count("base_facets", m_base.its.indices.size());
    performance.count("edit", static_cast<size_t>(edit));
    performance.stage("vector.validate");
    if (!matches(current) || current.its.vertices != m_base.its.vertices || current.its.indices != m_base.its.indices ||
        annotations.size() != 8)
        throw std::runtime_error("Vector painting requires the unchanged source mesh");
    performance.stage("vector.update_channels");
    const bool separate_surface = through_entry != nullptr || !m_through_operations.empty();
    Regions    slicing;
    if (separate_surface)
        slicing = m_through_operations.empty() ? m_channels[0] : m_slice_regions;
    auto channels   = updated_channels(current, annotations, paint, edit, separate_surface ? &slicing : nullptr, through_entry != nullptr);
    auto operations = edit == TraceryEdit::Regenerate ? std::vector<TraceryThroughOperation>{} : m_through_operations;
    if (through_entry || (edit == TraceryEdit::Erase && !operations.empty())) {
        auto incoming = paint_regions(m_base, m_sources, through_entry ? *through_entry : paint);
        // Surface-mode erase may supply only a selector delta.
        if (edit == TraceryEdit::Erase && incoming.empty()) {
            const auto delta = annotation_delta(m_base, m_sources, current, m_last[6], annotations[6]);
            for (const auto& [facet, states] : delta) {
                auto found = states.find(0);
                if (found != states.end())
                    incoming[facet][0] = found->second;
            }
        }
        TraceryThroughOperation operation;
        operation.erase = edit == TraceryEdit::Erase;
        for (const auto& [id, states] : incoming) {
            const auto  frame     = plane(m_base, id);
            const auto  face      = m_base.its.indices.at(id);
            const Vec3d a         = m_base.its.vertices[face[0]].cast<double>();
            const Vec3d b         = m_base.its.vertices[face[1]].cast<double>();
            const Vec3d c         = m_base.its.vertices[face[2]].cast<double>();
            const Vec3d direction = -(b - a).cross(c - a).normalized();
            if (!direction.allFinite())
                throw std::runtime_error("Invalid through entry face");
            for (const auto& [material, polygons] : states) {
                const auto triangles = triangulate_expolygons_2d(union_ex(polygons));
                for (size_t i = 0; i + 2 < triangles.size(); i += 3) {
                    TraceryThroughPatch patch;
                    patch.direction = direction;
                    patch.material  = material;
                    for (size_t j = 0; j < 3; ++j)
                        patch.entry.emplace_back(frame.origin + triangles[i + j].x() * frame.axis_u + triangles[i + j].y() * frame.axis_v);
                    operation.patches.push_back(std::move(patch));
                }
            }
        }
        if (!operation.patches.empty())
            operations.push_back(std::move(operation));
    }
    performance.stage("vector.publish");
    m_slice_regions      = operations.empty() ? Regions{} : std::move(slicing);
    m_through_operations = std::move(operations);
    m_channels = std::move(channels);
    m_last = annotations;
    m_last[5] = annotations[0]; // ordinary paint remains the editable base on an unchanged mesh
    m_vector = true;
    return {current.its, m_last, m_sources};
}

void TraceryDocument::clear_vector_glass(const TriangleMesh& current, const std::vector<Annotation>& annotations)
{
    TraceryPerformanceScope performance("vector_clear_glass");
    if (!m_vector || !matches(current) || current.its.vertices != m_base.its.vertices || current.its.indices != m_base.its.indices ||
        annotations.size() != 8)
        throw std::runtime_error("Vector painting requires the unchanged source mesh");
    // Preserve ordinary edits without copying or clipping the generated glass.
    performance.stage("clear.sync_annotations");
    for (size_t channel = 0; channel < 5; ++channel) {
        if (annotations[channel] == m_last[channel])
            continue;
        const auto changes = annotation_delta(m_base, m_sources, current, m_last[channel], annotations[channel]);
        if (channel == 0)
            overlay(m_channels[5], changes);
        else
            overlay(m_channels[channel], changes);
    }
    performance.stage("clear.restore_base");
    m_channels[0] = m_channels[5];
    m_slice_regions.clear();
    m_through_operations.clear();
    m_channels[6].clear();
    m_channels[7].clear();
    m_selection_reference = {};
    m_last = annotations;
    m_last[5] = annotations[0];
    m_last[6] = {};
    m_last[7] = {};
}

TriangleSelector::Tracery2DData TraceryDocument::surface_regions() const
{
    return partition_regions(m_base, m_channels.at(0));
}

std::vector<unsigned> TraceryDocument::material_ids() const
{
    std::set<unsigned> ids;
    for (const auto& [facet, states] : m_channels.at(0))
        for (const auto& [state, polygons] : states)
            if (state != 0 && !polygons.empty()) ids.insert(state);
    for (const auto& operation : m_through_operations)
        if (!operation.erase)
            for (const auto& patch : operation.patches)
                if (patch.material)
                    ids.insert(patch.material);
    return {ids.begin(), ids.end()};
}

std::map<unsigned, indexed_triangle_set> TraceryDocument::material_surfaces(bool for_slicing) const
{
    std::map<unsigned, indexed_triangle_set> result;
    for (const auto& [id, states] : (for_slicing && !m_through_operations.empty() ? m_slice_regions : m_channels.at(0))) {
        const auto frame = plane(m_base, id);
        for (const auto& [state, polygons] : states) {
            const auto points = triangulate_expolygons_2d(union_ex(polygons));
            if (points.empty()) continue;
            auto& mesh = result[state];
            for (size_t i = 0; i + 2 < points.size(); i += 3) {
                const int first = int(mesh.vertices.size());
                for (size_t j = 0; j < 3; ++j)
                    mesh.vertices.push_back((frame.origin + points[i+j].x() * frame.axis_u + points[i+j].y() * frame.axis_v).cast<float>());
                mesh.indices.emplace_back(first, first + 1, first + 2);
            }
        }
    }
    return result;
}

void TraceryDocument::apply_ordinary_paint(const TriangleSelector::Tracery2DData& paint)
{
    const auto incoming = paint_regions(m_base, m_sources, paint);
    overlay(m_channels[0], incoming);
    overlay(m_channels[5], incoming);
    if (!m_through_operations.empty())
        overlay(m_slice_regions, incoming);
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
    root["version"]            = format_version();
    root["base"]     = mesh_json(m_base.its);
    root["derived"]  = mesh_json(m_derived);
    root["sources"]  = m_sources;
    root["channels"] = Json::array();
    const size_t channel_count = m_channels.size() + (m_through_operations.empty() ? 0 : 1);
    for (size_t channel_id = 0; channel_id < channel_count; ++channel_id) {
        const auto& channel = channel_id < m_channels.size() ? m_channels[channel_id] : m_slice_regions;
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
    if (!m_through_operations.empty())
        root["through"] = Json::array();
    for (const auto& operation : m_through_operations) {
        Json patches = Json::array();
        for (const auto& patch : operation.patches) {
            Json points = Json::array();
            for (const auto& point : patch.entry)
                points.push_back({point.x(), point.y(), point.z()});
            patches.push_back({patch.material, {patch.direction.x(), patch.direction.y(), patch.direction.z()}, points});
        }
        root["through"].push_back({operation.erase, patches});
    }
    root["selection_reference"] = annotation_json(m_selection_reference);
    root["last"] = Json::array();
    for (const auto& annotation : m_last)
        root["last"].push_back(annotation_json(annotation));
    return root.dump();
}
TraceryDocument TraceryDocument::decode(const std::string& data)
{
    const Json root = Json::parse(data);
    if (root.at("version").get<int>() != 1 && root.at("version").get<int>() != 2 && root.at("version").get<int>() != 3)
        throw std::runtime_error("Unsupported stained-glass editing data version");
    TraceryDocument result;
    result.m_vector  = root.at("version").get<int>() >= 2;
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
    if (root.contains("selection_reference"))
        result.m_selection_reference = read_annotation(root.at("selection_reference"), result.m_sources.size());
    if (root.at("version").get<int>() == 3) {
        if (result.m_channels.size() != 9)
            throw std::runtime_error("Invalid through surface channels");
        result.m_slice_regions = std::move(result.m_channels.back());
        result.m_channels.pop_back();
        for (const auto& entry : root.at("through")) {
            TraceryThroughOperation operation;
            operation.erase = entry.at(0).get<bool>();
            for (const auto& p : entry.at(1)) {
                TraceryThroughPatch patch;
                patch.material  = p.at(0).get<unsigned>();
                const auto& d   = p.at(1);
                patch.direction = Vec3d(d.at(0).get<double>(), d.at(1).get<double>(), d.at(2).get<double>());
                if (!patch.direction.allFinite() || std::abs(patch.direction.norm() - 1.) > 1e-5 ||
                    patch.material > unsigned(EnforcerBlockerType::ExtruderMax))
                    throw std::runtime_error("Invalid through material or direction");
                for (const auto& v : p.at(2)) {
                    Vec3d point(v.at(0).get<double>(), v.at(1).get<double>(), v.at(2).get<double>());
                    if (!point.allFinite())
                        throw std::runtime_error("Invalid through entry vertex");
                    patch.entry.push_back(point);
                }
                if (patch.entry.size() != 3)
                    throw std::runtime_error("Invalid through entry patch");
                operation.patches.push_back(std::move(patch));
            }
            result.m_through_operations.push_back(std::move(operation));
        }
    }
    if (result.m_channels.size() != 8 || result.m_last.size() != 8)
        throw std::runtime_error("Invalid stained-glass channels");
    return result;
}
std::string TraceryDocument::encode_replay(const std::vector<Annotation>&         annotations,
                                           const TriangleSelector::Tracery2DData& paint,
                                           TraceryEdit                            edit) const
{
    Json root;
    root["version"]     = 1;
    root["document"]    = Json::parse(encode());
    root["edit"]        = static_cast<int>(edit);
    root["annotations"] = Json::array();
    for (const auto& annotation : annotations)
        root["annotations"].push_back(annotation_json(annotation));
    root["paint"] = Json::array();
    for (const auto& facet : paint) {
        auto vec3 = [](const Vec3d& v) { return Json::array({v.x(), v.y(), v.z()}); };
        Json f    = {{"source", facet.source_facet_id},
                     {"origin", vec3(facet.origin)},
                     {"u", vec3(facet.axis_u)},
                     {"v", vec3(facet.axis_v)},
                     {"regions", Json::array()}};
        for (const auto& region : facet.regions) {
            Json points = Json::array();
            for (const auto& p : region.polygon)
                points.push_back({p.x(), p.y()});
            f["regions"].push_back({{"id", region.fragment_id}, {"color", unsigned(region.color)}, {"points", std::move(points)}});
        }
        root["paint"].push_back(std::move(f));
    }
    return root.dump();
}
TraceryPartitionResult TraceryDocument::replay(const std::string& data)
{
    const Json root = Json::parse(data);
    if (root.at("version") != 1)
        throw std::runtime_error("Unsupported stained-glass replay version");
    auto                    document = decode(root.at("document").dump());
    std::vector<Annotation> annotations;
    for (const auto& a : root.at("annotations"))
        annotations.push_back(read_annotation(a, document.m_derived.indices.size()));
    TriangleSelector::Tracery2DData paint;
    for (const auto& f : root.at("paint")) {
        auto vec3 = [](const Json& v) { return Vec3d(v.at(0).get<double>(), v.at(1).get<double>(), v.at(2).get<double>()); };
        TriangleSelector::TraceryFacet2DData facet;
        facet.source_facet_id = f.at("source").get<int>();
        facet.origin          = vec3(f.at("origin"));
        facet.axis_u          = vec3(f.at("u"));
        facet.axis_v          = vec3(f.at("v"));
        for (const auto& r : f.at("regions")) {
            TriangleSelector::TraceryRegion2D region;
            region.fragment_id = r.at("id").get<decltype(region.fragment_id)>();
            region.color       = EnforcerBlockerType(r.at("color").get<unsigned>());
            for (const auto& p : r.at("points"))
                region.polygon.emplace_back(p.at(0).get<double>(), p.at(1).get<double>());
            facet.regions.push_back(std::move(region));
        }
        paint.push_back(std::move(facet));
    }
    const int edit = root.at("edit").get<int>();
    if (edit < int(TraceryEdit::Paint) || edit > int(TraceryEdit::Regenerate))
        throw std::runtime_error("Invalid stained-glass replay edit");
    return document.rebuild(TriangleMesh(document.m_derived), annotations, paint, TraceryEdit(edit));
}
TriangleSelector::Tracery2DData TraceryDocument::erased_regions(const TriangleMesh& mesh, const Annotation& before, const Annotation& after)
{
    std::vector<int> sources(mesh.its.indices.size());
    std::iota(sources.begin(), sources.end(), 0);
    const auto changes = annotation_delta(mesh, sources, mesh, before, after);
    Regions    erased;
    for (const auto& [facet, states] : changes)
        if (auto found = states.find(0); found != states.end())
            erased[facet][1] = found->second;
    return partition_regions(mesh, erased);
}
void TraceryDocument::remap_materials(const EnforcerBlockerStateMap& mapping)
{
    for (auto& operation : m_through_operations)
        if (!operation.erase)
            for (auto& patch : operation.patches)
                patch.material = unsigned(mapping.at(patch.material));
    for (auto& [facet, states] : m_slice_regions) {
        std::map<unsigned, Polygons> mapped;
        for (const auto& [state, polygons] : states)
            append(mapped[unsigned(mapping.at(state))], polygons);
        for (auto& [state, polygons] : mapped)
            polygons = union_(polygons);
        states = std::move(mapped);
    }
    const TriangleMesh current(m_derived);
    TriangleSelector reference(current);
    reference.deserialize(m_selection_reference, false);
    reference.remap_triangle_state(mapping);
    m_selection_reference = reference.serialize();
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

namespace Slic3r {
std::unique_ptr<TraceryDocument> TraceryDocument::from_volume(const ModelVolume& volume)
{
    const auto snapshot = from_volume_snapshot(volume);
    return snapshot ? std::make_unique<TraceryDocument>(*snapshot) : nullptr;
}

std::shared_ptr<const TraceryDocument> TraceryDocument::from_volume_snapshot(const ModelVolume& volume)
{
    TraceryPerformanceScope performance("vector_document_snapshot");
    if (volume.tracery_edit_data.empty()) return {};
    performance.stage("snapshot.decode");
    const auto cached = decode_cached(volume.tracery_edit_data);
    if (!cached->is_vector()) return {};
    if (!cached->matches(volume.mesh()) || cached->base_mesh().its.vertices != volume.mesh().its.vertices ||
        cached->base_mesh().its.indices != volume.mesh().its.indices)
        throw std::runtime_error("Vector painting requires the unchanged source mesh");
    performance.stage("snapshot.check_annotations");
    const auto& last = cached->last_annotations();
    if (last[0] == volume.mmu_segmentation_facets.get_data() && last[1] == volume.supported_facets.get_data() &&
        last[2] == volume.seam_facets.get_data() && last[3] == volume.fuzzy_skin_facets.get_data() &&
        last[4] == volume.exterior_facets.get_data()) {
        performance.count("annotations_unchanged", 1);
        return cached;
    }
    performance.count("annotations_unchanged", 0);
    performance.stage("snapshot.apply_annotation_changes");
    auto doc = std::make_shared<TraceryDocument>(*cached);
    auto channels = doc->last_annotations();
    channels[0] = volume.mmu_segmentation_facets.get_data();
    channels[1] = volume.supported_facets.get_data();
    channels[2] = volume.seam_facets.get_data();
    channels[3] = volume.fuzzy_skin_facets.get_data();
    channels[4] = volume.exterior_facets.get_data();
    doc->apply_vector_edit(volume.mesh(), channels, {}, TraceryEdit::Paint);
    return doc;
}
}
