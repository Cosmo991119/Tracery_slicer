#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TraceryDocument.hpp"

#include <boost/filesystem.hpp>
#include <map>

using namespace Slic3r;

namespace {
void load_window(Model& model)
{
    DynamicPrintConfig        config;
    ConfigSubstitutionContext context{ForwardCompatibilitySubstitutionRule::Disable};
    const auto                path = boost::filesystem::path(__FILE__).parent_path().parent_path() / "data/tracery/window-stable-base.3mf";
    REQUIRE(load_3mf(path.string().c_str(), config, context, &model, false));
    REQUIRE(model.objects.size() == 1);
    REQUIRE(model.objects.front()->volumes.size() == 1);
    REQUIRE(model.objects.front()->volumes.front()->mesh().its.indices.size() == 16856);
}

void require_closed_partition(const TraceryPartitionResult& result)
{
    REQUIRE(result.source_facets.size() == result.mesh.indices.size());
    std::map<std::pair<int, int>, unsigned> edges;
    for (const auto& triangle : result.mesh.indices) {
        const Vec3d a = result.mesh.vertices[triangle[0]].cast<double>();
        const Vec3d b = result.mesh.vertices[triangle[1]].cast<double>();
        const Vec3d c = result.mesh.vertices[triangle[2]].cast<double>();
        REQUIRE((b - a).cross(c - a).squaredNorm() > 0.);
        for (int edge = 0; edge < 3; ++edge)
            ++edges[std::minmax(triangle[edge], triangle[(edge + 1) % 3])];
    }
    for (const auto& [edge, owners] : edges)
        REQUIRE(owners == 2);
}

TriangleSelector::Tracery2DData brush_cells(const TriangleMesh& mesh, const TriangleSelector& mask, const Vec3f& center)
{
    std::map<int, TriangleSelector::TraceryFacet2DData> grouped;
    // Two Voronoi cells share the vertical bisector through the brush centre.
    for (const auto& leaf : mask.painted_leaves()) {
        for (int half = 0; half < 2; ++half) {
            std::vector<Vec3d> clipped;
            for (size_t i = 0; i < leaf.vertices.size(); ++i) {
                const Vec3d  a  = leaf.vertices[i].cast<double>();
                const Vec3d  b  = leaf.vertices[(i + 1) % leaf.vertices.size()].cast<double>();
                const double da = (a.x() - center.x()) * (half ? -1 : 1);
                const double db = (b.x() - center.x()) * (half ? -1 : 1);
                if (da <= 0.)
                    clipped.push_back(a);
                if ((da < 0. && db > 0.) || (da > 0. && db < 0.))
                    clipped.push_back(a + da / (da - db) * (b - a));
            }
            if (clipped.size() < 3)
                continue;
            auto& facet           = grouped[leaf.source_facet];
            facet.source_facet_id = leaf.source_facet;
            const auto& original  = mesh.its.indices[leaf.source_facet];
            facet.origin          = mesh.its.vertices[original[0]].cast<double>();
            const Vec3d ab        = mesh.its.vertices[original[1]].cast<double>() - facet.origin;
            const Vec3d ac        = mesh.its.vertices[original[2]].cast<double>() - facet.origin;
            facet.axis_u          = ab.normalized();
            facet.axis_v          = ab.cross(ac).normalized().cross(facet.axis_u);
            TriangleSelector::TraceryRegion2D region;
            region.fragment_id = half;
            region.color       = EnforcerBlockerType(2 + half);
            for (const auto& vertex : clipped) {
                const Vec3d delta = vertex - facet.origin;
                region.polygon.emplace_back(delta.dot(facet.axis_u), delta.dot(facet.axis_v));
            }
            facet.regions.push_back(std::move(region));
        }
    }
    TriangleSelector::Tracery2DData regions;
    for (auto& [id, facet] : grouped)
        regions.push_back(std::move(facet));
    return regions;
}
} // namespace

