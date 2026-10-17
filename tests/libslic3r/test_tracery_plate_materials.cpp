#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/TraceryPlateMaterials.hpp"
#include "libslic3r/TraceryPartition.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include <boost/filesystem.hpp>

using namespace Slic3r;

TEST_CASE("Opaque source materials cannot enter a flower fill selection", "[TraceryPlate]")
{
    const TriangleMesh mesh(its_make_cube(10., 10., 1.));
    TriangleSelector material(mesh), selection(mesh);
    const auto source = GENERATE(EnforcerBlockerType::NONE, EnforcerBlockerType(4));
    material.set_facet(0, source);
    material.set_unselectable_states({source});
    const auto& face = mesh.its.indices[0];
    const Vec3f hit = (mesh.its.vertices[face[0]] + mesh.its.vertices[face[1]] + mesh.its.vertices[face[2]]) / 3.f;
    const bool propagate = GENERATE(false, true);
    material.bucket_fill_select_triangles(hit, 0, {}, -1.f, propagate, true);
    CHECK(material.get_seed_fill_contour().empty());
    material.apply_seed_fill_to(selection, EnforcerBlockerType::ENFORCER);
    CHECK_FALSE(selection.has_facets(EnforcerBlockerType::ENFORCER));
    // Changing to an eligible material restores normal selection.
    material.set_unselectable_states({});
    material.bucket_fill_select_triangles(hit, 0, {}, -1.f, propagate, true);
    material.apply_seed_fill_to(selection, EnforcerBlockerType::ENFORCER);
    CHECK(selection.has_facets(EnforcerBlockerType::ENFORCER));
}

TEST_CASE("Brush and smart fill keep opaque subregions out of the source selection", "[TraceryPlate]")
{
    const TriangleMesh mesh(its_make_cube(10., 10., 1.));
    TriangleSelector material(mesh);
    material.paint_generated(mesh.bounding_box(), 2.f, [](const Vec3f& point) {
        return point.x() < 5.f ? EnforcerBlockerType(4) : EnforcerBlockerType(2);
    });
    const auto before = material.painted_leaves();
    material.set_unselectable_states({EnforcerBlockerType(4)});
    const bool brush = GENERATE(false, true);
    if (brush) {
        auto cursor = TriangleSelector::SinglePointCursor::cursor_factory(Vec3f(5, 5, .5), Vec3f(5, 5, 30), 30.f,
            TriangleSelector::CursorType::SPHERE, Transform3d::Identity(), {});
        material.select_patch(0, std::move(cursor), EnforcerBlockerType::ExtruderMax, Transform3d::Identity(), true, 0.f);
    } else {
        const auto& face = mesh.its.indices[0];
        const Vec3f hit = (mesh.its.vertices[face[0]] + mesh.its.vertices[face[1]] + mesh.its.vertices[face[2]]) / 3.f;
        material.seed_fill_select_triangles(hit, 0, Transform3d::Identity(), {}, 180.f, 0.f, true);
        material.seed_fill_apply_on_triangles(EnforcerBlockerType::ExtruderMax);
    }
    size_t selected = 0;
    for (const auto& leaf : before) {
        const Vec3f point = (leaf.vertices[0] + leaf.vertices[1] + leaf.vertices[2]) / 3.f;
        const auto after = material.state_at(point, leaf.source_facet);
        if (leaf.state == EnforcerBlockerType(4))
            CHECK(after == EnforcerBlockerType(4));
        else if (after == EnforcerBlockerType::ExtruderMax)
            ++selected;
    }
    CHECK(selected > 0);
}

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

