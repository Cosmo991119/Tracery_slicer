#ifndef slic3r_TraceryDisjointSet_hpp_
#define slic3r_TraceryDisjointSet_hpp_

#include <cstddef>
#include <numeric>
#include <utility>
#include <vector>

namespace Slic3r {

class TraceryDisjointSet
{
public:
    explicit TraceryDisjointSet(size_t count) : m_parent(count), m_size(count, 1)
    {
        std::iota(m_parent.begin(), m_parent.end(), size_t(0));
    }

    size_t root(size_t index)
    {
        size_t representative = index;
        while (m_parent[representative] != representative)
            representative = m_parent[representative];
        while (m_parent[index] != index) {
            const size_t next = m_parent[index];
            m_parent[index] = representative;
            index = next;
        }
        return representative;
    }

    void unite(size_t first, size_t second)
    {
        first = root(first);
        second = root(second);
        if (first == second)
            return;
        if (m_size[first] < m_size[second])
            std::swap(first, second);
        m_parent[second] = first;
        m_size[first] += m_size[second];
    }

private:
    std::vector<size_t> m_parent;
    std::vector<size_t> m_size;
};

} // namespace Slic3r
#endif
