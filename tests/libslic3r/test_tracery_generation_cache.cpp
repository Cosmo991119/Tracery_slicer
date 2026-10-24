#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "libslic3r/TraceryGenerationCache.hpp"
#include "libslic3r/TraceryRegionMapping.hpp"

using namespace Slic3r;

TEST_CASE("Density preparation reuses unchanged input and rejects changed source data", "[TraceryGenerationCache]")
{
    indexed_triangle_set its;
    its.vertices          = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}};
    its.indices           = {{0, 1, 2}};
    auto             mesh = std::make_shared<const TriangleMesh>(its);
    TriangleSelector selected(*mesh), ordinary(*mesh);
    selected.set_facet(0, EnforcerBlockerType::ENFORCER);
    ordinary.set_facet(0, EnforcerBlockerType(5));
    std::array<double, 16> transform{};
    transform[0] = transform[5] = transform[10] = transform[15] = 1.;
    TraceryGenerationKey key{{mesh, transform, 5, selected.serialize(), ordinary.serialize(), {{1.f, 0.f, 0.f, 1.f}}}};
    TraceryGenerationCache<indexed_triangle_set> cache;
    REQUIRE_FALSE(cache.find(key));
    auto prepared = std::make_shared<indexed_triangle_set>(selected.get_facets_strict_with_sources(EnforcerBlockerType::ENFORCER).facets);
    cache.store(key, prepared);
    REQUIRE(cache.find(key) == prepared);
    auto      changed = key;
    const int change  = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    if (change == 0)
        std::get<0>(changed[0]) = std::make_shared<const TriangleMesh>(its);
    if (change == 1)
        std::get<1>(changed[0])[12] = 20.;
    if (change == 2)
        std::get<2>(changed[0]) = 6;
    if (change == 3)
        std::get<3>(changed[0]) = TriangleSelector::TriangleSplittingData{};
    if (change == 4)
        std::get<4>(changed[0]) = TriangleSelector::TriangleSplittingData{};
    if (change == 5)
        std::get<5>(changed[0])[0] = {0.f, 1.f, 0.f, 1.f};
    if (change == 6)
        changed.push_back(changed.front());
    if (change == 7)
        std::get<5>(changed[0])[0][0] -= 0.001f; // smaller than display colour equality tolerance
    CHECK_FALSE(cache.find(changed));
    CHECK(cache.find(key)->indices == its.indices);
    // Publishing another complete input evicts the previous prepared selection.
    cache.store(changed, std::make_shared<indexed_triangle_set>());
    CHECK_FALSE(cache.find(key));
    CHECK(cache.find(changed)->indices.empty());
}

TEST_CASE("Vector region reuse requires exact geometry and facet identity", "[TraceryGenerationCache]")
{
    indexed_triangle_set mesh;
    mesh.vertices = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}, {10, 10, 0}};
    mesh.indices  = {{0, 1, 2}, {1, 3, 2}};
    TraceryPartitionResult partition{mesh, {}, {0, 1}};
    std::vector<int>       previous{0, 1};
    REQUIRE(tracery_has_identity_mapping(mesh, partition, previous));
    const int change = GENERATE(0, 1, 2, 3, 4);
    if (change == 0)
        partition.mesh.vertices[0].x() += 1.f;
    if (change == 1)
        std::swap(partition.mesh.indices[0], partition.mesh.indices[1]);
    if (change == 2)
        partition.source_facets[0] = 1;
    if (change == 3)
        previous[0] = 1;
    if (change == 4)
        previous.pop_back();
    CHECK_FALSE(tracery_has_identity_mapping(mesh, partition, previous));
}
