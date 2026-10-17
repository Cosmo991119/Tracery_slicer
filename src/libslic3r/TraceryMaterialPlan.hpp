#ifndef slic3r_TraceryMaterialPlan_hpp_
#define slic3r_TraceryMaterialPlan_hpp_

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <vector>

namespace Slic3r {

enum class TraceryMaterialMode { CMYW, CMY, CMYK };

constexpr double TRACERY_VISUAL_REUSE_DELTA_E = 2.0;

// Distances must use final preview colours from eligible existing/pending slots.
// Return distances.size() if none qualify; when full, use the closest valid slot.
// Stable ties preserve existing references across repeated applications.
inline size_t tracery_preview_slot(const std::vector<double>& distances, bool can_add)
{
    size_t best = distances.size();
    double nearest = can_add ? TRACERY_VISUAL_REUSE_DELTA_E : std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < distances.size(); ++i) {
        if (std::isfinite(distances[i]) && distances[i] >= 0.0 && distances[i] <= nearest &&
            (best == distances.size() || distances[i] < nearest)) {
            best = i;
            nearest = distances[i];
        }
    }
    return best;
}

// Physical slots are one-based. Merely listing a black filament must never
// select CMY/CMYK: only references from the current plate count.
inline std::vector<unsigned int> tracery_used_black_materials(const std::vector<bool>& black_slots,
                                                            const std::vector<unsigned int>& used)
{
    std::set<unsigned int> result;
    for (unsigned int id : used)
        if (id > 0 && id <= black_slots.size() && black_slots[id - 1])
            result.insert(id);
    return {result.begin(), result.end()};
}

inline TraceryMaterialMode tracery_material_mode(bool uses_black, bool black_can_mix)
{
    return !uses_black ? TraceryMaterialMode::CMYW :
           black_can_mix ? TraceryMaterialMode::CMYK : TraceryMaterialMode::CMY;
}

inline bool legal_tracery_weights(const std::array<int, 4>& weights, TraceryMaterialMode mode)
{
    int total = 0, active = 0, chromatic = 0;
    for (size_t i = 0; i < weights.size(); ++i) {
        if (weights[i] < 0 || weights[i] > 100)
            return false;
        total += weights[i];
        if (weights[i] > 0) {
            ++active;
            chromatic += i < 3;
        }
    }
    if (total != 100 || (mode == TraceryMaterialMode::CMY && weights[3] != 0))
        return false;
    // Preserve the established CMYW glass palette. CMY and CMYK must not
    // inherit its requirement that a third ingredient always be white.
    if (mode == TraceryMaterialMode::CMYW && (active > 3 || chromatic > 2))
        return false;
    const int minimum = active == 2 ? 30 : 15;
    for (int weight : weights)
        if (weight > 0 && weight < minimum)
            return false;
    return true;
}

// Score is supplied by the colour model; material eligibility is enforced
// before evaluation, including during the one-percent refinement pass.
template<class Score>
std::array<int, 4> match_tracery_weights(TraceryMaterialMode mode, Score score)
{
    std::array<int, 4> best{};
    double best_score = std::numeric_limits<double>::infinity();
    auto consider = [&](const std::array<int, 4>& weights) {
        if (!legal_tracery_weights(weights, mode))
            return;
        const double value = score(weights);
        if (value < best_score) {
            best_score = value;
            best = weights;
        }
    };
    for (int c = 0; c <= 100; c += 5)
        for (int m = 0; m <= 100 - c; m += 5)
            for (int y = 0; y <= 100 - c - m; y += 5)
                consider({c, m, y, 100 - c - m - y});
    const auto centre = best;
    for (int c = std::max(0, centre[0] - 4); c <= std::min(100, centre[0] + 4); ++c)
        for (int m = std::max(0, centre[1] - 4); m <= std::min(100 - c, centre[1] + 4); ++m)
            for (int y = std::max(0, centre[2] - 4); y <= std::min(100 - c - m, centre[2] + 4); ++y)
                consider({c, m, y, 100 - c - m - y});
    return best;
}

inline bool tracery_recipe_uses_only(const std::vector<unsigned int>& ingredients, const std::vector<unsigned int>& allowed)
{
    return !ingredients.empty() && std::all_of(ingredients.begin(), ingredients.end(), [&](unsigned int id) {
        return std::find(allowed.begin(), allowed.end(), id) != allowed.end();
    });
}

inline bool tracery_fits_four_heads(const std::vector<unsigned int>& used, const std::vector<unsigned int>& required)
{
    std::set<unsigned int> physical(used.begin(), used.end());
    physical.insert(required.begin(), required.end());
    return !physical.count(0) && physical.size() <= 4;
}

} // namespace Slic3r
#endif
