#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "libslic3r/TraceryThrough.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/TraceryDocument.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/TraceryPlateMaterials.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include <boost/filesystem.hpp>
using namespace Slic3r;
static TraceryThroughOperation stroke(unsigned material = 2, bool erase = false)
{ return {erase, {{{{2, 2, 5}, {8, 2, 5}, {8, 8, 5}, {2, 8, 5}}, {0, 0, -1}, material}}}; }
static double area_mm(const ExPolygons& polygons)
{
    double area = 0;
    for (const auto& p : polygons)
        area += p.area() * SCALING_FACTOR * SCALING_FACTOR;
    return area;
}
TEST_CASE("Through material fills all interior layers without changing the source mesh", "[TraceryThrough]")
{
    auto       mesh     = make_cube(10, 10, 5).its;
    const auto vertices = mesh.vertices;
    const auto faces    = mesh.indices;
    auto       layers   = slice_tracery_through(mesh, {stroke()}, Transform3d::Identity(), {0.1f, 2.5f, 4.9f, 5.1f});
    REQUIRE(layers.size() == 4);
    for (size_t i = 0; i < 3; ++i)
        CHECK(area_mm(layers[i][2]) == Catch::Approx(36).margin(1e-5));
    CHECK(layers[3].empty());
    CHECK(mesh.vertices == vertices);
    CHECK(mesh.indices == faces);
}

TEST_CASE("Through channels stop at the first exit before a second solid", "[TraceryThrough]")
{
    TriangleMesh mesh  = make_cube(10, 10, 5);
    auto         lower = make_cube(10, 10, 5);
    lower.translate(0, 0, -10);
    mesh.merge(lower);
    auto layers = slice_tracery_through(mesh.its, {stroke()}, Transform3d::Identity(), {-7.5f, -2.5f, 2.5f});
    CHECK(layers[0].empty());
    CHECK(layers[1].empty());
    CHECK(area_mm(layers[2][2]) == Catch::Approx(36).margin(1e-5));
}

TEST_CASE("Through channels follow a sloping exit instead of a centroid depth", "[TraceryThrough]")
{
    auto mesh = make_cube(10, 10, 5).its;
    for (auto& vertex : mesh.vertices)
        if (vertex.z() == 0)
            vertex.z() = 0.2f * vertex.x();
    auto layers = slice_tracery_through(mesh, {stroke()}, Transform3d::Identity(), {0.2f, 1.f, 3.f});
    CHECK(layers[0].empty());
    CHECK(area_mm(layers[1][2]) == Catch::Approx(18).margin(1e-4));
    CHECK(area_mm(layers[2][2]) == Catch::Approx(36).margin(1e-5));
}

TEST_CASE("Channels parallel to print layers and mirrored transforms retain their material", "[TraceryThrough]")
{
    auto                    mesh = make_cube(10, 10, 5).its;
    TraceryThroughOperation side{false, {{{{10, 2, 1}, {10, 8, 1}, {10, 8, 4}, {10, 2, 4}}, {-1, 0, 0}, 2}}};
    Transform3d             transform = Transform3d::Identity();
    transform.linear()(0, 0)          = -1;
    transform.translation()           = Vec3d(25, 15, 0);
    auto layers                       = slice_tracery_through(mesh, {side}, transform, {0.5f, 2.5f, 4.5f});
    CHECK(layers[0].empty());
    CHECK(area_mm(layers[1][2]) == Catch::Approx(60).margin(1e-4));
    CHECK(layers[2].empty());
}

