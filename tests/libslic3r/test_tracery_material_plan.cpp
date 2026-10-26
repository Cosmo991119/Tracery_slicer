#include <catch2/catch_test_macros.hpp>
#include "libslic3r/TraceryMaterialPlan.hpp"

using namespace Slic3r;

TEST_CASE("Black listed in the palette does not change glass materials until the model uses it", "[TraceryMaterials]")
{
    const std::vector<bool> slots{false, false, false, false, true};
    const auto unused = tracery_used_black_materials(slots, {1, 2, 3, 4});
    REQUIRE(unused.empty());
    REQUIRE(tracery_material_mode(!unused.empty(), false) == TraceryMaterialMode::CMYW);
    const auto painted = tracery_used_black_materials(slots, {1, 5, 5});
    REQUIRE(painted == std::vector<unsigned int>{5});
    REQUIRE(tracery_material_mode(!painted.empty(), false) == TraceryMaterialMode::CMY);
    REQUIRE(tracery_material_mode(!painted.empty(), true) == TraceryMaterialMode::CMYK);
}

TEST_CASE("Opaque black occupies a head but never enters a glass recipe", "[TraceryMaterials]")
{
    const auto mode = tracery_material_mode(true, false);
    REQUIRE(mode == TraceryMaterialMode::CMY);
    const auto weights = match_tracery_weights(mode, [](const auto& w) { return double(100 - w[3]); });
    REQUIRE(weights[3] == 0);
    REQUIRE(weights[0] + weights[1] + weights[2] == 100);
}

TEST_CASE("The palette follows actual black usage and its optical role", "[TraceryMaterials]")
{
    REQUIRE(tracery_material_mode(false, false) == TraceryMaterialMode::CMYW);
    REQUIRE(tracery_material_mode(false, true) == TraceryMaterialMode::CMYW);
    REQUIRE(tracery_material_mode(true, true) == TraceryMaterialMode::CMYK);
    REQUIRE(legal_tracery_weights({35, 30, 35, 0}, TraceryMaterialMode::CMY));
    REQUIRE_FALSE(legal_tracery_weights({35, 30, 35, 0}, TraceryMaterialMode::CMYW));
    REQUIRE(legal_tracery_weights({25, 25, 25, 25}, TraceryMaterialMode::CMYK));
    REQUIRE_FALSE(legal_tracery_weights({25, 25, 25, 25}, TraceryMaterialMode::CMYW));
    const auto black = match_tracery_weights(TraceryMaterialMode::CMYK, [](const auto& w) { return double(100 - w[3]); });
    REQUIRE(black == std::array<int, 4>{0, 0, 0, 100});
}

TEST_CASE("Similar-looking cached recipes cannot reintroduce forbidden materials", "[TraceryMaterials]")
{
    REQUIRE(tracery_recipe_uses_only({2, 4}, {2, 4, 7}));
    REQUIRE_FALSE(tracery_recipe_uses_only({2, 9}, {2, 4, 7}));
    REQUIRE_FALSE(tracery_recipe_uses_only({}, {2, 4, 7}));
}

TEST_CASE("Unused palette slots do not cost heads but used white and black conflict with CMY", "[TraceryMaterials]")
{
    REQUIRE(tracery_fits_four_heads({9}, {2, 4, 7}));
    REQUIRE(tracery_fits_four_heads({2, 4, 7, 9}, {2, 4, 7, 9}));
    REQUIRE_FALSE(tracery_fits_four_heads({8, 9}, {2, 4, 7}));
    REQUIRE_FALSE(tracery_fits_four_heads({0}, {2, 4, 7}));
}

TEST_CASE("Glass reuses the closest final preview regardless of recipe percentages", "[TraceryMaterials]")
{
    // Existing and newly planned slots are searched together. A closer planned
    // slot wins over the first existing slot inside the reuse tolerance.
    CHECK(tracery_preview_slot({1.8, 0.3, 8.0}, true) == 1);
    CHECK(tracery_preview_slot({0.0, 0.0}, true) == 0);
    CHECK(tracery_preview_slot({3.0, 8.0}, true) == 2); // create a new colour
}

TEST_CASE("A full glass palette falls back to the nearest eligible colour", "[TraceryMaterials]")
{
    CHECK(tracery_preview_slot({8.0, 3.0, 12.0}, false) == 1);
    CHECK(tracery_preview_slot({}, false) == 0);
}

TEST_CASE("Glass prefers a pair when its color error is within half a Delta E", "[TraceryMaterials]")
{
    const std::array<int, 4> pair{50, 50, 0, 0};
    const std::array<int, 4> triple{40, 40, 0, 20};
    for (double pair_error : {1.4, 1.5, 1.5001}) {
        const auto selected = match_tracery_weights(TraceryMaterialMode::CMYW, [&](const auto& w) {
            return w == pair ? pair_error : w == triple ? 1.0 : 100.0;
        });
        CHECK(selected == (pair_error <= 1.5 ? pair : triple));
    }
}