TEST_CASE("Restored source masks exclude opaque black but keep translucent black", "[TraceryPlate]")
{
    PresetBundle bundle;
    bundle.filaments.load_preset("", "Opaque PLA", DynamicPrintConfig{}, false);
    bundle.filaments.load_preset("", "PLA Full Spectrum", DynamicPrintConfig{}, false);
    DynamicPrintConfig derived;
    Preset::inherits(derived) = "PLA Full Spectrum";
    bundle.filaments.load_preset("", "Custom transparent black", derived, false);
    bundle.filament_presets = {"Opaque PLA", "Custom transparent black", "Opaque PLA"};
    bundle.project_config.set_key_value("filament_colour", new ConfigOptionStrings({"#000000", "#000000", "#FFFFFF"}));
    const bool default_black = GENERATE(false, true);
    const bool translucent = GENERATE(false, true);
    const unsigned int black_id = translucent ? 2 : 1;
    const TriangleMesh mesh(its_make_cube(10., 10., 1.));
    TriangleSelector source(mesh), selection(mesh);
    source.set_facet(0, default_black ? EnforcerBlockerType::NONE : EnforcerBlockerType(black_id));
    source.set_facet(1, EnforcerBlockerType(3));
    const auto original_paint = source.serialize();
    selection.set_facet(0, EnforcerBlockerType::ENFORCER);
    selection.set_facet(1, EnforcerBlockerType::ENFORCER);
    const auto blocked = tracery_unselectable_states(bundle, default_black ? black_id : 3);
    tracery_filter_source_selection(selection, source, blocked);
    for (int facet : {0, 1}) {
        const auto& face = mesh.its.indices[facet];
        const Vec3f point = (mesh.its.vertices[face[0]] + mesh.its.vertices[face[1]] + mesh.its.vertices[face[2]]) / 3.f;
        CHECK(selection.state_at(point, facet) == ((facet == 1 || translucent) ?
              EnforcerBlockerType::ENFORCER : EnforcerBlockerType::NONE));
    }
    CHECK(source.serialize() == original_paint); // Filtering never repaints materials.
    if (translucent) {
        bundle.filament_presets[1] = "Opaque PLA";
        tracery_filter_source_selection(selection, source,
            tracery_unselectable_states(bundle, default_black ? black_id : 3));
        const auto& face = mesh.its.indices[0];
        const Vec3f point = (mesh.its.vertices[face[0]] + mesh.its.vertices[face[1]] + mesh.its.vertices[face[2]]) / 3.f;
        CHECK(selection.state_at(point, 0) == EnforcerBlockerType::NONE);
        CHECK(source.serialize() == original_paint);
    }
}

