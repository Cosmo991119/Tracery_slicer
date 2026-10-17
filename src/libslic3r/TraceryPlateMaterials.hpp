#ifndef slic3r_TraceryPlateMaterials_hpp_
#define slic3r_TraceryPlateMaterials_hpp_

#include "Model.hpp"
#include "MixedFilament.hpp"
#include "TraceryMaterialPlan.hpp"
#include <set>

namespace Slic3r {
class PresetBundle;

bool tracery_filament_is_full_spectrum(const PresetBundle& bundle, unsigned int filament_id);
TraceryMaterialMode tracery_plate_material_mode(const PresetBundle& bundle, unsigned int used_black_id);

using TraceryPlateInstances = std::map<size_t, std::set<size_t>>;

// Raw physical/virtual references, before resolving recipes into ingredients.
std::vector<unsigned int> tracery_object_materials(const ModelObject& object, const DynamicPrintConfig& inherited);
EnforcerBlockerStateMap tracery_identity_material_map();
EnforcerBlockerStateMap tracery_palette_reindex(const MixedFilamentManager& before, size_t old_physical,
                                               const MixedFilamentManager& after, size_t new_physical);
TriangleSelector::TriangleSplittingData tracery_remap_paint(const TriangleMesh& mesh,
    const TriangleSelector::TriangleSplittingData& paint, const EnforcerBlockerStateMap& mapping);
std::vector<unsigned int> tracery_black_materials(const PresetBundle& bundle, const std::vector<unsigned int>& used);
std::set<EnforcerBlockerType> tracery_unselectable_states(const PresetBundle& bundle, unsigned int default_material);
// Only active source masks are filtered. Generated/back-projected paint must
// never use this policy. Source and mask share the material subdivision tree.
void tracery_filter_source_selection(TriangleSelector& selection, const TriangleSelector& source,
                                     const std::set<EnforcerBlockerType>& blocked);
void tracery_remap_object_materials(ModelObject& object, const EnforcerBlockerStateMap& mapping,
                                    const DynamicPrintConfig* inherited = nullptr);

struct TraceryPlateConversion {
    std::unique_ptr<Model> model;
    std::map<size_t, size_t> target_objects;
};
// Works only on a private copy. Other plates receive identity-preserving ID
// reindexing when physical slots were appended, never the colour conversion.
TraceryPlateConversion prepare_tracery_plate_conversion(const Model& source, const TraceryPlateInstances& instances,
    const DynamicPrintConfig& inherited, const EnforcerBlockerStateMap& reindex,
    const EnforcerBlockerStateMap& conversion, int plate_index);

// Palette state travels with undo snapshots because model references alone
// cannot restore virtual colours after physical slots have been appended.
struct TraceryPaletteSnapshot {
    DynamicPrintConfig config;
    DynamicPrintConfig print_materials;
    std::vector<std::string> presets;
    MixedFilamentManager manager;
};
std::shared_ptr<const TraceryPaletteSnapshot> capture_tracery_palette(const DynamicPrintConfig& config,
    const std::vector<std::string>& presets, const MixedFilamentManager& manager,
    const DynamicPrintConfig* print_config = nullptr);
} // namespace Slic3r
#endif