TEST_CASE("Later strokes replace materials and erase restores the underlying volume", "[TraceryThrough]")
{
    auto mesh    = make_cube(10, 10, 5).its;
    auto repaint = stroke(3);
    for (auto& p : repaint.patches[0].entry)
        p.x() = 4 + (p.x() - 2) / 3;
    auto layers = slice_tracery_through(mesh, {stroke(), repaint}, Transform3d::Identity(), {2.5f});
    CHECK(area_mm(layers[0][2]) == Catch::Approx(24).margin(1e-4));
    CHECK(area_mm(layers[0][3]) == Catch::Approx(12).margin(1e-4));
    CHECK(intersection_ex(layers[0][2], layers[0][3]).empty());
    repaint.erase = true;
    layers        = slice_tracery_through(mesh, {stroke(), repaint}, Transform3d::Identity(), {2.5f});
    CHECK(area_mm(layers[0][2]) == Catch::Approx(24).margin(1e-4));
    CHECK(layers[0].count(3) == 0);
}

static TriangleSelector::Tracery2DData top_entry(const TriangleMesh& mesh, unsigned material)
{
    TriangleSelector::Tracery2DData regions;
    const auto                      box = mesh.bounding_box();
    for (size_t i = 0; i < mesh.its.indices.size(); ++i) {
        const auto& t = mesh.its.indices[i];
        if (mesh.its.vertices[t[0]].z() != box.max.z() || mesh.its.vertices[t[1]].z() != box.max.z() ||
            mesh.its.vertices[t[2]].z() != box.max.z())
            continue;
        TriangleSelector::TraceryFacet2DData facet;
        facet.source_facet_id = int(i);
        facet.origin          = Vec3d(box.min.x(), box.min.y(), box.max.z());
        TriangleSelector::TraceryRegion2D region;
        region.color   = EnforcerBlockerType(material);
        region.polygon = {{2, 2}, {8, 2}, {8, 8}, {2, 8}};
        facet.regions.push_back(region);
        regions.push_back(facet);
    }
    return regions;
}

TEST_CASE("Through documents round trip, remap recipes and keep display paint out of surface slicing", "[TraceryThrough]")
{
    const auto                               mesh = make_cube(10, 10, 5);
    std::vector<TraceryDocument::Annotation> annotations(8);
    auto                                     document = TraceryDocument::capture(mesh, annotations);
    const auto                               regions  = top_entry(mesh, 2);
    document.apply_vector_edit(mesh, annotations, regions, TraceryEdit::Paint, &regions);
    CHECK(document.material_surfaces().count(2) == 1);
    CHECK(document.material_surfaces(true).count(2) == 0);
    auto restored = TraceryDocument::decode(document.encode());
    CHECK(restored.encode() == document.encode());
    auto mapping = tracery_identity_material_map();
    mapping[2]   = EnforcerBlockerType(3);
    restored.remap_materials(mapping);
    auto layers = slice_tracery_through(mesh.its, restored.through_operations(), Transform3d::Identity(), {2.5f});
    CHECK(area_mm(layers[0][3]) == Catch::Approx(36).margin(1e-4));
    restored.apply_vector_edit(mesh, annotations, regions, TraceryEdit::Erase, &regions);
    layers = slice_tracery_through(mesh.its, restored.through_operations(), Transform3d::Identity(), {2.5f});
    CHECK(layers[0].empty());
    restored.clear_vector_glass(mesh, annotations);
    CHECK(restored.through_operations().empty());
}

TEST_CASE("Cached publication preserves full-depth operations and separate slice surfaces", "[TraceryThrough][TraceryCache]")
{
    const auto                               mesh = make_cube(10, 10, 5);
    std::vector<TraceryDocument::Annotation> annotations(8);
    auto                                     document = TraceryDocument::capture(mesh, annotations);
    const auto                               regions  = top_entry(mesh, 2);
    document.apply_vector_edit(mesh, annotations, regions, TraceryEdit::Paint, &regions);
    const auto                      expected = document.encode();
    TraceryDocument::ReadCacheScope cache;
    const auto                      encoded = std::move(document).encode_cached();
    REQUIRE(encoded == expected);
    const auto snapshot = TraceryDocument::decode_cached(encoded);
    REQUIRE(snapshot->format_version() == 3);
    CHECK(snapshot->material_surfaces().count(2) == 1);
    CHECK(snapshot->material_surfaces(true).count(2) == 0);
    auto layers = slice_tracery_through(mesh.its, snapshot->through_operations(), Transform3d::Identity(), {2.5f});
    CHECK(area_mm(layers[0][2]) == Catch::Approx(36).margin(1e-4));
    auto edited = *snapshot;
    edited.clear_vector_glass(mesh, annotations);
    CHECK(edited.through_operations().empty());
    CHECK_FALSE(snapshot->through_operations().empty());
    CHECK(TraceryDocument::decode_cached(encoded) == snapshot);
    CHECK(TraceryDocument::decode(encoded).encode() == encoded);
}

