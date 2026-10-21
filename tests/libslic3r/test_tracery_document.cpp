#include <catch2/catch_test_macros.hpp>
#include "libslic3r/TraceryDocument.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include <boost/filesystem.hpp>
using namespace Slic3r;

TEST_CASE("Rebuilding stained glass never promotes derived triangles into the base", "[TraceryDocument]")
{
    indexed_triangle_set mesh;
    mesh.vertices = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}};
    mesh.indices  = {{0, 1, 2}};
    TriangleMesh                                         base(mesh);
    std::vector<TriangleSelector::TriangleSplittingData> annotations(8);
    auto                                                 document = TraceryDocument::capture(base, annotations);
    TriangleSelector::TraceryFacet2DData                 facet;
    facet.source_facet_id = 0;
    TriangleSelector::TraceryRegion2D region;
    region.polygon = {{0, 0}, {4, 0}, {4, 6}, {0, 10}};
    region.color   = EnforcerBlockerType(2);
    facet.regions.push_back(region);
    auto first = document.rebuild(base, annotations, {facet}, TraceryEdit::Paint);
    REQUIRE(first.mesh.indices.size() > 1);
    const auto expected = first.mesh;
    for (int stroke = 0; stroke < 10; ++stroke) {
        TriangleMesh current(first.mesh);
        first = document.rebuild(current, first.annotations, {}, TraceryEdit::Paint);
        CHECK(document.base_mesh().its.indices.size() == 1);
        CHECK(first.mesh.vertices == expected.vertices);
        CHECK(first.mesh.indices == expected.indices);
        for (int source : first.source_facets)
            CHECK(source == 0);
    }
}

TEST_CASE("Erasing part of stained glass restores paint without regenerating the remainder", "[TraceryDocument]")
{
    indexed_triangle_set mesh;
    mesh.vertices = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}};
    mesh.indices  = {{0, 1, 2}};
    TriangleMesh     base(mesh);
    TriangleSelector original(base), mask(base);
    original.set_facet(0, EnforcerBlockerType(5));
    mask.set_facet(0, EnforcerBlockerType(1));
    std::vector<TriangleSelector::TriangleSplittingData> annotations(8);
    annotations[0] = annotations[5] = original.serialize();
    auto document                   = TraceryDocument::capture(base, annotations);
    annotations[6]                  = mask.serialize();
    TriangleSelector::TraceryFacet2DData facet;
    facet.source_facet_id = 0;
    TriangleSelector::TraceryRegion2D left, right;
    left.polygon           = {{0, 0}, {4, 0}, {4, 6}, {0, 10}};
    right.polygon          = {{4, 0}, {10, 0}, {4, 6}};
    left.color             = EnforcerBlockerType(2);
    right.color            = EnforcerBlockerType(3);
    facet.regions          = {left, right};
    auto             first = document.rebuild(base, annotations, {facet}, TraceryEdit::Paint);
    TriangleMesh     current(first.mesh);
    TriangleSelector erase(current);
    erase.set_leaf_states([](const Vec3f& p, int) { return EnforcerBlockerType(p.x() < 4 ? 0 : 1); });
    auto changed            = first.annotations;
    changed[6]              = erase.serialize();
    auto             result = document.rebuild(current, changed, {}, TraceryEdit::Erase);
    TriangleMesh     output(result.mesh);
    TriangleSelector colours(output);
    colours.deserialize(result.annotations[0], false);
    for (const auto& leaf : colours.painted_leaves(true)) {
        const Vec3f center = (leaf.vertices[0] + leaf.vertices[1] + leaf.vertices[2]) / 3.f;
        CHECK(unsigned(leaf.state) == (center.x() < 4 ? 5 : 3));
    }
}

