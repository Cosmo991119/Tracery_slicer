#ifndef slic3r_TraceryBasePaint_hpp_
#define slic3r_TraceryBasePaint_hpp_

#include <vector>

namespace Slic3r {

// An empty saved annotation is a valid snapshot. Only the presence of a
// matching document, not the annotation contents, determines precedence.
template<class Paint>
void tracery_append_base_paint(std::vector<Paint>& snapshots, const Paint& current, const Paint* saved)
{
    snapshots.push_back(saved != nullptr ? *saved : current);
}

} // namespace Slic3r

#endif
