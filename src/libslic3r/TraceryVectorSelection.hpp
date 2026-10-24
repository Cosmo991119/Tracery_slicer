#ifndef slic3r_TraceryVectorSelection_hpp_
#define slic3r_TraceryVectorSelection_hpp_

#include "TriangleSelector.hpp"
#include "Polygon.hpp"
#include <map>
#include <set>

namespace Slic3r {

// A mask in immutable source-facet coordinates. These polygons are never a
// replacement mesh and never change the ordinary paint's subdivision tree.
class TraceryVectorSelection
{
public:
    using Data    = TriangleSelector::Tracery2DData;
    using Regions = std::map<int, Polygons>;
    static TraceryVectorSelection from_data(const TriangleMesh& mesh, const Data& data);
    static TraceryVectorSelection from_annotation(const TriangleMesh& mesh, const TriangleSelector::TriangleSplittingData& annotation);
    static TriangleSelector::TraceryFacet2DData frame(const TriangleMesh& mesh, int facet);
    void                                        apply(const TraceryVectorSelection& stroke, bool erase);
    TraceryVectorSelection                      difference(const TraceryVectorSelection& other) const;
    Data                                        triangulated(const TriangleMesh& mesh) const;
    Data                                        colored(const TriangleMesh&                            mesh,
                                                        const TriangleSelector::TriangleSplittingData& paint,
                                                        const std::set<EnforcerBlockerType>&           blocked = {}) const;
    void                                        restrict_to(const TraceryVectorSelection& allowed);
    double                                      area() const;
    bool                                        empty() const { return m_regions.empty(); }
    const Regions&                              regions() const { return m_regions; }
    bool                                        operator==(const TraceryVectorSelection& other) const;

private:
    Regions               m_regions;
    std::map<int, double> m_area_factors;
};

} // namespace Slic3r
#endif