TEST_CASE("Saved stained-glass editing data resumes on the same base and rejects changed geometry", "[TraceryDocument]")
{
    indexed_triangle_set mesh;
    mesh.vertices = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}};
    mesh.indices  = {{0, 1, 2}};
    TriangleMesh                                         base(mesh);
    std::vector<TriangleSelector::TriangleSplittingData> annotations(8);
    auto                                                 document = TraceryDocument::capture(base, annotations);
    TriangleSelector::TraceryFacet2DData                 facet;
    facet.source_facet_id = 0;
    TriangleSelector::TraceryRegion2D region;
    region.polygon = {{1, 1}, {4, 1}, {1, 4}};
    region.color   = EnforcerBlockerType(2);
    facet.regions.push_back(region);
    auto         result   = document.rebuild(base, annotations, {facet}, TraceryEdit::Paint);
    const auto   encoded  = document.encode();
    auto         reopened = TraceryDocument::decode(encoded);
    TriangleMesh current(result.mesh);
    CHECK(reopened.matches(current));
    auto again = reopened.rebuild(current, result.annotations, {}, TraceryEdit::Paint);
    CHECK(again.mesh.vertices == result.mesh.vertices);
    CHECK(again.mesh.indices == result.mesh.indices);
    CHECK(again.annotations == result.annotations);
    current.its.vertices[0].x() += 1.f; // same facet count is not proof of identity
    CHECK_FALSE(reopened.matches(current));
    CHECK_THROWS(reopened.rebuild(current, result.annotations, {}, TraceryEdit::Paint));
    CHECK(reopened.encode() == encoded);
    CHECK_THROWS(TraceryDocument::decode("{\"version\":999}"));
}

TEST_CASE("Project save preserves editable base data and volume copies own their edits", "[TraceryDocument]")
{
    TriangleMesh base = make_cube(10, 10, 1);
    base.translate(-5.f, -5.f, -.5f);
    std::vector<TriangleSelector::TriangleSplittingData> annotations(8);
    auto                                                 document = TraceryDocument::capture(base, annotations);
    TriangleSelector::Tracery2DData                      paint;
    for (size_t i = 0; i < base.its.indices.size(); ++i) {
        const auto& t = base.its.indices[i];
        if (base.its.vertices[t[0]].z() != .5f || base.its.vertices[t[1]].z() != .5f || base.its.vertices[t[2]].z() != .5f)
            continue;
        TriangleSelector::TraceryFacet2DData f;
        f.source_facet_id = int(i);
        f.origin.z()      = .5;
        TriangleSelector::TraceryRegion2D r;
        r.polygon = {{-5, -5}, {.2, -5}, {2.2, 5}, {-5, 5}};
        r.color   = EnforcerBlockerType(2);
        f.regions.push_back(r);
        paint.push_back(f);
    }
    auto  result = document.rebuild(base, annotations, paint, TraceryEdit::Paint);
    Model model;
    model.set_backup_path((boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("tracery-source-%%%%-%%%%")).string());
    auto* object = model.add_object();
    auto* volume = object->add_volume(TriangleMesh(result.mesh));
    object->add_instance();
    REQUIRE(document.matches(volume->mesh()));
    volume->tracery_edit_data = document.encode();
    TriangleSelector colours(volume->mesh());
    colours.deserialize(result.annotations[0], false);
    volume->mmu_segmentation_facets.set(colours);
    auto* copy = model.add_object(*object);
    CHECK(copy->volumes[0]->tracery_edit_data == volume->tracery_edit_data);
    copy->volumes[0]->set_mesh(make_cube(2, 2, 2));
    CHECK(copy->volumes[0]->tracery_edit_data.empty());
    CHECK_FALSE(volume->tracery_edit_data.empty());
    const auto         path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("tracery-document-%%%%-%%%%.3mf");
    Model              loaded;
    DynamicPrintConfig config;
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
    loaded.set_backup_path((path.string() + ".backup"));
    StoreParams       params;
    const std::string filename = path.string();
    params.path                = filename.c_str();
    params.model               = &model;
    params.config              = &config;
    REQUIRE(store_bbs_3mf(params));
    REQUIRE(
        load_bbs_3mf(filename.c_str(), &config, &context, &loaded, &plates, &presets, &is_bbs, &version, nullptr, LoadStrategy::LoadModel));
    REQUIRE(loaded.objects.size() == 2);
    const auto* restored = loaded.objects.front()->volumes.front();
    REQUIRE_FALSE(restored->tracery_edit_data.empty());
    auto reopened = TraceryDocument::decode(restored->tracery_edit_data);
    REQUIRE(reopened.matches(restored->mesh()));
    auto current_annotations = reopened.last_annotations();
    current_annotations[0]   = restored->mmu_segmentation_facets.get_data();
    auto rebuilt             = reopened.rebuild(restored->mesh(), current_annotations, {}, TraceryEdit::Paint);
    CHECK(rebuilt.annotations == result.annotations);
    CHECK(rebuilt.mesh.vertices == result.mesh.vertices);
    CHECK(rebuilt.mesh.indices == result.mesh.indices);
}

