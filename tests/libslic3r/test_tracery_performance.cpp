#include <catch2/catch_test_macros.hpp>
#include "libslic3r/Model.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/TraceryMaterialPlan.hpp"
#include "libslic3r/TraceryPartition.hpp"
#include "libslic3r/TraceryPlateMaterials.hpp"
#include "slic3r/GUI/MixedColorMatchHelpers.hpp"
#include "slic3r/GUI/MixedFilamentColorMapPanel.hpp"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;

namespace {
using Clock = std::chrono::steady_clock;
constexpr int repeats = 3;
constexpr size_t paint_channels = 8; // Same channels as the flower apply path.

// Timing is diagnostic, never a machine-dependent correctness assertion.
// Fixture construction, warm-up, assertions and destruction are outside samples.
template<class Operation, class Verify>
void measure(const std::string& scenario, size_t facets, size_t items, Operation operation, Verify verify)
{
    { auto warm = operation(); verify(warm); }
    std::vector<double> samples;
    for (int i = 0; i < repeats; ++i) {
        const auto begin = Clock::now();
        auto result = operation();
        const auto end = Clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(end - begin).count());
        verify(result);
    }
    std::sort(samples.begin(), samples.end());
    std::cout << "TRACERY_PERF scenario=" << scenario << " facets=" << facets << " items=" << items
              << " repeats=" << repeats << std::fixed << std::setprecision(3)
              << " median_ms=" << samples[samples.size() / 2] << " min_ms=" << samples.front()
              << " max_ms=" << samples.back() << '\n';
}

TriangleMesh grid(int side)
{
    indexed_triangle_set mesh;
    for (int y = 0; y <= side; ++y)
        for (int x = 0; x <= side; ++x)
            mesh.vertices.emplace_back(float(x), float(y), 0.f);
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side; ++x) {
            const int a = y * (side + 1) + x;
            mesh.indices.emplace_back(a, a + 1, a + side + 2);
            mesh.indices.emplace_back(a, a + side + 2, a + side + 1);
        }
    return TriangleMesh(std::move(mesh));
}

// Model objects need a non-degenerate solid for their convex hull. Preserve a
// dense painted surface without measuring warnings from a zero-thickness model.
TriangleMesh slab(int side)
{
    auto mesh = grid(side).its;
    const int vertices = int(mesh.vertices.size());
    const size_t faces = mesh.indices.size();
    for (int i = 0; i < vertices; ++i) {
        const Vec3f point = mesh.vertices[i] - Vec3f(0.f, 0.f, 1.f);
        mesh.vertices.push_back(point);
    }
    for (size_t i = 0; i < faces; ++i) {
        const Vec3i32 face = mesh.indices[i];
        mesh.indices.emplace_back(face[2] + vertices, face[1] + vertices, face[0] + vertices);
    }
    const auto wall = [&](int a, int b) {
        mesh.indices.emplace_back(a, a + vertices, b + vertices);
        mesh.indices.emplace_back(a, b + vertices, b);
    };
    for (int i = 0; i < side; ++i) {
        wall(i, i + 1);
        wall(i * (side + 1) + side, (i + 1) * (side + 1) + side);
        wall(side * (side + 1) + i + 1, side * (side + 1) + i);
        wall((i + 1) * (side + 1), i * (side + 1));
    }
    return TriangleMesh(std::move(mesh));
}

