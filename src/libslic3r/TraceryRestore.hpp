#ifndef slic3r_TraceryRestore_hpp_
#define slic3r_TraceryRestore_hpp_

#include "TraceryPartition.hpp"

namespace Slic3r {

struct TraceryRestoreState
{
    TriangleMesh                                         original_mesh;
    std::vector<TriangleSelector::TriangleSplittingData> original_annotations;
    std::vector<TriangleSelector::TriangleSplittingData> last_annotations;
    std::vector<int>                                     original_facets;

    template<class Archive> void serialize(Archive& ar) { ar(original_mesh, original_annotations, last_annotations, original_facets); }
};

TraceryRestoreState capture_tracery_restore_state(const TriangleMesh&                                         mesh,
                                                  const std::vector<TriangleSelector::TriangleSplittingData>& annotations);

TraceryPartitionResult restore_tracery_mesh(const TraceryRestoreState&                                  state,
                                            const TriangleMesh&                                         current_mesh,
                                            const std::vector<TriangleSelector::TriangleSplittingData>& current_annotations);

void advance_tracery_restore_state(TraceryRestoreState&                                        state,
                                   const TriangleMesh&                                         current_mesh,
                                   const std::vector<TriangleSelector::TriangleSplittingData>& current_annotations,
                                   const TraceryPartitionResult&                               partition);

} // namespace Slic3r
#endif