TEST_CASE("Full-depth vectors reach every real slicer layer including the middle", "[TraceryThrough][TraceryThroughIntegration]")
{
    Model model;
    auto* object = model.add_object();
    auto* volume = object->add_volume(make_cube(10, 10, 5));
    object->add_instance();
    std::vector<TraceryDocument::Annotation> annotations(8);
    auto                                     document = TraceryDocument::capture(volume->mesh(), annotations);
    const auto                               regions  = top_entry(volume->mesh(), 2);
    document.apply_vector_edit(volume->mesh(), annotations, regions, TraceryEdit::Paint, &regions);
    volume->tracery_edit_data = document.encode();
    DynamicPrintConfig config;
    config.apply(FullPrintConfig::defaults());
    config.set_key_value("filament_diameter", new ConfigOptionFloats{1.75, 1.75});
    config.set_key_value("filament_colour", new ConfigOptionStrings{"#FFFFFF", "#FF0000"});
    config.set_key_value("outer_wall_line_width", new ConfigOptionFloatOrPercent(0.42, false));
    config.set_key_value("inner_wall_line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));
    Print print;
    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);
    auto* printed = print.get_object(0);
    printed->slice();
    REQUIRE(printed->layer_count() > 20);
    for (size_t i = 0; i < printed->layer_count(); ++i) {
        const auto* layer   = printed->get_layer(int(i));
        double      colored = 0;
        for (const auto* region : layer->regions()) {
            if (region->region().config().wall_filament.value != 2)
                continue;
            for (const auto& surface : region->slices.surfaces)
                colored += area_mm({surface.expolygon});
        }
        INFO("layer=" << i);
        CHECK(colored == Catch::Approx(36).margin(0.01));
    }
    CHECK(volume->mmu_segmentation_facets.empty());
    CHECK(volume->mesh().its.indices.size() == 12);
}

static DynamicPrintConfig through_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(4);
    config.set_num_filaments(4);
    config.set_key_value("filament_diameter", new ConfigOptionFloats{1.75, 1.75, 1.75, 1.75});
    config.set_key_value("filament_colour", new ConfigOptionStrings{"#FFFFFF", "#FF0000", "#00FF00", "#0000FF"});
    config.set_key_value("outer_wall_line_width", new ConfigOptionFloatOrPercent(0.42, false));
    config.set_key_value("inner_wall_line_width", new ConfigOptionFloatOrPercent(0.45, false));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));
    return config;
}
static void paint_through(ModelVolume& volume, unsigned material)
{
    std::vector<TraceryDocument::Annotation> annotations(8);
    auto                                     document = TraceryDocument::capture(volume.mesh(), annotations);
    const auto                               regions  = top_entry(volume.mesh(), material);
    document.apply_vector_edit(volume.mesh(), annotations, regions, TraceryEdit::Paint, &regions);
    volume.tracery_edit_data = document.encode();
}