// A constant one-square-millimetre brush footprint; only two original facets
// are touched. Larger grids add unpainted geometry, not a larger brush.
TriangleSelector::Tracery2DData stroke(const TriangleMesh& mesh, int x)
{
    TriangleSelector::Tracery2DData regions;
    for (size_t i = 0; i < mesh.its.indices.size(); ++i) {
        const auto& t = mesh.its.indices[i];
        const Vec3f lo = mesh.its.vertices[t[0]].cwiseMin(mesh.its.vertices[t[1]]).cwiseMin(mesh.its.vertices[t[2]]);
        const Vec3f hi = mesh.its.vertices[t[0]].cwiseMax(mesh.its.vertices[t[1]]).cwiseMax(mesh.its.vertices[t[2]]);
        if (lo.x() >= x + 1 || hi.x() <= x || lo.y() >= 1 || hi.y() <= 0)
            continue;
        TriangleSelector::TraceryFacet2DData facet;
        facet.source_facet_id = int(i);
        TriangleSelector::TraceryRegion2D region;
        region.color = EnforcerBlockerType(2);
        region.polygon = {Vec2d(x, 0), Vec2d(x + 0.37, 0), Vec2d(x + 0.67, 1), Vec2d(x, 1)};
        facet.regions.push_back(std::move(region));
        regions.push_back(std::move(facet));
    }
    return regions;
}

void verify_partition(const TraceryPartitionResult& result, double expected_area)
{
    REQUIRE(result.annotations.size() == paint_channels);
    REQUIRE(result.source_facets.size() == result.mesh.indices.size());
    double area = 0.;
    for (const auto& t : result.mesh.indices) {
        const Vec3d a = result.mesh.vertices[t[0]].cast<double>();
        const Vec3d b = result.mesh.vertices[t[1]].cast<double>();
        const Vec3d c = result.mesh.vertices[t[2]].cast<double>();
        area += (b - a).cross(c - a).norm() * 0.5;
    }
    REQUIRE(std::abs(area - expected_area) < std::max(1e-4, expected_area * 1e-6));
    REQUIRE(result.annotations.front().used_states[2]);
}
} // namespace

TEST_CASE("Profile local flower partition against total mesh size", "[TraceryPerformance]")
{
    for (int side : {16, 64, 128, 512}) {
        const auto mesh = grid(side);
        const auto regions = stroke(mesh, 0);
        REQUIRE(regions.size() == 2);
        const std::vector<TriangleSelector::TriangleSplittingData> annotations(paint_channels);
        measure("local_partition", mesh.its.indices.size(), regions.size(),
            [&] { return build_exact_tracery_partition(mesh, annotations, regions); },
            [&](const auto& result) { verify_partition(result, double(side * side)); });
    }
}

TEST_CASE("Profile flower fragment density at a fixed brush footprint", "[TraceryPerformance]")
{
    const auto mesh = grid(64);
    const std::vector<TriangleSelector::TriangleSplittingData> annotations(paint_channels);
    for (int count : {4, 16, 64}) {
        auto regions = stroke(mesh, 0);
        for (auto& facet : regions) {
            facet.regions.clear();
            for (int i = 0; i < count; ++i) {
                TriangleSelector::TraceryRegion2D region;
                region.fragment_id = size_t(i);
                region.color = EnforcerBlockerType(2 + i % 2);
                const double left = double(i) / count, right = double(i + 1) / count;
                region.polygon = {Vec2d(left, 0), Vec2d(right, 0), Vec2d(right, 1), Vec2d(left, 1)};
                facet.regions.push_back(std::move(region));
            }
        }
        measure("fragment_density", mesh.its.indices.size(), count,
            [&] { return build_exact_tracery_partition(mesh, annotations, regions); },
            [&](const auto& result) {
                verify_partition(result, 64. * 64.);
                REQUIRE(result.annotations.front().used_states[3]);
            });
    }
}

