#ifndef slic3r_TraceryPartition_hpp_
#define slic3r_TraceryPartition_hpp_

#include "TriangleSelector.hpp"

namespace Slic3r {

// Channel zero is MMU paint. Other channels are preserved annotations, including
// editor masks. Every channel is rebuilt against the returned mesh atomically.
struct TraceryPartitionResult
{
    indexed_triangle_set                                 mesh;
    std::vector<TriangleSelector::TriangleSplittingData> annotations;
    std::vector<int>                                     source_facets;
};

// Throws on invalid geometry; the caller's mesh/annotations are never mutated.
TraceryPartitionResult build_exact_tracery_partition(const TriangleMesh&                                         source,
                                                     const std::vector<TriangleSelector::TriangleSplittingData>& annotations,
                                                     const TriangleSelector::Tracery2DData&                      regions,
                                                     const std::function<void()>&                                cancel = {});

// Clips retained editor geometry when original facet IDs are replaced.
std::vector<Vec3f> clip_tracery_polygon_to_facet(const indexed_triangle_set& mesh, int facet, const std::vector<Vec3f>& polygon);

// Paint IDs still needed outside the pending glass regions. The volume's base
// material and the new recipes are budgeted separately by the caller.
std::vector<unsigned int> tracery_retained_paint_materials(const TriangleMesh& source,
    const TriangleSelector::TriangleSplittingData& paint, const TriangleSelector::Tracery2DData& replacement);

} // namespace Slic3r
#endif