TEST_CASE("Through mixed recipes expand across interior layers without falling back to surface depth",
          "[TraceryThrough][TraceryThroughIntegration]")
{
    struct AutoGeneration
    {
        bool previous = MixedFilamentManager::auto_generate_enabled();
        AutoGeneration() { MixedFilamentManager::set_auto_generate_enabled(false); }
        ~AutoGeneration() { MixedFilamentManager::set_auto_generate_enabled(previous); }
    } guard;
    Model model;
    auto* object = model.add_object();
    auto* volume = object->add_volume(make_cube(10, 10, 5));
    object->add_instance();
    MixedFilamentManager manager;
    manager.add_custom_filament(2, 3, 50, {"#FFFFFF", "#FF0000", "#00FF00", "#0000FF"});
    manager.mixed_filaments().front().distribution_mode = int(MixedFilament::Simple);
    manager.mixed_filaments().front().manual_pattern.clear();
    auto config = through_config();
    config.set("mixed_filament_definitions", manager.serialize_custom_entries());
    config.set_key_value("mixed_filament_region_collapse", new ConfigOptionBool(true));
    config.set_key_value("dithering_local_z_mode", new ConfigOptionBool(false));
    paint_through(*volume, 5);
    Print print;
    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.mixed_filament_manager().is_mixed(5, 4));
    auto* printed = print.get_object(0);
    printed->slice();
    std::set<int> components;
    for (size_t i = 0; i < printed->layer_count(); ++i) {
        double colored = 0;
        for (const auto* region : printed->get_layer(int(i))->regions()) {
            const int material = region->region().config().wall_filament.value;
            if (material != 2 && material != 3)
                continue;
            for (const auto& surface : region->slices.surfaces) {
                colored += area_mm({surface.expolygon});
                components.insert(material);
            }
        }
        INFO("layer=" << i);
        CHECK(colored == Catch::Approx(36).margin(0.01));
    }
    CHECK(components == std::set<int>{2, 3});
}

TEST_CASE("Through ownership respects later solid parts and negative volumes", "[TraceryThrough][TraceryThroughIntegration]")
{
    for (bool negative : {false, true}) {
        Model model;
        auto* object = model.add_object();
        auto* volume = object->add_volume(make_cube(10, 10, 5));
        object->add_instance();
        paint_through(*volume, 2);
        auto* other = object->add_volume(make_cube(2, 10, 5));
        other->set_offset(volume->get_offset());
        other->config.set_key_value("extruder", new ConfigOptionInt(3));
        if (negative)
            other->set_type(ModelVolumeType::NEGATIVE_VOLUME);
        Print print;
        print.set_status_silent();
        print.apply(model, through_config());
        auto* printed = print.get_object(0);
        printed->slice();
        const auto* layer   = printed->get_layer(int(printed->layer_count() / 2));
        double      colored = 0;
        for (const auto* region : layer->regions()) {
            if (region->region().config().wall_filament.value != 2)
                continue;
            for (const auto& surface : region->slices.surfaces)
                colored += area_mm({surface.expolygon});
        }
        INFO("negative=" << negative);
        CHECK(colored == Catch::Approx(24).margin(0.01));
    }
}