TEST_CASE("Ordinary paint and support edits survive erasing a derived flower mesh", "[TraceryDocument]")
{
    indexed_triangle_set its;
    its.vertices = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}};
    its.indices  = {{0, 1, 2}};
    TriangleMesh     base(its);
    TriangleSelector initial(base), mask(base);
    initial.set_facet(0, EnforcerBlockerType(5));
    mask.set_facet(0, EnforcerBlockerType(1));
    std::vector<TraceryDocument::Annotation> annotations(8);
    annotations[0] = annotations[5] = initial.serialize();
    auto document                   = TraceryDocument::capture(base, annotations);
    annotations[6]                  = mask.serialize();
    TriangleSelector::TraceryFacet2DData facet;
    facet.source_facet_id = 0;
    TriangleSelector::TraceryRegion2D region;
    region.color   = EnforcerBlockerType(2);
    region.polygon = {{0, 0}, {4, 0}, {4, 6}, {0, 10}};
    facet.regions.push_back(region);
    auto             first = document.rebuild(base, annotations, {facet}, TraceryEdit::Paint);
    TriangleMesh     current(first.mesh);
    TriangleSelector ordinary(current), support(current);
    ordinary.deserialize(first.annotations[0], false);
    ordinary.set_leaf_states([](const Vec3f& p, int) { return EnforcerBlockerType(p.x() < 4 ? 7 : 5); });
    support.set_leaf_states([](const Vec3f&, int) { return EnforcerBlockerType::BLOCKER; });
    auto edited             = first.annotations;
    edited[0]               = ordinary.serialize();
    edited[1]               = support.serialize();
    edited[6]               = {};
    auto             erased = document.rebuild(current, edited, {}, TraceryEdit::Erase);
    TriangleMesh     output(erased.mesh);
    TriangleSelector colours(output), supports(output);
    colours.deserialize(erased.annotations[0], false);
    supports.deserialize(erased.annotations[1], false);
    for (const auto& leaf : colours.painted_leaves(true)) {
        const Vec3f p = (leaf.vertices[0] + leaf.vertices[1] + leaf.vertices[2]) / 3.f;
        CHECK(unsigned(leaf.state) == (p.x() < 4 ? 7 : 5));
        CHECK(supports.state_at(p, leaf.source_facet) == EnforcerBlockerType::BLOCKER);
    }
}