TEST_CASE("Profile whole-project preparation with objects on other plates", "[TraceryPerformance]")
{
    const auto mesh = slab(64);
    for (int outside : {0, 4, 16}) {
        Model model;
        for (int i = 0; i <= outside; ++i) {
            auto* object = model.add_object("performance-grid", "", mesh);
            object->add_instance()->set_offset(Vec3d(300. * i, 0., 0.));
            object->config.set_key_value("extruder", new ConfigOptionInt(2));
            TriangleSelector paint(mesh);
            for (size_t facet = 0; facet < mesh.its.indices.size(); ++facet)
                paint.set_facet(int(facet), EnforcerBlockerType(2));
            object->volumes.front()->mmu_segmentation_facets.set(paint);
        }
        const auto identity = tracery_identity_material_map();
        auto conversion = identity;
        conversion[2] = EnforcerBlockerType(3);
        for (bool change : {false, true}) {
            measure(change ? "plate_conversion" : "plate_identity", mesh.its.indices.size(), outside,
                [&] { return prepare_tracery_plate_conversion(model, {{0, {0}}}, DynamicPrintConfig{}, identity,
                                                               change ? conversion : identity, 0); },
                [&](const auto& result) {
                    REQUIRE(result.model->objects.size() == model.objects.size());
                    REQUIRE(result.model->objects[result.target_objects.at(0)]->config.extruder() == (change ? 3 : 2));
                    for (int i = 1; i <= outside; ++i) {
                        REQUIRE(result.model->objects[i]->config.extruder() == 2);
                        REQUIRE(result.model->objects[i]->volumes.front()->mmu_segmentation_facets.get_data() ==
                                model.objects[i]->volumes.front()->mmu_segmentation_facets.get_data());
                    }
                    const auto& paint = result.model->objects[result.target_objects.at(0)]->volumes.front()->mmu_segmentation_facets;
                    REQUIRE(paint.get_data().used_states[change ? 3 : 2]);
                    REQUIRE(model.objects[0]->config.extruder() == 2);
                    REQUIRE(model.objects[0]->volumes.front()->mmu_segmentation_facets.get_data().used_states[2]);
                });
        }
    }
}

TEST_CASE("Profile sequential flower partitions on the previous stroke output", "[TraceryPerformance]")
{
    auto mesh = grid(32);
    std::vector<TriangleSelector::TriangleSplittingData> annotations(paint_channels);
    for (int index = 0; index < 12; ++index) {
        const auto regions = stroke(mesh, index);
        REQUIRE(regions.size() == 2);
        measure("sequential_partition", mesh.its.indices.size(), index + 1,
            [&] { return build_exact_tracery_partition(mesh, annotations, regions); },
            [&](const auto& result) { verify_partition(result, 32. * 32.); });
        // Advance once, outside timed repetitions. The next sample retains all
        // prior cuts and serialized paint, like successive completed strokes.
        auto next = build_exact_tracery_partition(mesh, annotations, regions);
        mesh = TriangleMesh(std::move(next.mesh));
        annotations = std::move(next.annotations);
    }
}

TEST_CASE("Profile flower weight search using the production colour scorer", "[TraceryPerformance]")
{
    for (auto mode : {TraceryMaterialMode::CMY, TraceryMaterialMode::CMYW, TraceryMaterialMode::CMYK}) {
        std::vector<wxColour> palette{wxColour(0, 255, 255), wxColour(255, 0, 255), wxColour(255, 255, 0)};
        if (mode != TraceryMaterialMode::CMY)
            palette.emplace_back(mode == TraceryMaterialMode::CMYW ? wxColour(255, 255, 255) : wxColour(0, 0, 0));
        const std::string name = mode == TraceryMaterialMode::CMY ? "CMY" : mode == TraceryMaterialMode::CMYW ? "CMYW" : "CMYK";
        for (int targets : {1, 24}) {
            using Results = std::vector<std::array<int, 4>>;
            size_t evaluations = 0;
            measure("weight_search_" + name, 0, targets, [&] {
                Results result;
                evaluations = 0;
                for (int i = 0; i < targets; ++i) {
                    const wxColour target(190 + i % 50, 45 + (i * 13) % 170, 70 + (i * 17) % 160);
                    result.push_back(match_tracery_weights(mode, [&](const auto& weights) {
                        ++evaluations;
                        // Same callback as build_tracery_recipe, including the
                        // vector allocation, polynomial blend and DeltaE00.
                        const std::vector<double> mix(weights.begin(), weights.begin() + palette.size());
                        return GUI::color_delta_e00(target, GUI::blend_multi_filament_mixer(palette, mix));
                    }));
                }
                return result;
            }, [&](const Results& result) {
                REQUIRE(result.size() == size_t(targets));
                for (const auto& weights : result)
                    REQUIRE(legal_tracery_weights(weights, mode));
            });
            std::cout << "TRACERY_WORK scenario=weight_search_" << name << " targets=" << targets
                      << " score_evaluations=" << evaluations << '\n';
        }
    }
}

