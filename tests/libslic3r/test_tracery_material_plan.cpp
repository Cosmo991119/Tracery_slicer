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
