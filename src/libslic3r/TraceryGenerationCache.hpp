#ifndef slic3r_TraceryGenerationCache_hpp_
#define slic3r_TraceryGenerationCache_hpp_

#include "TriangleSelector.hpp"
#include <array>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>

namespace Slic3r {

// Exact input identity, independent of density and generated paint.
// Holding the immutable mesh prevents pointer reuse after object replacement.
using TraceryGenerationVolumeKey = std::tuple<std::shared_ptr<const TriangleMesh>,
                                              std::array<double, 16>,
                                              int,
                                              TriangleSelector::TriangleSplittingData,
                                              TriangleSelector::TriangleSplittingData,
                                              std::vector<std::array<float, 4>>>;
using TraceryGenerationKey       = std::vector<TraceryGenerationVolumeKey>;

// One prepared selection only. No serialized cache and no retained GUI pointers.
template<class Prepared> class TraceryGenerationCache
{
public:
    std::shared_ptr<Prepared> find(const TraceryGenerationKey& key) const { return m_key && *m_key == key ? m_value : nullptr; }
    void                      store(TraceryGenerationKey key, std::shared_ptr<Prepared> value)
    {
        m_key   = std::move(key);
        m_value = std::move(value);
    }

private:
    std::optional<TraceryGenerationKey> m_key;
    std::shared_ptr<Prepared>           m_value;
};

} // namespace Slic3r
#endif