TEST_CASE("Projected erase and material remapping use saved base colours", "[TraceryDocument]")
{
    indexed_triangle_set its;
    its.vertices = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}};
    its.indices  = {{0, 1, 2}};
    TriangleMesh     base(its);
    TriangleSelector original(base);
    original.set_facet(0, EnforcerBlockerType(5));
    std::vector<TraceryDocument::Annotation> annotations(8);
    annotations[0] = annotations[5]               = original.serialize();
    auto                                 document = TraceryDocument::capture(base, annotations);
    TriangleSelector::TraceryFacet2DData facet;
    facet.source_facet_id = 0;
    TriangleSelector::TraceryRegion2D region;
    region.color   = EnforcerBlockerType(2);
    region.polygon = {{0, 0}, {10, 0}, {0, 10}};
    facet.regions.push_back(region);
    auto                    first = document.rebuild(base, annotations, {facet}, TraceryEdit::Paint);
    EnforcerBlockerStateMap mapping;
    for (size_t i = 0; i < mapping.size(); ++i)
        mapping[i] = EnforcerBlockerType(i);
    mapping[2] = EnforcerBlockerType(3);
    mapping[5] = EnforcerBlockerType(6);
    document.remap_materials(mapping);
    TriangleMesh     current(first.mesh);
    auto             erased = document.rebuild(current, document.last_annotations(), {facet}, TraceryEdit::Erase);
    TriangleMesh     output(erased.mesh);
    TriangleSelector colours(output);
    colours.deserialize(erased.annotations[0], false);
    for (const auto& leaf : colours.painted_leaves(true))
        CHECK(unsigned(leaf.state) == 6);
}

TEST_CASE("Sub-micron base facets retain their paint through project serialization", "[TraceryDocument]")
{
    indexed_triangle_set its;
    its.vertices = {{17.2507915f, 9.7820549f, 5.f}, {17.2342682f, 9.89667511f, 5.f}, {17.2413483f, 9.84755135f, 5.f}};
    its.indices  = {{0, 1, 2}};
    TriangleMesh     base(its);
    TriangleSelector paint(base);
    paint.set_facet(0, EnforcerBlockerType(5));
    std::vector<TraceryDocument::Annotation> annotations(8);
    annotations[0] = annotations[5] = paint.serialize();
    auto             document       = TraceryDocument::decode(TraceryDocument::capture(base, annotations).encode());
    auto             output         = document.rebuild(base, annotations, {}, TraceryEdit::Paint);
    TriangleMesh     mesh(output.mesh);
    TriangleSelector restored(mesh);
    restored.deserialize(output.annotations[0], false);
    for (const auto& leaf : restored.painted_leaves(true))
        CHECK(unsigned(leaf.state) == 5);
}

TEST_CASE("Repeated paint and erase do not accumulate derived topology", "[TraceryDocument]")
{
    indexed_triangle_set its;
    its.vertices = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}};
    its.indices  = {{0, 1, 2}};
    TriangleMesh                             current(its);
    std::vector<TraceryDocument::Annotation> annotations(8);
    auto                                     document    = TraceryDocument::capture(current, annotations);
    size_t                                   first_count = 0;
    for (int stroke = 0; stroke < 12; ++stroke) {
        TriangleSelector mask(current);
        mask.set_leaf_states([](const Vec3f&, int) { return EnforcerBlockerType(1); });
        annotations[6] = mask.serialize();
        TriangleSelector::Tracery2DData regions;
        for (size_t i = 0; i < current.its.indices.size(); ++i) {
            TriangleSelector::TraceryFacet2DData facet;
            facet.source_facet_id = int(i);
            TriangleSelector::TraceryRegion2D region;
            region.color   = EnforcerBlockerType(2);
            region.polygon = {{0, 0}, {4, 0}, {4, 6}, {0, 10}};
            facet.regions.push_back(region);
            regions.push_back(facet);
        }
        auto painted = document.rebuild(current, annotations, regions, TraceryEdit::Paint);
        if (stroke == 0)
            first_count = painted.mesh.indices.size();
        CHECK(painted.mesh.indices.size() == first_count);
        annotations    = painted.annotations;
        annotations[6] = {};
        auto erased    = document.rebuild(TriangleMesh(painted.mesh), annotations, {}, TraceryEdit::Erase);
        CHECK(erased.mesh.indices == its.indices);
        CHECK(document.base_mesh().its.indices == its.indices);
        current     = TriangleMesh(erased.mesh);
        annotations = erased.annotations;
    }
}

