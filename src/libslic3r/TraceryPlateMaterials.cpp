#include "TraceryPlateMaterials.hpp"
#include <stdexcept>

namespace Slic3r {
namespace {
constexpr const char* feature_keys[] = {"wall_filament", "solid_infill_filament", "sparse_infill_filament",
                                      "support_filament", "support_interface_filament"};
void inherit_materials(ModelObject& object, const DynamicPrintConfig& inherited)
{
    for (const char* key : feature_keys) {
        const bool support = std::string(key).find("support") == 0;
        if (!object.config.has(key) && (support || object.config.extruder() == 0))
            if (const auto* value = inherited.option(key); value && value->getInt() > 0)
                object.config.set_key_value(key, value->clone());
    }
    if (object.config.extruder() == 0)
        object.config.set_key_value("extruder", new ConfigOptionInt(1));
}
unsigned int mapped_id(unsigned int id, const EnforcerBlockerStateMap& map)
{
    if (id >= map.size() || (id != 0 && map[id] == EnforcerBlockerType::NONE))
        throw std::runtime_error("Unresolved material F" + std::to_string(id));
    return unsigned(map[id]);
}
void remap_config(ModelConfig& config, const EnforcerBlockerStateMap& mapping)
{
    auto remap = [&](const char* key) {
        if (const auto* value = config.option(key); value && value->getInt() > 0) {
            const unsigned int old_id = unsigned(value->getInt());
            const unsigned int new_id = mapped_id(old_id, mapping);
            if (old_id != new_id)
                config.set_key_value(key, new ConfigOptionInt(int(new_id)));
        }
    };
    remap("extruder");
    for (const char* key : feature_keys)
        remap(key);
}
}

std::vector<unsigned int> tracery_object_materials(const ModelObject& object, const DynamicPrintConfig& inherited)
{
    std::set<unsigned int> result;
    auto collect = [&](const auto& config) {
        for (const char* key : {"extruder", "wall_filament", "solid_infill_filament", "sparse_infill_filament",
                               "support_filament", "support_interface_filament"})
            if (const auto* value = config.option(key); value && value->getInt() > 0)
                result.insert(unsigned(value->getInt()));
    };
    // Match inheritance materialization used by the conversion below.
    collect(object.config);
    if (object.config.extruder() == 0) result.insert(1);
    for (const char* key : feature_keys)
        if (!object.config.has(key) && (std::string(key).find("support") == 0 || object.config.extruder() == 0))
            if (const auto* value = inherited.option(key); value && value->getInt() > 0)
                result.insert(unsigned(value->getInt()));
    for (const auto* volume : object.volumes) {
        if (!volume->is_model_part() && !volume->is_modifier())
            continue;
        collect(volume->config);
        if (volume->is_model_part())
            result.insert(unsigned(std::max(1, volume->extruder_id())));
        const auto& used = volume->mmu_segmentation_facets.get_data().used_states;
        for (size_t id = 1; id < used.size(); ++id)
            if (used[id]) result.insert(unsigned(id));
    }
    for (const auto& range : object.layer_config_ranges)
        collect(range.second);
    return {result.begin(), result.end()};
}

EnforcerBlockerStateMap tracery_identity_material_map()
{
    EnforcerBlockerStateMap map;
    for (size_t i = 0; i < map.size(); ++i) map[i] = EnforcerBlockerType(i);
    return map;
}

EnforcerBlockerStateMap tracery_palette_reindex(const MixedFilamentManager& before, size_t old_physical,
                                               const MixedFilamentManager& after, size_t new_physical)
{
    auto map = tracery_identity_material_map();
    std::map<uint64_t, unsigned int> new_ids;
    unsigned int id = unsigned(new_physical);
    for (const auto& row : after.mixed_filaments())
        if (row.enabled && !row.deleted) new_ids.emplace(row.stable_id, ++id);
    id = unsigned(old_physical);
    for (const auto& row : before.mixed_filaments()) {
        if (!row.enabled || row.deleted) continue;
        ++id;
        const auto found = new_ids.find(row.stable_id);
        if (id >= map.size() || found == new_ids.end() || found->second >= map.size())
            throw std::runtime_error("Cannot preserve an existing mixed material while adding glass materials");
        map[id] = EnforcerBlockerType(found->second);
    }
    return map;
}

TriangleSelector::TriangleSplittingData tracery_remap_paint(const TriangleMesh& mesh,
    const TriangleSelector::TriangleSplittingData& paint, const EnforcerBlockerStateMap& mapping)
{
    bool changed = false;
    for (size_t id = 1; id < paint.used_states.size(); ++id)
        if (paint.used_states[id]) changed |= mapped_id(unsigned(id), mapping) != id;
    if (!changed) return paint;
    TriangleSelector selector(mesh);
    selector.deserialize(paint, false);
    selector.remap_triangle_state(mapping);
    return selector.serialize();
}

TriangleSelector::Tracery2DData tracery_preserve_black(const TriangleMesh& mesh,
    const TriangleSelector::TriangleSplittingData& paint, TriangleSelector::Tracery2DData regions,
    unsigned int black_id, unsigned int default_material)
{
    if (black_id == 0) return regions;
    TriangleSelector selector(mesh);
    selector.deserialize(paint, false);
    for (const auto& leaf : selector.painted_leaves(default_material == black_id)) {
        if (unsigned(leaf.state) != black_id && !(leaf.state == EnforcerBlockerType::NONE && default_material == black_id))
            continue;
        const Vec3d a = leaf.vertices[0].cast<double>();
        const Vec3d b = leaf.vertices[1].cast<double>() - a;
        const Vec3d c = leaf.vertices[2].cast<double>() - a;
        if (b.cross(c).squaredNorm() <= 1e-24) continue;
        TriangleSelector::TraceryFacet2DData facet;
        facet.source_facet_id = leaf.source_facet;
        facet.origin = a;
        facet.axis_u = b.normalized();
        facet.axis_v = b.cross(c).normalized().cross(facet.axis_u);
        TriangleSelector::TraceryRegion2D region;
        region.color = leaf.state;
        region.polygon = {Vec2d::Zero(), Vec2d(b.norm(), 0.), Vec2d(c.dot(facet.axis_u), c.dot(facet.axis_v))};
        facet.regions.push_back(std::move(region));
        regions.push_back(std::move(facet)); // priority over generated regions
    }
    return regions;
}

void tracery_remap_object_materials(ModelObject& object, const EnforcerBlockerStateMap& mapping,
                                    const DynamicPrintConfig* inherited)
{
    if (inherited) inherit_materials(object, *inherited);
    remap_config(object.config, mapping);
    for (auto* volume : object.volumes) {
        if (!volume->is_model_part() && !volume->is_modifier()) continue;
        remap_config(volume->config, mapping);
        const auto paint = tracery_remap_paint(volume->mesh(), volume->mmu_segmentation_facets.get_data(), mapping);
        if (paint != volume->mmu_segmentation_facets.get_data()) {
            TriangleSelector selector(volume->mesh());
            selector.deserialize(paint, false);
            volume->mmu_segmentation_facets.set(selector);
        }
    }
    for (auto& range : object.layer_config_ranges) remap_config(range.second, mapping);
}

TraceryPlateConversion prepare_tracery_plate_conversion(const Model& source, const TraceryPlateInstances& instances,
    const DynamicPrintConfig& inherited, const EnforcerBlockerStateMap& reindex,
    const EnforcerBlockerStateMap& conversion, int plate_index)
{
    TraceryPlateConversion result;
    result.model = std::make_unique<Model>(source);
    for (const auto& [index, selected] : instances)
        result.target_objects.emplace(index, result.model->isolate_instances(index, selected));
    std::set<size_t> targets;
    for (const auto& [original, target] : result.target_objects) targets.insert(target);
    for (size_t i = 0; i < result.model->objects.size(); ++i)
        tracery_remap_object_materials(*result.model->objects[i], targets.count(i) ? conversion : reindex,
                                       targets.count(i) ? &inherited : nullptr);
    for (auto& [plate, codes] : result.model->plates_custom_gcodes)
        for (auto& item : codes.gcodes)
            if (item.type == CustomGCode::Type::ToolChange && item.extruder > 0)
                item.extruder = int(mapped_id(unsigned(item.extruder), plate == plate_index ? conversion : reindex));
    return result;
}
std::shared_ptr<const TraceryPaletteSnapshot> capture_tracery_palette(const DynamicPrintConfig& config,
    const std::vector<std::string>& presets, const MixedFilamentManager& manager, const DynamicPrintConfig* print_config)
{
    auto state = std::make_shared<TraceryPaletteSnapshot>();
    for (const auto& key : config.keys())
        if (key.find("filament_") == 0 || key.find("mixed_filament_") == 0 || key.find("flush_volumes_") == 0 ||
            key == "tracery_black_material_mode")
            state->config.set_key_value(key, config.option(key)->clone());
    if (print_config)
        for (const char* key : feature_keys)
            if (const auto* value = print_config->option(key))
                state->print_materials.set_key_value(key, value->clone());
    state->presets = presets;
    state->manager = manager;
    return state;
}
} // namespace Slic3r
