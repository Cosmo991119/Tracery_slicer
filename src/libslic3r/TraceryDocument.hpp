#ifndef slic3r_TraceryDocument_hpp_
#define slic3r_TraceryDocument_hpp_
#include "TraceryPartition.hpp"
#include "Polygon.hpp"
#include <map>

namespace Slic3r {
enum class TraceryEdit { Paint, Erase, Regenerate };

// Authoritative regions live in immutable base-facet coordinates. Neither the
// generated mesh nor its triangle IDs are ever promoted into the base.
class TraceryDocument
{
public:
    using Annotation = TriangleSelector::TriangleSplittingData;
    using Regions    = std::map<int, std::map<unsigned, Polygons>>;
    static TraceryDocument                 capture(const TriangleMesh& mesh, const std::vector<Annotation>& annotations);
    TraceryPartitionResult                 rebuild(const TriangleMesh&                    current,
                                                   const std::vector<Annotation>&         annotations,
                                                   const TriangleSelector::Tracery2DData& paint,
                                                   TraceryEdit                            edit);
    const TriangleMesh&                    base_mesh() const { return m_base; }
    const std::vector<int>&                source_facets() const { return m_sources; }
    const std::vector<Annotation>&         last_annotations() const { return m_last; }
    bool                                   matches(const TriangleMesh& mesh) const;
    std::string                            encode() const;
    static TraceryDocument                 decode(const std::string& data);
    static TriangleSelector::Tracery2DData erased_regions(const TriangleMesh& mesh, const Annotation& before, const Annotation& after);
    void                                   remap_materials(const EnforcerBlockerStateMap& mapping);

private:
    TriangleMesh            m_base;
    indexed_triangle_set    m_derived;
    std::vector<int>        m_sources;
    std::vector<Regions>    m_channels;
    std::vector<Annotation> m_last;
};
} // namespace Slic3r
#endif
