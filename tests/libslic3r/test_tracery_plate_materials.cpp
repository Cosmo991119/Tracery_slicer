#include <catch2/catch_test_macros.hpp>
#include "libslic3r/Model.hpp"
#include "libslic3r/TraceryPlateMaterials.hpp"
#include "libslic3r/TraceryPartition.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include <boost/filesystem.hpp>

using namespace Slic3r;

TEST_CASE("Isolating a plate preserves the other plate and instance identities", "[TraceryPlate]")
{
    Model model;
    auto* object = model.add_object("shared", "", TriangleMesh(its_make_cube(10., 10., 1.)));
    object->add_instance()->set_offset(Vec3d(20, 20, 0));
    object->add_instance()->set_offset(Vec3d(300, 20, 0));
    const auto outside_id = object->instances[1]->id();
    const auto inside_id = object->instances[0]->id();
    const auto object_id = object->id();
    const size_t target = model.isolate_instances(0, {0});
    REQUIRE(target != 0);
    REQUIRE(model.objects[0]->id() == object_id);
    REQUIRE(model.objects[0]->instances.size() == 1);
    CHECK(model.objects[0]->instances[0]->id() == outside_id);
    CHECK(model.objects[target]->instances[0]->id() == inside_id);
    model.objects[target]->config.set_key_value("extruder", new ConfigOptionInt(6));
    CHECK(model.objects[0]->config.extruder() != 6);
    CHECK(model.objects[0]->volumes[0]->mesh().its.indices == model.objects[target]->volumes[0]->mesh().its.indices);
}


TEST_CASE("Whole-plate conversion remaps ordinary parts and shared paint without touching another plate", "[TraceryPlate]")
{
    Model model;
    auto* shared = model.add_object("shared", "", TriangleMesh(its_make_cube(10., 10., 1.)));
    shared->add_instance();
    shared->add_instance()->set_offset(Vec3d(300, 0, 0));
    shared->config.set_key_value("extruder", new ConfigOptionInt(2));
    TriangleSelector paint(shared->volumes[0]->mesh());
    paint.set_facet(0, EnforcerBlockerType(4)); // black must survive
    paint.set_facet(1, EnforcerBlockerType(2));
    shared->volumes[0]->mmu_segmentation_facets.set(paint);
    const auto original_paint = shared->volumes[0]->mmu_segmentation_facets.get_data();
    auto* ordinary = model.add_object("ordinary", "", TriangleMesh(its_make_cube(5., 5., 1.)));
    ordinary->add_instance();
    ordinary->config.set_key_value("extruder", new ConfigOptionInt(3));
    ordinary->volumes[0]->config.set_key_value("solid_infill_filament", new ConfigOptionInt(2));
    auto mapping = tracery_identity_material_map();
    mapping[2] = EnforcerBlockerType(8);
    mapping[3] = EnforcerBlockerType(9);
    auto plan = prepare_tracery_plate_conversion(model, {{0, {0}}, {1, {0}}}, DynamicPrintConfig{},
                                                 tracery_identity_material_map(), mapping, 0);
    const auto* outside = plan.model->objects[0];
    CHECK(outside->config.extruder() == 2);
    CHECK(outside->volumes[0]->mmu_segmentation_facets.get_data() == original_paint);
    const auto* inside = plan.model->objects[plan.target_objects.at(0)];
    CHECK(inside->config.extruder() == 8);
    CHECK(inside->volumes[0]->mmu_segmentation_facets.get_data().used_states[4]);
    CHECK(inside->volumes[0]->mmu_segmentation_facets.get_data().used_states[8]);
    CHECK_FALSE(inside->volumes[0]->mmu_segmentation_facets.get_data().used_states[2]);
    CHECK(plan.model->objects[1]->config.extruder() == 9);
    CHECK(plan.model->objects[1]->volumes[0]->config.opt_int("solid_infill_filament") == 8);
    CHECK(plan.model->objects[1]->volumes[0]->mesh().its.indices == ordinary->volumes[0]->mesh().its.indices);
    // Preparing a plan never mutates the live model; failed planning is safe.
    CHECK(model.objects.size() == 2);
    CHECK(shared->instances.size() == 2);
    CHECK(shared->config.extruder() == 2);

    // Commit replaces objects, not project ownership or backup metadata.
    model.set_object_backup_id(*shared, 87);
    model.wipe_tower.positions = {Vec2d(30, 40)};
    model.swap_objects(*plan.model);
    CHECK(model.get_object_backup_id(*model.objects[0]) == 87);
    CHECK(model.wipe_tower.positions == std::vector<Vec2d>{Vec2d(30, 40)});
    CHECK(model.objects[0]->get_model() == &model);
    CHECK(plan.model->objects[0]->get_model() == plan.model.get());
}