TEST_CASE("Through projections ignore target colour and stay inside their footprint", "[TraceryPlate]")
{
    TriangleMesh mesh(its_make_cube(10., 10., 1.));
    TriangleSelector paint(mesh);
    const auto original_state = GENERATE(EnforcerBlockerType::NONE, EnforcerBlockerType(4), EnforcerBlockerType(2));
    paint.set_facet(0, original_state);
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
    region.polygon = {Vec2d::Zero(), Vec2d(b.norm() * .5, 0.),
                      Vec2d(c.dot(facet.axis_u) * .5, c.dot(facet.axis_v) * .5)};
    facet.regions.push_back(region);
    // Destination colour is deliberately not a selection/generation filter.
    const auto result = build_exact_tracery_partition(mesh, {paint.serialize()}, {facet});
    const TriangleMesh partitioned_mesh(result.mesh);
    TriangleSelector actual(partitioned_mesh);
    actual.deserialize(result.annotations[0], false);
    double glass_area = 0.;
    double retained_area = 0.;
    for (const auto& leaf : actual.painted_leaves(true)) {
        if (result.source_facets.at(leaf.source_facet) != 0)
            continue;
        const double area = .5 * (leaf.vertices[1] - leaf.vertices[0]).cross(leaf.vertices[2] - leaf.vertices[0]).norm();
        if (leaf.state == EnforcerBlockerType(8)) glass_area += area;
        else if (leaf.state == original_state) retained_area += area;
    }
    const double source_area = .5 * b.cross(c).norm();
    CHECK(std::abs(glass_area - source_area * .25) < 1e-4);
    CHECK(std::abs(retained_area - source_area * .75) < 1e-4);
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

TEST_CASE("Legacy manual black settings cannot override the material used by the plate", "[TraceryPlate]")
{
    PresetBundle bundle;
    bundle.filaments.load_preset("", "Opaque PLA", DynamicPrintConfig{}, false);
    bundle.filaments.load_preset("", "Snapmaker PLA Full Spectrum", DynamicPrintConfig{}, false);
    bundle.filament_presets = {"Opaque PLA", "Snapmaker PLA Full Spectrum"};
    for (int legacy : {0, 1, 2}) {
        bundle.project_config.set_deserialize_strict("tracery_black_material_mode", std::to_string(legacy));
        CHECK(tracery_plate_material_mode(bundle, 1) == TraceryMaterialMode::CMY);
        CHECK(tracery_plate_material_mode(bundle, 2) == TraceryMaterialMode::CMYK);
        CHECK(tracery_plate_material_mode(bundle, 0) == TraceryMaterialMode::CMYW);
    }
}

TEST_CASE("Black material category follows preset ancestry and slot changes", "[TraceryPlate]")
{
    PresetBundle bundle;
    auto add = [&](const std::string& name, const std::string& parent) {
        DynamicPrintConfig config;
        Preset::inherits(config) = parent;
        bundle.filaments.load_preset("", name, config, false);
    };
    add("Snapmaker PLA Full Spectrum", "");
    add("Derived glass material", "Snapmaker PLA Full Spectrum");
    add("My black material", "Derived glass material");
    add("Opaque PLA", "");
    add("Cycle A", "Cycle B");
    add("Cycle B", "Cycle A");
    bundle.filament_presets = {"My black material", "Missing preset", "Cycle A", "Opaque PLA", "Missing Full Spectrum"};
    CHECK(tracery_plate_material_mode(bundle, 1) == TraceryMaterialMode::CMYK);
    CHECK(tracery_plate_material_mode(bundle, 2) == TraceryMaterialMode::CMY);
    CHECK(tracery_plate_material_mode(bundle, 3) == TraceryMaterialMode::CMY);
    CHECK(tracery_plate_material_mode(bundle, 4) == TraceryMaterialMode::CMY);
    CHECK(tracery_plate_material_mode(bundle, 5) == TraceryMaterialMode::CMY);
    CHECK(tracery_plate_material_mode(bundle, 0) == TraceryMaterialMode::CMYW);
    CHECK_FALSE(tracery_filament_is_full_spectrum(bundle, 6));
    bundle.filament_presets[0] = "Opaque PLA";
    CHECK(tracery_plate_material_mode(bundle, 1) == TraceryMaterialMode::CMY);
    bundle.filament_presets[0] = "Snapmaker PLA Full Spectrum";
    CHECK(tracery_plate_material_mode(bundle, 1) == TraceryMaterialMode::CMYK);
}

TEST_CASE("Different glass ratios reuse the colour actually displayed after layer allocation", "[TraceryPlate]")
{
    MixedFilamentManager manager;
    MixedFilamentDisplayContext context;
    context.preview_settings.local_z_mode = true;
    context.preview_settings.preferred_a_height = 0.1;
    context.preview_settings.preferred_b_height = 0.1;
    manager.set_display_context(context);
    const std::vector<std::string> colours{"#FF00FF", "#FFFF00"};
    MixedFilamentBatchEntry first;
    first.component_a = 1;
    first.component_b = 2;
    first.mix_b_percent = 45;
    first.distribution_mode = int(MixedFilament::LayerCycle);
    first.gradient_component_ids = "1/2";
    first.gradient_component_weights = "55/45";
    first.display_color = "#123456"; // Nominal prediction must not determine reuse.
    auto second = first;
    second.mix_b_percent = 53;
    second.gradient_component_weights = "47/53";
    second.display_color = "#654321";
    const auto final_first = manager.preview_custom_filament(first, colours);
    const auto final_second = manager.preview_custom_filament(second, colours);
    CHECK(final_first == final_second);
    CHECK(manager.mixed_filaments().empty()); // Planning cannot change shared slots.
    manager.add_batch_custom_filaments({first, second}, colours);
    REQUIRE(manager.mixed_filaments().size() == 2);
    CHECK(final_first == manager.mixed_filaments()[0].display_color);
    CHECK(final_second == manager.mixed_filaments()[1].display_color);
    CHECK(tracery_preview_slot({final_first == final_second ? 0.0 : 100.0}, true) == 0);
}

TEST_CASE("Previewing a full palette leaves shared recipes and stable identities unchanged", "[TraceryPlate]")
{
    MixedFilamentManager manager;
    MixedFilamentDisplayContext context;
    context.preview_settings.local_z_mode = true;
    context.preview_settings.preferred_a_height = 0.1;
    context.preview_settings.preferred_b_height = 0.1;
    manager.set_display_context(context);
    const std::vector<std::string> colours{"#FF00FF", "#FFFF00", "#00FFFF"};
    MixedFilamentBatchEntry entry;
    entry.component_a = 1;
    entry.component_b = 2;
    entry.mix_b_percent = 45;
    entry.distribution_mode = int(MixedFilament::LayerCycle);
    manager.add_batch_custom_filaments(std::vector<MixedFilamentBatchEntry>(MAXIMUM_FILAMENT_NUMBER - colours.size(), entry), colours);
    REQUIRE(manager.total_filaments(colours.size()) == MAXIMUM_FILAMENT_NUMBER);
    const auto original = manager.mixed_filaments();
    const auto original_colours = manager.display_colors();
    // Repeated strokes still resolve to the existing displayed colour when full.
    for (int percent : {30, 38, 45, 53, 60, 70}) {
        entry.mix_b_percent = percent;
        const auto preview = manager.preview_custom_filament(entry, colours);
        REQUIRE(preview == original_colours.front());
        CHECK(tracery_preview_slot({preview == original_colours.front() ? 0.0 : 100.0}, false) == 0);
    }
    CHECK(manager.mixed_filaments() == original);
    CHECK(manager.display_colors() == original_colours);
    // Three-component previews also use the same normalized sequence as insertion.
    MixedFilamentManager fresh;
    entry.gradient_component_ids = "3/1/2";
    entry.gradient_component_weights = "30/35/35";
    const auto preview = fresh.preview_custom_filament(entry, colours);
    fresh.add_batch_custom_filaments({entry}, colours);
    REQUIRE(fresh.mixed_filaments().size() == 1);
    CHECK(preview == fresh.mixed_filaments().front().display_color);
}