TEST_CASE("Window brush painting survives rebuilding, reopening and erasing", "[TraceryDocument][WindowBrush]")
{
    Model model;
    load_window(model);
    const auto& volume = *model.objects.front()->volumes.front();
    const auto& mesh   = volume.mesh();
    for (float radius : {1.f, 3.f, 8.f}) {
        CAPTURE(radius);
        TriangleSelector mask(mesh);
        const auto&      triangle = mesh.its.indices[4044];
        const Vec3f      center = (mesh.its.vertices[triangle[0]] + mesh.its.vertices[triangle[1]] + mesh.its.vertices[triangle[2]]) / 3.f;
        auto             cursor = TriangleSelector::SinglePointCursor::cursor_factory(center, center + Vec3f(0, 0, 100), radius,
                                                                                      TriangleSelector::CursorType::CIRCLE, Transform3d::Identity(),
                                                                                      TriangleSelector::ClippingPlane());
        mask.select_patch(4044, std::move(cursor), EnforcerBlockerType(1), Transform3d::Identity(), true);
        const auto                               regions = brush_cells(mesh, mask, center);
        const auto                               current = volume.mmu_segmentation_facets.get_data();
        std::vector<TraceryDocument::Annotation> annotations{current,
                                                             volume.supported_facets.get_data(),
                                                             volume.seam_facets.get_data(),
                                                             volume.fuzzy_skin_facets.get_data(),
                                                             volume.exterior_facets.get_data(),
                                                             current,
                                                             {},
                                                             {}};
        auto                                     document = TraceryDocument::capture(mesh, annotations);
        annotations[6]                                    = mask.serialize();
        const auto result                                 = document.rebuild(mesh, annotations, regions, TraceryEdit::Paint);
        document                                          = TraceryDocument::decode(document.encode());
        const auto repeated = document.rebuild(TriangleMesh(result.mesh), result.annotations, {}, TraceryEdit::Paint);
        REQUIRE(result.mesh.vertices == repeated.mesh.vertices);
        REQUIRE(result.mesh.indices == repeated.mesh.indices);
        auto erased         = result.annotations;
        erased[6]           = {};
        const auto restored = document.rebuild(TriangleMesh(result.mesh), erased, {}, TraceryEdit::Erase);
        REQUIRE(document.base_mesh().its.indices == mesh.its.indices);
        REQUIRE(document.base_mesh().its.vertices == mesh.its.vertices);
        require_closed_partition(result);
        require_closed_partition(restored);
    }
}

TEST_CASE("Dense window partitions preserve closed mesh topology", "[TraceryDocument][WindowDense]")
{
    Model model;
    load_window(model);
    const auto&                              volume = *model.objects.front()->volumes.front();
    const auto&                              mesh   = volume.mesh();
    std::vector<TraceryDocument::Annotation> annotations(8);
    annotations[0] = annotations[5] = volume.mmu_segmentation_facets.get_data();
    TriangleSelector::Tracery2DData regions;
    // Cut every upward-facing facet, including the sub-micron source slivers.
    // This stresses cross-facet float rounding far beyond a single brush stroke.
    for (size_t i = 0; i < mesh.its.indices.size(); ++i) {
        const auto& triangle = mesh.its.indices[i];
        const Vec3d a        = mesh.its.vertices[triangle[0]].cast<double>();
        const Vec3d b        = mesh.its.vertices[triangle[1]].cast<double>() - a;
        const Vec3d c        = mesh.its.vertices[triangle[2]].cast<double>() - a;
        const Vec3d normal   = b.cross(c);
        REQUIRE(normal.squaredNorm() > 0.);
        if (normal.z() <= 0.)
            continue;
        TriangleSelector::TraceryFacet2DData facet;
        facet.source_facet_id = int(i);
        facet.origin          = a;
        facet.axis_u          = b.normalized();
        facet.axis_v          = normal.normalized().cross(facet.axis_u);
        TriangleSelector::TraceryRegion2D region;
        region.color   = EnforcerBlockerType(2);
        region.polygon = {Vec2d::Zero(), Vec2d(b.norm() * .371, 0), Vec2d(c.dot(facet.axis_u) * .371, c.dot(facet.axis_v) * .371)};
        facet.regions.push_back(std::move(region));
        regions.push_back(std::move(facet));
    }
    auto       document = TraceryDocument::capture(mesh, annotations);
    const auto result   = document.rebuild(mesh, annotations, regions, TraceryEdit::Paint);
    require_closed_partition(result);
}
