#ifndef slic3r_TraceryPreviousFacetIndex_hpp_
#define slic3r_TraceryPreviousFacetIndex_hpp_

#include <cstddef>
#include <unordered_map>
#include <vector>

namespace Slic3r {

// Retain input order, so the same candidate wins as in the original linear scan.
class TraceryPreviousFacetIndex
{
public:
    void add(int source_facet, size_t index) { m_facets[source_facet].push_back(index); }

    const std::vector<size_t>& candidates(int source_facet) const
    {
        const auto found = m_facets.find(source_facet);
        if (found != m_facets.end())
            return found->second;
        static const std::vector<size_t> empty;
        return empty;
    }

private:
    std::unordered_map<int, std::vector<size_t>> m_facets;
};

} // namespace Slic3r
#endif