TEST_CASE("Profile the saved cathedral fixture with existing paint", "[TraceryPerformance]")
{
    Model model;
    DynamicPrintConfig config;
    ConfigSubstitutionContext context{ForwardCompatibilitySubstitutionRule::Disable};
    const auto path = boost::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
                      "test file/cathedral-stained-glass-window.3mf";
    PlateDataPtrs plates;
    std::vector<Preset*> presets;
    struct Cleanup {
        Model& model;
        PlateDataPtrs& plates;
        std::vector<Preset*>& presets;
        ~Cleanup() {
            model.remove_backup_path_if_exist();
            for (auto* plate : plates) delete plate;
            for (auto* preset : presets) delete preset;
        }
    } cleanup{model, plates, presets};
    model.set_backup_path((boost::filesystem::temp_directory_path() /
                          boost::filesystem::unique_path("tracery-perf-%%%%-%%%%")).string());
    bool is_bbs = false;
    Semver version;
    REQUIRE(load_bbs_3mf(path.string().c_str(), &config, &context, &model, &plates, &presets, &is_bbs, &version,
                         nullptr, LoadStrategy::LoadModel));
    const ModelVolume* selected = nullptr;
    for (const auto* object : model.objects)
        for (const auto* volume : object->volumes)
            if (volume->is_model_part() && volume->name == "part_22") selected = volume;
    REQUIRE(selected != nullptr);
    const auto& mesh = selected->mesh();
    std::vector<TriangleSelector::TriangleSplittingData> annotations{
        selected->mmu_segmentation_facets.get_data(), selected->supported_facets.get_data(), selected->seam_facets.get_data(),
        selected->fuzzy_skin_facets.get_data(), selected->exterior_facets.get_data(), selected->mmu_segmentation_facets.get_data(),
        {}, {}};
    TriangleSelector::Tracery2DData regions;
    double area = 0.;
    for (size_t i = 0; i < mesh.its.indices.size(); ++i) {
        const auto& t = mesh.its.indices[i];
        const Vec3d a = mesh.its.vertices[t[0]].cast<double>();
        const Vec3d b = mesh.its.vertices[t[1]].cast<double>() - a;
        const Vec3d c = mesh.its.vertices[t[2]].cast<double>() - a;
        const Vec3d normal = b.cross(c);
        area += normal.norm() * 0.5;
        if (normal.normalized().z() < 0.5) continue;
        TriangleSelector::TraceryFacet2DData facet;
        facet.source_facet_id = int(i);
        facet.origin = a;
        facet.axis_u = b.normalized();
        facet.axis_v = normal.normalized().cross(facet.axis_u);
        TriangleSelector::TraceryRegion2D region;
        region.color = EnforcerBlockerType(2);
        region.polygon = {Vec2d::Zero(), Vec2d(b.norm() * 0.371, 0),
                          Vec2d(c.dot(facet.axis_u) * 0.371, c.dot(facet.axis_v) * 0.371)};
        facet.regions.push_back(region);
        regions.push_back(std::move(facet));
    }
    REQUIRE_FALSE(regions.empty());
    measure("cathedral_partition", mesh.its.indices.size(), regions.size(),
        [&] { return build_exact_tracery_partition(mesh, annotations, regions); },
        [&](const auto& result) { verify_partition(result, area); });
}
