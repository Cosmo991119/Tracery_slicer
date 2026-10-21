#ifndef slic3r_TraceryRegionMapping_hpp_
#define slic3r_TraceryRegionMapping_hpp_

#include "TraceryPartition.hpp"
#include <memory>

namespace Slic3r {

struct TraceryMappingStatistics
{
    size_t candidate_pairs = 0;
    size_t spatial_candidates = 0;
    size_t clip_calls = 0;
    size_t separated_candidates = 0;
    size_t inherited_regions = 0;
    size_t inherited_facets = 0;
    size_t nonempty_results = 0;
};

struct TraceryMappedRegion
{
    int facet;
    std::vector<Vec3f> polygon;
};

namespace detail {
// Shares the exact clipping frame/quantization with the geometry partitioner.
class TraceryFacetClipper
{
public:
    TraceryFacetClipper(const indexed_triangle_set& mesh, int facet);
    ~TraceryFacetClipper();
    std::vector<Vec3f> clip(const std::vector<Vec3f>& polygon, TraceryMappingStatistics& statistics) const;
    size_t estimated_bytes() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace detail

// One immutable partition index is shared by the new brush regions and every
// retained selection. Input order/identity is owned by the caller, never color.
// The partition must outlive this mapper and remain unchanged. Lazy clipping
// frames make map() a single-threaded operation; the source is not retained.
class TraceryRegionMapper
{
public:
    TraceryRegionMapper(const indexed_triangle_set& source, const TraceryPartitionResult& partition);
    ~TraceryRegionMapper();
    TraceryRegionMapper(const TraceryRegionMapper&) = delete;
    TraceryRegionMapper& operator=(const TraceryRegionMapper&) = delete;
    std::vector<TraceryMappedRegion> map(int source_facet, const std::vector<Vec3f>& polygon, TraceryMappingStatistics& statistics);
    size_t estimated_bytes() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Slic3r
#endif
