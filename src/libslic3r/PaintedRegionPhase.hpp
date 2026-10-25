#ifndef slic3r_PaintedRegionPhase_hpp_
#define slic3r_PaintedRegionPhase_hpp_

#include "ExPolygon.hpp"
#include <functional>
#include <vector>

namespace Slic3r {

// Local layer indices for mixed painting, indexed by layer * channel_count + channel,
// then by the source ExPolygon index. Disconnected 3D zones have independent origins.
std::vector<std::vector<int>> compute_painted_region_phases(const std::vector<std::vector<ExPolygons>>& segmentation,
                                                          const std::vector<bool>& enabled_channels,
                                                          const std::function<void()>& throw_on_cancel);

} // namespace Slic3r
#endif
