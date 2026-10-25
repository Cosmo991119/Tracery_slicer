#include "PaintedRegionPhase.hpp"
#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include <boost/pending/disjoint_sets.hpp>
#include <limits>

namespace Slic3r {
namespace {
struct PhaseRegion
{
    const ExPolygon* mask;
    BoundingBox      bbox;
    size_t           layer;
    size_t           component;
};

static std::vector<std::vector<int>> channel_layer_phases(const std::vector<std::vector<ExPolygons>>& segmentation,
                                                        size_t channel, const std::function<void()>& throw_on_cancel)
{
    std::vector<PhaseRegion> nodes;
    std::vector<size_t> layer_begin(segmentation.size() + 1);
    std::vector<std::vector<int>> phases(segmentation.size());
    for (size_t layer = 0; layer < segmentation.size(); ++layer) {
        throw_on_cancel();
        layer_begin[layer] = nodes.size();
        if (channel >= segmentation[layer].size())
            continue;
        const auto& masks = segmentation[layer][channel];
        phases[layer].assign(masks.size(), -1);
        for (size_t component = 0; component < masks.size(); ++component)
            if (!masks[component].empty())
                nodes.push_back({&masks[component], get_extents(masks[component]), layer, component});
    }
    layer_begin.back() = nodes.size();
    boost::disjoint_sets_with_storage<> connected(nodes.size());
    // Test shared interior, not just boundary crossings: almost-contained
    // polygons with coincident edges can fool ExPolygon::overlaps().
    for (size_t layer = 1; layer < segmentation.size(); ++layer) {
        throw_on_cancel();
        for (size_t current = layer_begin[layer]; current < layer_begin[layer + 1]; ++current)
            for (size_t previous = layer_begin[layer - 1]; previous < layer_begin[layer]; ++previous)
                if (nodes[current].bbox.overlap(nodes[previous].bbox) &&
                    !intersection_ex(*nodes[previous].mask, *nodes[current].mask).empty())
                    connected.union_set(previous, current);
    }
    // Finish connectivity before assigning origins: a later merge must align
    // the earlier branches too, otherwise their cadence jumps at that merge.
    std::vector<int> origin(nodes.size(), std::numeric_limits<int>::max());
    for (size_t i = 0; i < nodes.size(); ++i) {
        const size_t root = connected.find_set(i);
        origin[root] = std::min(origin[root], int(nodes[i].layer));
    }
    for (size_t i = 0; i < nodes.size(); ++i)
        phases[nodes[i].layer][nodes[i].component] = int(nodes[i].layer) - origin[connected.find_set(i)];
    return phases;
}
} // namespace

std::vector<std::vector<int>> compute_painted_region_phases(const std::vector<std::vector<ExPolygons>>& segmentation,
                                                          const std::vector<bool>& enabled_channels,
                                                          const std::function<void()>& throw_on_cancel)
{
    const size_t channels = enabled_channels.size();
    std::vector<std::vector<int>> result(segmentation.size() * channels);
    for (size_t channel = 1; channel < channels; ++channel) {
        throw_on_cancel();
        if (!enabled_channels[channel])
            continue;
        auto phases = channel_layer_phases(segmentation, channel, throw_on_cancel);
        for (size_t layer = 0; layer < phases.size(); ++layer)
            result[layer * channels + channel] = std::move(phases[layer]);
    }
    return result;
}

} // namespace Slic3r
