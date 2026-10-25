#include "libslic3r/PaintedRegionPhase.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace Slic3r;

namespace {
ExPolygon box(double x0, double y0, double x1, double y1)
{
    return ExPolygon(Polygon(Points{Point(scale_(x0), scale_(y0)), Point(scale_(x1), scale_(y0)),
                                    Point(scale_(x1), scale_(y1)), Point(scale_(x0), scale_(y1))}));
}
}

TEST_CASE("Mixed paint keeps one cadence when islands merge and split", "[PaintedRegionPhase]")
{
    std::vector<std::vector<ExPolygons>> layers(4, std::vector<ExPolygons>(2));
    layers[0][1] = {box(0, 0, 2, 2)};
    layers[1][1] = {box(0, 0, 2, 2), box(4, 0, 6, 2)};
    layers[2][1] = {box(0, 0, 6, 2)};
    layers[3][1] = layers[1][1];
    const auto phases = compute_painted_region_phases(layers, {false, true}, [] {});
    for (size_t layer = 0; layer < layers.size(); ++layer) {
        REQUIRE(phases[layer * 2 + 1].size() == layers[layer][1].size());
        for (int phase : phases[layer * 2 + 1]) {
            CAPTURE(layer, phase);
            CHECK(phase == int(layer));
        }
    }
}

TEST_CASE("Mixed paint retains cadence across nearly coincident contour edges", "[PaintedRegionPhase]")
{
    // Actual layers 75/76, material 22. The earlier region is almost contained
    // by the later one; a one-unit protrusion makes the old boundary-only overlap
    // check miss their substantial common interior and restart the recipe.
    std::vector<std::vector<ExPolygons>> layers(2, std::vector<ExPolygons>(2));
    layers[0][1] = {ExPolygon(Polygon(Points{{30943684, -1835520}, {30943683, -1835520},
                                           {30943683, -2255093}, {31363256, -2255093}}))};
    layers[1][1] = {ExPolygon(Polygon(Points{{30545522, -1437358}, {30491332, -1381373}, {30491331, -1381372},
                                           {30491331, -2255093}, {31363256, -2255093}}))};
    const auto phases = compute_painted_region_phases(layers, {false, true}, [] {});
    REQUIRE(phases[3].size() == 1);
    CHECK(phases[3][0] == 1);
}

TEST_CASE("Mixed paint keeps independent colors and disconnected activations separate", "[PaintedRegionPhase]")
{
    std::vector<std::vector<ExPolygons>> layers(5, std::vector<ExPolygons>(4));
    layers[0][1] = {box(0, 0, 2, 2)};
    layers[1][1] = {box(0, 0, 2, 2), box(5, 0, 7, 2)};
    layers[2][1] = layers[1][1];
    // A blank layer ends activation. Reappearing at the same XY starts afresh.
    layers[4][1] = {box(0, 0, 2, 2)};
    layers[1][2] = {box(0, 0, 2, 2)};
    layers[2][2] = layers[1][2];
    layers[0][3] = layers[0][1]; // Physical/disabled channel has no mixed phase.
    const auto phases = compute_painted_region_phases(layers, {false, true, true, false}, [] {});
    CHECK(phases[9] == std::vector<int>{2, 1});
    CHECK(phases[10] == std::vector<int>{1});
    CHECK(phases[17] == std::vector<int>{0});
    CHECK(phases[3].empty());
    CHECK(phases[13].empty());
}

TEST_CASE("Mixed phase connectivity respects holes and requires shared interior", "[PaintedRegionPhase]")
{
    auto ring = box(0, 0, 10, 10);
    auto hole = box(3, 3, 7, 7).contour;
    hole.make_clockwise();
    ring.holes.push_back(hole);
    std::vector<std::vector<ExPolygons>> layers(2, std::vector<ExPolygons>(2));
    layers[0][1] = {ring};
    layers[1][1] = {box(4, 4, 6, 6), box(10, 0, 12, 2), box(0, 0, 2, 2)};
    const auto phases = compute_painted_region_phases(layers, {false, true}, [] {});
    CHECK(phases[3] == std::vector<int>{0, 0, 1});
}