TEST_CASE("Full-depth channels survive 3MF save and reopen with unchanged source geometry", "[TraceryThrough]")
{
    Model model, loaded;
    auto* object = model.add_object();
    auto* volume = object->add_volume(make_cube(10, 10, 5));
    object->add_instance();
    paint_through(*volume, 2);
    const auto path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("tracery-through-%%%%-%%%%.3mf");
    model.set_backup_path(path.string() + ".source");
    loaded.set_backup_path(path.string() + ".loaded");
    // A project config is sparse. Full print defaults include generic enums
    // whose runtime maps are not initialized in this standalone test.
    DynamicPrintConfig config;
    config.set_key_value("filament_colour", new ConfigOptionStrings{"#FFFFFF", "#FF0000"});
    config.set_key_value("filament_diameter", new ConfigOptionFloats{1.75, 1.75});
    ConfigSubstitutionContext context{ForwardCompatibilitySubstitutionRule::Disable};
    PlateDataPtrs             plates;
    std::vector<Preset*>      presets;
    bool                      is_bbs = false;
    Semver                    version;
    struct Cleanup
    {
        boost::filesystem::path path;
        Model&                  source;
        Model&                  loaded;
        PlateDataPtrs&          plates;
        ~Cleanup()
        {
            boost::filesystem::remove(path);
            source.remove_backup_path_if_exist();
            loaded.remove_backup_path_if_exist();
            release_PlateData_list(plates);
        }
    } cleanup{path, model, loaded, plates};
    const auto  filename = path.string();
    StoreParams params;
    params.path   = filename.c_str();
    params.model  = &model;
    params.config = &config;
    REQUIRE(store_bbs_3mf(params));
    REQUIRE(
        load_bbs_3mf(filename.c_str(), &config, &context, &loaded, &plates, &presets, &is_bbs, &version, nullptr, LoadStrategy::LoadModel));
    REQUIRE(loaded.objects.size() == 1);
    const auto* reopened = loaded.objects[0]->volumes[0];
    CHECK(reopened->mesh().its.indices == volume->mesh().its.indices);
    const auto doc = TraceryDocument::from_volume_snapshot(*reopened);
    REQUIRE(doc);
    REQUIRE_FALSE(doc->through_operations().empty());
    const auto z      = float(reopened->mesh().bounding_box().center().z());
    auto       layers = slice_tracery_through(reopened->mesh().its, doc->through_operations(), Transform3d::Identity(), {z});
    CHECK(area_mm(layers[0][2]) == Catch::Approx(36).margin(1e-4));
}

TEST_CASE("Negative cavities stop through paint before material below the cavity", "[TraceryThrough][TraceryThroughIntegration]")
{
    Model model;
    auto* object = model.add_object();
    auto* volume = object->add_volume(make_cube(10, 10, 5));
    object->add_instance();
    paint_through(*volume, 2);
    auto* cavity = object->add_volume(make_cube(8, 8, 1));
    cavity->set_type(ModelVolumeType::NEGATIVE_VOLUME);
    cavity->set_offset(volume->get_offset());
    Print print;
    print.set_status_silent();
    print.apply(model, through_config());
    auto* printed = print.get_object(0);
    printed->slice();
    for (size_t i = 0; i < printed->layer_count(); ++i) {
        const auto* layer   = printed->get_layer(int(i));
        double      colored = 0;
        for (const auto* region : layer->regions())
            if (region->region().config().wall_filament.value == 2)
                for (const auto& surface : region->slices.surfaces)
                    colored += area_mm({surface.expolygon});
        // The source cube spans print Z 0..5; the centered cavity spans 2..3.
        INFO("z=" << layer->slice_z);
        CHECK(colored == Catch::Approx(layer->slice_z > 3 ? 36 : 0).margin(0.01));
    }
}

TEST_CASE("Full-depth slicing scales to a grid of color cells", "[TraceryThroughPerformance]")
{
    const auto              mesh = make_cube(20, 20, 5);
    TraceryThroughOperation operation;
    for (int y = 0; y < 20; ++y)
        for (int x = 0; x < 20; ++x)
            operation.patches.push_back(
                {{{double(x), double(y), 5}, {double(x + 1), double(y), 5}, {double(x + 1), double(y + 1), 5}, {double(x), double(y + 1), 5}},
                 {0, 0, -1},
                 unsigned(2 + (x + y) % 3)});
    std::vector<float> zs;
    for (int i = 0; i < 25; ++i)
        zs.push_back(0.1f + i * 0.2f);
    auto layers = slice_tracery_through(mesh.its, {operation}, Transform3d::Identity(), zs);
    for (const auto& layer : layers) {
        double total = 0;
        for (const auto& [material, polygons] : layer)
            total += area_mm(polygons);
        CHECK(total == Catch::Approx(400).margin(1e-3));
    }
}