TEST_CASE("Glass reuses pairs within the preference margin without widening the reuse limit", "[TraceryMaterials]")
{
    CHECK(tracery_preview_slot({1.0, 1.4}, true, {3, 2}) == 1);
    CHECK(tracery_preview_slot({1.0, 1.5}, true, {3, 2}) == 1);
    CHECK(tracery_preview_slot({1.0, 1.5001}, true, {3, 2}) == 0);
    CHECK(tracery_preview_slot({0.0, 0.0}, true, {3, 2}) == 1);
    CHECK(tracery_preview_slot({0.0, 0.0, 0.0}, true, {1, 3, 2}) == 0);
    CHECK(tracery_preview_slot({1.8, 2.1}, true, {3, 2}) == 0);
    CHECK(tracery_preview_slot({1.5, 2.0}, true, {3, 2}) == 1);
    CHECK(tracery_preview_slot({0.0, 0.1, 0.1}, true, {3, 2, 2}) == 1);
}

TEST_CASE("A newly selected glass pair survives reuse unless the palette is full", "[TraceryMaterials]")
{
    CHECK(tracery_preview_slot({0.0}, true, {3}, 2) == 1); // create the pair
    CHECK(tracery_preview_slot({0.0, 0.3}, true, {3, 2}, 2) == 1); // reuse a pair
    CHECK(tracery_preview_slot({0.0, 0.8}, true, {3, 2}, 2) == 2); // create a closer pair
    CHECK(tracery_preview_slot({0.0}, false, {3}, 2) == 0); // capacity fallback
    CHECK(tracery_preview_slot({3.0, 3.5}, false, {3, 2}, 2) == 1);
    CHECK(tracery_preview_slot({3.0, 3.6}, false, {3, 2}, 2) == 0);
    CHECK(tracery_preview_slot({0.0}, true, {1}, 2) == 0); // retain a pure color
    CHECK(tracery_preview_slot({}, false, {}, 2) == 0);
}

TEST_CASE("Glass searches pairs at one-percent resolution in every material mode", "[TraceryMaterials]")
{
    const std::array<int, 4> pair{37, 63, 0, 0};
    for (auto mode : {TraceryMaterialMode::CMYW, TraceryMaterialMode::CMY, TraceryMaterialMode::CMYK}) {
        const std::array<int, 4> multi = mode == TraceryMaterialMode::CMYW ? std::array<int, 4>{30, 35, 0, 35} :
                                       mode == TraceryMaterialMode::CMY ? std::array<int, 4>{30, 35, 35, 0} :
                                                                        std::array<int, 4>{25, 25, 25, 25};
        const auto selected = match_tracery_weights(mode, [&](const auto& w) {
            CHECK(legal_tracery_weights(w, mode));
            if (w == multi) return 1.0;
            return w[2] == 0 && w[3] == 0 ? 1.4 + 0.1 * std::abs(w[0] - 37) : 100.0;
        });
        CHECK(selected == pair);
    }
}

TEST_CASE("Glass pair preference preserves pure colors and ingredient constraints", "[TraceryMaterials]")
{
    const std::array<int, 4> pure{100, 0, 0, 0}, pair{50, 50, 0, 0}, triple{30, 35, 0, 35};
    const auto selected = match_tracery_weights(TraceryMaterialMode::CMYW, [&](const auto& w) {
        return w == pure ? 0.2 : w == pair ? 0.3 : w == triple ? 0.4 : 100.0;
    });
    CHECK(selected == pure);
    CHECK_FALSE(legal_tracery_weights({29, 71, 0, 0}, TraceryMaterialMode::CMYW));
    CHECK_FALSE(legal_tracery_weights({43, 43, 0, 14}, TraceryMaterialMode::CMYW));
    CHECK(legal_tracery_weights({30, 70, 0, 0}, TraceryMaterialMode::CMYW));
    CHECK(legal_tracery_weights({40, 45, 0, 15}, TraceryMaterialMode::CMYW));
}

TEST_CASE("Invalid glass color scores cannot win pair preference", "[TraceryMaterials]")
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    CHECK(tracery_preview_slot({nan, inf, -1.0, 0.2}, true, {2, 2, 2, 3}) == 3);
    const std::array<int, 4> triple{30, 35, 0, 35};
    CHECK(match_tracery_weights(TraceryMaterialMode::CMYW, [&](const auto& w) { return w == triple ? 1.0 : nan; }) == triple);
    CHECK(match_tracery_weights(TraceryMaterialMode::CMYW, [&](const auto&) { return inf; }) == std::array<int, 4>{});
}
