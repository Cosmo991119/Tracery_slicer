#include "TraceryRestore.hpp"
#include "ClipperUtils.hpp"
#include "Tesselate.hpp"
#include "Model.hpp"
#include <map>
#include <numeric>
#include <stdexcept>

namespace Slic3r {

bool restore_tracery_base_paint(ModelVolume& volume, const TriangleSelector::TriangleSplittingData& paint)
{
    if (volume.mmu_segmentation_facets.get_data() == paint)
        return false;
    TriangleSelector selector(volume.mesh());
    selector.deserialize(paint, false);
    return volume.mmu_segmentation_facets.set(selector);
}

namespace {
using Annotation     = TriangleSelector::TriangleSplittingData;
using RegionKey      = std::pair<int, EnforcerBlockerType>;
using RestoreRegions = std::map<RegionKey, Polygons>;

TriangleSelector::TraceryFacet2DData restore_facet_plane(const TriangleMesh& mesh, int facet)
{
    const auto&                          t = mesh.its.indices.at(facet);
    TriangleSelector::TraceryFacet2DData plane;
    plane.source_facet_id = facet;
    plane.origin          = mesh.its.vertices[t[0]].cast<double>();
    const Vec3d u         = mesh.its.vertices[t[1]].cast<double>() - plane.origin;
    const Vec3d v         = mesh.its.vertices[t[2]].cast<double>() - plane.origin;
    plane.axis_u          = u.normalized();
    plane.axis_v          = u.cross(v).normalized().cross(plane.axis_u);
    return plane;
}

RestoreRegions project_restore_regions(const TraceryRestoreState& state, const TriangleMesh& mesh, const Annotation& annotation)
{
    TriangleSelector selector(mesh);
    selector.deserialize(annotation, false);
    RestoreRegions regions;
    for (const auto& leaf : selector.painted_leaves(true)) {
        const int  original = state.original_facets.at(leaf.source_facet);
        const auto plane    = restore_facet_plane(state.original_mesh, original);
        Polygon    polygon;
        for (const Vec3f& vertex : leaf.vertices) {
            const Vec3d p = vertex.cast<double>() - plane.origin;
            polygon.points.emplace_back(scale_(p.dot(plane.axis_u)), scale_(p.dot(plane.axis_v)));
        }
        polygon.make_counter_clockwise();
        regions[{original, leaf.state}].push_back(std::move(polygon));
    }
    for (auto& [key, polygons] : regions)
        polygons = union_(polygons);
    return regions;
}

TriangleSelector::Tracery2DData changed_annotation_regions(const TraceryRestoreState& state,
                                                           const TriangleMesh&        mesh,
                                                           const Annotation&          before,
                                                           const Annotation&          after)
{
    const auto                                          old_regions = project_restore_regions(state, mesh, before);
    const auto                                          new_regions = project_restore_regions(state, mesh, after);
    std::map<int, TriangleSelector::TraceryFacet2DData> planes;
    for (const auto& [key, polygons] : new_regions) {
        const auto       old     = old_regions.find(key);
        const ExPolygons changed = diff_ex(polygons, old == old_regions.end() ? Polygons{} : old->second);
        if (changed.empty())
            continue;
        auto [entry, inserted] = planes.emplace(key.first, restore_facet_plane(state.original_mesh, key.first));
        const auto triangles   = triangulate_expolygons_2d(changed);
        for (size_t i = 0; i + 2 < triangles.size(); i += 3) {
            TriangleSelector::TraceryRegion2D region;
            region.polygon = {triangles[i], triangles[i + 1], triangles[i + 2]};
            region.color   = key.second;
            region.erase   = key.second == EnforcerBlockerType::NONE;
            entry->second.regions.push_back(std::move(region));
        }
    }
    TriangleSelector::Tracery2DData data;
    for (auto& [id, plane] : planes)
        data.push_back(std::move(plane));
    return data;
}

std::vector<Annotation> restore_tracery_annotations(const TraceryRestoreState&     state,
                                                    const TriangleMesh&            mesh,
                                                    const std::vector<Annotation>& annotations)
{
    if (mesh.its.indices.size() != state.original_facets.size() || annotations.size() != state.last_annotations.size() ||
        annotations.size() != state.original_annotations.size())
        throw std::runtime_error("Stained-glass restore snapshot does not match the current mesh");
    auto restored = state.original_annotations;
    for (size_t channel = 0; channel < annotations.size(); ++channel) {
        if (annotations[channel] == state.last_annotations[channel])
            continue;
        const auto       changes = changed_annotation_regions(state, mesh, state.last_annotations[channel], annotations[channel]);
        TriangleSelector selector(state.original_mesh);
        selector.deserialize(restored[channel], false);
        // Later edits remain ordinary selector paint, including erasing NONE.
        // Only annotation leaves are split; the restored model mesh stays exact.
        selector.paint_all_tracery_regions(changes, 0.05f);
        restored[channel] = selector.serialize();
    }
    return restored;
}
} // namespace

TraceryRestoreState capture_tracery_restore_state(const TriangleMesh&                                         mesh,
                                                  const std::vector<TriangleSelector::TriangleSplittingData>& annotations)
{
    TraceryRestoreState state;
    state.original_mesh        = mesh;
    state.original_annotations = state.last_annotations = annotations;
    state.original_facets.resize(mesh.its.indices.size());
    std::iota(state.original_facets.begin(), state.original_facets.end(), 0);
    return state;
}

TraceryPartitionResult restore_tracery_mesh(const TraceryRestoreState&                                  state,
                                            const TriangleMesh&                                         current_mesh,
                                            const std::vector<TriangleSelector::TriangleSplittingData>& current_annotations)
{ return {state.original_mesh.its, restore_tracery_annotations(state, current_mesh, current_annotations), {}}; }

void advance_tracery_restore_state(TraceryRestoreState&                                        state,
                                   const TriangleMesh&                                         current_mesh,
                                   const std::vector<TriangleSelector::TriangleSplittingData>& current_annotations,
                                   const TraceryPartitionResult&                               partition)
{
    if (partition.annotations.size() < current_annotations.size() || partition.source_facets.size() != partition.mesh.indices.size())
        throw std::runtime_error("Invalid stained-glass partition restore mapping");
    const auto       annotations = restore_tracery_annotations(state, current_mesh, current_annotations);
    std::vector<int> sources;
    for (int facet : partition.source_facets)
        sources.push_back(state.original_facets.at(facet));
    state.original_facets      = std::move(sources);
    state.original_annotations = annotations;
    state.last_annotations.assign(partition.annotations.begin(), partition.annotations.begin() + current_annotations.size());
}
} // namespace Slic3r