TEST_CASE("Material slot changes update colours hidden underneath stained glass", "[TraceryDocument]")
{
    indexed_triangle_set its;
    its.vertices = {{-5, -5, 0}, {5, -5, 0}, {-5, 5, 0}};
    its.indices  = {{0, 1, 2}};
    Model            model;
    auto*            volume = model.add_object()->add_volume(TriangleMesh(its));
    TriangleSelector base_colour(volume->mesh()), displayed_colour(volume->mesh());
    base_colour.set_facet(0, EnforcerBlockerType(5));
    displayed_colour.set_facet(0, EnforcerBlockerType(2));
    std::vector<TraceryDocument::Annotation> annotations(8);
    annotations[0] = displayed_colour.serialize();
    annotations[5] = base_colour.serialize();
    volume->mmu_segmentation_facets.set(displayed_colour);
    volume->tracery_edit_data = TraceryDocument::capture(volume->mesh(), annotations).encode();
    unsigned expected         = 0;
    SECTION("Deleting a hidden slot replaces its base colour")
    {
        volume->update_extruder_count_when_delete_filament(5, 5, 3);
        expected = 3;
    }
    SECTION("Clipping the material count clears hidden out-of-range slots") { volume->update_extruder_count(3); }
    SECTION("Reindexing also remaps hidden slots")
    {
        EnforcerBlockerStateMap mapping;
        for (size_t i = 0; i < mapping.size(); ++i)
            mapping[i] = EnforcerBlockerType(i);
        mapping[5] = EnforcerBlockerType(4);
        volume->remap_extruder_ids(5, mapping);
        expected = 4;
    }
    auto             document = TraceryDocument::decode(volume->tracery_edit_data);
    auto             result   = document.rebuild(volume->mesh(), document.last_annotations(), {}, TraceryEdit::Regenerate);
    TriangleMesh     mesh(result.mesh);
    TriangleSelector restored(mesh);
    restored.deserialize(result.annotations[0], false);
    for (const auto& leaf : restored.painted_leaves(true))
        CHECK(unsigned(leaf.state) == expected);
}

TEST_CASE("Sub-tolerance facets keep their dominant state without unstable internal cuts", "[TraceryDocument]")
{
    indexed_triangle_set its;
    its.vertices = {{0, 0, 0}, {1, 0, 0}, {0, 0.000001f, 0}};
    its.indices  = {{0, 1, 2}};
    TriangleMesh     base(its);
    TriangleSelector original(base);
    original.set_facet(0, EnforcerBlockerType(5));
    std::vector<TraceryDocument::Annotation> annotations(8);
    annotations[0] = annotations[5]               = original.serialize();
    auto                                 document = TraceryDocument::capture(base, annotations);
    TriangleSelector::TraceryFacet2DData facet;
    facet.source_facet_id = 0;
    TriangleSelector::TraceryRegion2D region;
    region.color   = EnforcerBlockerType(2);
    region.polygon = {{0, 0}, {.2, 0}, {0, .0000002}};
    facet.regions.push_back(region);
    auto result = document.rebuild(base, annotations, {facet}, TraceryEdit::Paint);
    REQUIRE(result.mesh.indices == its.indices);
    REQUIRE(result.mesh.vertices == its.vertices);
    TriangleSelector colours(base);
    colours.deserialize(result.annotations[0], false);
    CHECK(colours.painted_leaves().front().state == EnforcerBlockerType(5));
    // A full-facet colour change still applies; only sub-tolerance internal
    // boundaries are approximated, not the entire original facet discarded.
    facet.regions.front().polygon = {{0, 0}, {1, 0}, {0, .000001}};
    result                        = document.rebuild(base, result.annotations, {facet}, TraceryEdit::Paint);
    colours.deserialize(result.annotations[0], false);
    CHECK(colours.painted_leaves().front().state == EnforcerBlockerType(2));
    document = TraceryDocument::decode(document.encode());
    result   = document.rebuild(base, result.annotations, {}, TraceryEdit::Regenerate);
    colours.deserialize(result.annotations[0], false);
    CHECK(colours.painted_leaves().front().state == EnforcerBlockerType(5));
}