TEST_CASE("Appending physical bases reindexes shared virtual IDs without changing outside recipes", "[TraceryPlate]")
{
    const std::vector<std::string> colours{"#00FFFF", "#FF00FF", "#FFFF00", "#000000"};
    MixedFilamentManager before;
    before.auto_generate(colours);
    MixedFilamentBatchEntry row;
    row.component_a = 1;
    row.component_b = 2;
    row.mix_b_percent = 50;
    row.display_color = "#6060DD";
    std::vector<unsigned int> old_ids;
    before.add_batch_custom_filaments({row}, colours, &old_ids);
    REQUIRE(old_ids.size() == 1);
    const auto original = *before.mixed_filament_from_id(old_ids[0], colours.size());
    auto expanded = colours;
    expanded.push_back("#FFFFFF");
    auto after = before;
    after.auto_generate(expanded);
    const auto reindex = tracery_palette_reindex(before, colours.size(), after, expanded.size());
    const auto* preserved = after.mixed_filament_from_id(unsigned(reindex[old_ids[0]]), expanded.size());
    REQUIRE(preserved != nullptr);
    CHECK(preserved->stable_id == original.stable_id);
    CHECK(preserved->component_a == original.component_a);
    CHECK(preserved->component_b == original.component_b);
    CHECK(preserved->mix_b_percent == original.mix_b_percent);

    Model model;
    auto* object = model.add_object("shared recipe", "", TriangleMesh(its_make_cube(5., 5., 1.)));
    object->add_instance();
    object->add_instance()->set_offset(Vec3d(300, 0, 0));
    object->config.set_key_value("extruder", new ConfigOptionInt(int(old_ids[0])));
    auto convert = reindex;
    convert[old_ids[0]] = EnforcerBlockerType(3);
    auto plan = prepare_tracery_plate_conversion(model, {{0, {0}}}, {}, reindex, convert, 0);
    CHECK(plan.model->objects[0]->config.extruder() == int(reindex[old_ids[0]]));
    CHECK(plan.model->objects[plan.target_objects.at(0)]->config.extruder() == 3);
    CHECK(model.objects[0]->config.extruder() == int(old_ids[0]));
}

TEST_CASE("Glass coverage never overwrites original black paint or a black default surface", "[TraceryPlate]")
{
    TriangleMesh mesh(its_make_cube(10., 10., 1.));
    TriangleSelector paint(mesh);
    paint.set_facet(0, EnforcerBlockerType(4));
    const auto& triangle = mesh.its.indices[0];
    const Vec3d a = mesh.its.vertices[triangle[0]].cast<double>();
    const Vec3d b = mesh.its.vertices[triangle[1]].cast<double>() - a;
    const Vec3d c = mesh.its.vertices[triangle[2]].cast<double>() - a;
    TriangleSelector::TraceryFacet2DData facet;
    facet.source_facet_id = 0;
    facet.origin = a;
    facet.axis_u = b.normalized();
    facet.axis_v = b.cross(c).normalized().cross(facet.axis_u);
    TriangleSelector::TraceryRegion2D region;
    region.color = EnforcerBlockerType(8);
    region.polygon = {Vec2d::Zero(), Vec2d(b.norm(), 0), Vec2d(c.dot(facet.axis_u), c.dot(facet.axis_v))};
    facet.regions.push_back(region);
    auto regions = tracery_preserve_black(mesh, paint.serialize(), {facet}, 4, 1);
    auto result = build_exact_tracery_partition(mesh, {paint.serialize()}, regions);
    CHECK(result.annotations[0].used_states[4]);
    CHECK_FALSE(result.annotations[0].used_states[8]);
    TriangleSelector empty(mesh);
    regions = tracery_preserve_black(mesh, empty.serialize(), {facet}, 4, 4);
    result = build_exact_tracery_partition(mesh, {empty.serialize()}, regions);
    CHECK_FALSE(result.annotations[0].used_states[8]);
}

TEST_CASE("Palette snapshots remain independent and failed conversion leaves the source intact", "[TraceryPlate]")
{
    DynamicPrintConfig config;
    config.set_key_value("filament_colour", new ConfigOptionStrings({"#000000", "#FF0000"}));
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString("original"));
    const auto state = capture_tracery_palette(config, {"black", "red"}, {});
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString("converted"));
    CHECK(state->config.opt_string("mixed_filament_definitions") == "original");
    config.apply(state->config);
    CHECK(config.opt_string("mixed_filament_definitions") == "original");
    Model model;
    auto* object = model.add_object("invalid material", "", TriangleMesh(its_make_cube(5., 5., 1.)));
    object->add_instance();
    object->config.set_key_value("extruder", new ConfigOptionInt(2));
    auto convert = tracery_identity_material_map();
    convert[2] = EnforcerBlockerType::NONE;
    REQUIRE_THROWS(prepare_tracery_plate_conversion(model, {{0, {0}}}, {}, tracery_identity_material_map(), convert, 0));
    CHECK(model.objects.size() == 1);
    CHECK(model.objects[0]->config.extruder() == 2);
}

