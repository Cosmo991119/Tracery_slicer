#ifndef libslic3r_TraceryVectorFill_hpp_
#define libslic3r_TraceryVectorFill_hpp_

#include "TriangleSelector.hpp"
#include <cstdint>

namespace Slic3r {

// A query cache for an immutable source-paint selector. Recreate this object
// whenever that selector's geometry or paint changes. Preview flags and the
// blocked-material policy may change without rebuilding the topology cache.
class TraceryVectorFill
{
public:
    explicit TraceryVectorFill(TriangleSelector& source) : m_source(source) {}
    const TriangleSelector::Tracery2DData& select(const Vec3f& hit, int facet,
                                                 const TriangleSelector::ClippingPlane& clip,
                                                 float angle, bool connected);
    size_t revision() const { return m_revision; }
    bool clear_preview();
    bool show_preview();
    const TriangleSelector::Tracery2DData& regions() const { return m_regions; }

private:
    TriangleSelector& m_source;
    std::vector<Vec3i32> m_neighbors, m_propagated;
    std::vector<int> m_selected;
    std::vector<uint32_t> m_members;
    uint32_t m_epoch = 0;
    size_t m_revision = 0;
    TriangleSelector::Tracery2DData m_regions;
    TriangleSelector::ClippingPlane m_clip;
    std::set<EnforcerBlockerType> m_blocked;
    float m_angle = 0.f;
    bool m_connected = false, m_valid = false, m_preview = false, m_preview_dirty = false;
};

} // namespace Slic3r
#endif