TEST_CASE("Inherited materials become plate-local and converted paint survives 3MF reload", "[TraceryPlate]")
{
    Model source;
    auto* object = source.add_object("shared", "", TriangleMesh(its_make_cube(10., 10., 1.)));
    object->add_instance();
    object->add_instance()->set_offset(Vec3d(300, 0, 0));
    TriangleSelector paint(object->volumes[0]->mesh());
    paint.set_facet(0, EnforcerBlockerType(4));
    paint.set_facet(1, EnforcerBlockerType(2));
    object->volumes[0]->mmu_segmentation_facets.set(paint);
    DynamicPrintConfig inherited;
    inherited.set_key_value("solid_infill_filament", new ConfigOptionInt(2));
    inherited.set_key_value("support_filament", new ConfigOptionInt(3));
    inherited.set_key_value("support_interface_filament", new ConfigOptionInt(0));
    object->layer_config_ranges[{0., 0.5}].set_key_value("wall_filament", new ConfigOptionInt(2));
    auto mapping = tracery_identity_material_map();
    mapping[1] = EnforcerBlockerType(7);
    mapping[2] = EnforcerBlockerType(8);
    mapping[3] = EnforcerBlockerType(9);
    auto plan = prepare_tracery_plate_conversion(source, {{0, {0}}}, inherited,
                                                 tracery_identity_material_map(), mapping, 0);
    const auto* target = plan.model->objects[plan.target_objects.at(0)];
    CHECK(target->config.opt_int("solid_infill_filament") == 8);
    CHECK(target->config.opt_int("support_filament") == 9);
    CHECK_FALSE(target->config.has("support_interface_filament"));
    CHECK(target->layer_config_ranges.begin()->second.opt_int("wall_filament") == 8);
    CHECK_FALSE(plan.model->objects[0]->config.has("support_filament"));
    CHECK(plan.model->objects[0]->layer_config_ranges.begin()->second.opt_int("wall_filament") == 2);
    const auto path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("tracery-plate-%%%%-%%%%.3mf");
    struct Cleanup {
        boost::filesystem::path path;
        ~Cleanup() { boost::filesystem::remove(path); }
    } cleanup{path};
    const std::string file = path.string();
    DynamicPrintConfig config;
    const std::vector<std::string> colours{"#00FFFF", "#FF00FF", "#FFFF00", "#000000", "#FFFFFF", "#FF8000"};
    MixedFilamentManager recipes;
    std::vector<MixedFilamentBatchEntry> rows(3);
    for (size_t i = 0; i < rows.size(); ++i) {
        rows[i].component_a = 1;
        rows[i].component_b = 2;
        rows[i].mix_b_percent = int(30 + i * 20);
    }
    recipes.add_batch_custom_filaments(rows, colours);
    config.set_key_value("filament_colour", new ConfigOptionStrings(colours));
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(recipes.serialize_custom_entries()));
    plan.model->set_backup_path((boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("tracery-export-%%%%-%%%%")).string());
    StoreParams params;
    params.path = file.c_str();
    params.model = plan.model.get();
    params.config = &config;
    REQUIRE(store_bbs_3mf(params));
    Model restored;
    PlateDataPtrs plates;
    std::vector<Preset*> presets;
    bool is_bbs = false;
    Semver version;
    struct ModelCleanup {
        Model& model;
        PlateDataPtrs& plates;
        ~ModelCleanup() {
            model.remove_backup_path_if_exist();
            for (auto* plate : plates) delete plate;
        }
    } model_cleanup{restored, plates};
    restored.set_backup_path((boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("tracery-plate-%%%%-%%%%")).string());
    ConfigSubstitutionContext context{ForwardCompatibilitySubstitutionRule::Disable};
    DynamicPrintConfig loaded_config;
    REQUIRE(load_bbs_3mf(file.c_str(), &loaded_config, &context, &restored, &plates, &presets, &is_bbs, &version,
                         nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
    REQUIRE(loaded_config.has("mixed_filament_definitions"));
    MixedFilamentManager loaded_recipes;
    loaded_recipes.load_custom_entries(loaded_config.opt_string("mixed_filament_definitions"), colours);
    const auto* restored_recipe = loaded_recipes.mixed_filament_from_id(8, colours.size());
    REQUIRE(restored_recipe != nullptr);
    CHECK(restored_recipe->component_a == 1);
    CHECK(restored_recipe->component_b == 2);
    CHECK(restored_recipe->mix_b_percent == 50);
    REQUIRE(restored.objects.size() == 2);
    CHECK(restored.objects[0]->volumes[0]->mmu_segmentation_facets.get_data().used_states[2]);
    CHECK_FALSE(restored.objects[0]->volumes[0]->mmu_segmentation_facets.get_data().used_states[8]);
    CHECK(restored.objects[1]->volumes[0]->mmu_segmentation_facets.get_data().used_states[8]);
    CHECK(restored.objects[1]->volumes[0]->mmu_segmentation_facets.get_data().used_states[4]);
    CHECK(restored.objects[1]->config.extruder() == 7);
    REQUIRE(restored.objects[1]->config.has("support_filament"));
    CHECK(restored.objects[1]->config.opt_int("support_filament") == 9);
}
