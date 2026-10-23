#include "GLGizmoTracery.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/MixedColorMatchHelpers.hpp"
#include "slic3r/GUI/MixedFilamentColorMapPanel.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/TraceryMaterialPlan.hpp"
#include "libslic3r/TraceryDocument.hpp"
#include "libslic3r/TraceryBasePaint.hpp"
#include "libslic3r/TraceryDisjointSet.hpp"
#include "libslic3r/TraceryPreviousFacetIndex.hpp"
#include "libslic3r/TraceryPerformance.hpp"
#include "libslic3r/TraceryRegionMapping.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include "libslic3r/AABBMesh.hpp"
#include "libslic3r/KDTreeIndirect.hpp"
#include "libslic3r/Utils.hpp"

#include <cereal/types/map.hpp>

#include <GL/glew.h>
#include <wx/image.h>
#include <wx/stdpaths.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace {

using Slic3r::TraceryPerformanceScope;

// Opt-in diagnostic: written to stderr alongside the performance trace.
bool tracery_color_trace_enabled()
{
    const char* value = std::getenv("TRACERY_COLOR_TRACE");
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

unsigned tracery_color_rgb(const Slic3r::ColorRGBA& color)
{
    const auto component = [](float v) { return unsigned(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return (component(color.r()) << 16) | (component(color.g()) << 8) | component(color.b());
}

#ifndef NDEBUG
struct TraceryDisplayCanvasProbe {
    std::filesystem::path directory;
    std::string phase;
    std::map<std::uintptr_t, std::string> last_canvas_states;
    std::set<std::string> captured_frames;
    bool active = false;
};

TraceryDisplayCanvasProbe display_canvas_probe;

void set_display_canvas_probe_phase(const std::string& phase)
{
    if (!display_canvas_probe.active)
        return;
    display_canvas_probe.phase = phase;
    display_canvas_probe.last_canvas_states.clear();
}

void write_display_canvas_probe_event(const char* event, const std::string& detail)
{
    if (!display_canvas_probe.active)
        return;
    std::ofstream out(display_canvas_probe.directory / "events.tsv", std::ios::app | std::ios::binary);
    if (!out)
        return;
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    out << milliseconds << '\t' << display_canvas_probe.phase << '\t' << event
        << "\tdetail=" << detail << '\n';
}
#endif

// A stable pseudo-random value makes fragments repeatable until the user
// explicitly regenerates them. This is intentionally not tied to model data.
float tracery_noise(size_t value)
{
    value ^= value >> 17;
    value *= size_t(0xed5ad4bbU);
    value ^= value >> 11;
    value *= size_t(0xac4c1b51U);
    value ^= value >> 15;
    return float(value & size_t(0xffff)) / 65535.0f;
}

wxColour tracery_wx_color(const Slic3r::ColorRGBA& color)
{
    return wxColour(int(std::lround(std::clamp(color.r(), 0.0f, 1.0f) * 255.0f)),
        int(std::lround(std::clamp(color.g(), 0.0f, 1.0f) * 255.0f)),
        int(std::lround(std::clamp(color.b(), 0.0f, 1.0f) * 255.0f)));
}

std::string tracery_hex(const wxColour& color)
{
    return color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
}

void update_tracery_region_bounds(Slic3r::TriangleSelector::TraceryRegion2D& region)
{
    if (region.polygon.empty()) {
        region.has_bounds = false;
        return;
    }
    region.bounds_min = region.polygon.front();
    region.bounds_max = region.bounds_min;
    for (const Slic3r::Vec2d& point : region.polygon) {
        region.bounds_min = region.bounds_min.cwiseMin(point);
        region.bounds_max = region.bounds_max.cwiseMax(point);
    }
    region.has_bounds = true;
}

float hue_channel(float first, float second, float hue)
{
    if (hue < 0.0f) hue += 1.0f;
    if (hue > 1.0f) hue -= 1.0f;
    if (6.0f * hue < 1.0f) return first + (second - first) * 6.0f * hue;
    if (2.0f * hue < 1.0f) return second;
    if (3.0f * hue < 2.0f) return first + (second - first) * (2.0f / 3.0f - hue) * 6.0f;
    return first;
}

Slic3r::ColorRGBA analogous_color(const Slic3r::ColorRGBA& base, size_t fragment_id)
{
    const float high = std::max({ base.r(), base.g(), base.b() });
    const float low = std::min({ base.r(), base.g(), base.b() });
    const float light = (high + low) * 0.5f;
    const float chroma = high - low;
    if (chroma < 0.08f) {
        // Neutral regions stay restrained, but become recognisably warm,
        // blue-grey or violet-grey rather than merely lighter and darker.
        const float tint = (tracery_noise(fragment_id + 41) - 0.5f) * 0.18f;
        return Slic3r::ColorRGBA(std::clamp(light + tint, 0.0f, 1.0f),
            std::clamp(light + tint * 0.20f, 0.0f, 1.0f),
            std::clamp(light - tint, 0.0f, 1.0f), 1.0f);
    }

    float hue = 0.0f;
    if (high == base.r()) hue = std::fmod((base.g() - base.b()) / chroma, 6.0f);
    else if (high == base.g()) hue = (base.b() - base.r()) / chroma + 2.0f;
    else hue = (base.r() - base.g()) / chroma + 4.0f;
    hue /= 6.0f;
    if (hue < 0.0f) hue += 1.0f;

    const float saturation = chroma / std::max(0.001f, 1.0f - std::abs(2.0f * light - 1.0f));
    // Keep fragments recognisably inside the source region's main hue. The
    // previous +/-29 degree hue excursion made a painted red region drift
    // toward unrelated purple and orange fragments before CMYW matching.
    const float shifted_hue = hue + (tracery_noise(fragment_id + 43) - 0.5f) * 0.055f;
    const float shifted_saturation = std::clamp(saturation + (tracery_noise(fragment_id + 67) - 0.5f) * 0.14f, 0.0f, 1.0f);
    const float shifted_light = std::clamp(light + (tracery_noise(fragment_id + 89) - 0.5f) * 0.22f, 0.05f, 0.95f);
    const float second = shifted_light * (1.0f + shifted_saturation * (1.0f - std::abs(2.0f * shifted_light - 1.0f)));
    const float first = 2.0f * shifted_light - second;
    return Slic3r::ColorRGBA(std::clamp(hue_channel(first, second, shifted_hue + 1.0f / 3.0f), 0.0f, 1.0f),
        std::clamp(hue_channel(first, second, shifted_hue), 0.0f, 1.0f),
        std::clamp(hue_channel(first, second, shifted_hue - 1.0f / 3.0f), 0.0f, 1.0f), 1.0f);
}

Slic3r::ColorRGBA glass_color(const Slic3r::ColorRGBA& base, const Slic3r::ColorRGBA& boundary_color,
    float boundary_mix, size_t fragment_id)
{
    const Slic3r::ColorRGBA local = analogous_color(base, fragment_id);
    const Slic3r::ColorRGBA neighboring = analogous_color(boundary_color, fragment_id + 151);
    return Slic3r::ColorRGBA(local.r() * (1.0f - boundary_mix) + neighboring.r() * boundary_mix,
        local.g() * (1.0f - boundary_mix) + neighboring.g() * boundary_mix,
        local.b() * (1.0f - boundary_mix) + neighboring.b() * boundary_mix, 1.0f);
}

Slic3r::GUI::MixedColorMatchRecipeResult build_tracery_recipe(
    const std::vector<std::string>& physical_colours, const wxColour& target, Slic3r::TraceryMaterialMode mode)
{
    using namespace Slic3r;
    Slic3r::GUI::MixedColorMatchRecipeResult result;
    const size_t count = mode == TraceryMaterialMode::CMY ? 3 : 4;
    if (!target.IsOk() || physical_colours.size() != count)
        return result;
    std::vector<wxColour> palette;
    for (const auto& value : physical_colours) {
        const wxColour colour(value);
        if (!colour.IsOk())
            return result;
        palette.push_back(colour);
    }
    const auto weights = match_tracery_weights(mode, [&](const std::array<int, 4>& w) {
        const std::vector<double> mix(w.begin(), w.begin() + count);
        return GUI::color_delta_e00(target, GUI::blend_multi_filament_mixer(palette, mix));
    });
    std::vector<unsigned int> ids;
    std::string encoded_weights;
    for (size_t i = 0; i < count; ++i) {
        if (weights[i] == 0)
            continue;
        ids.push_back(unsigned(i + 1));
        if (!encoded_weights.empty())
            encoded_weights += "/";
        encoded_weights += std::to_string(weights[i]);
    }
    if (ids.empty())
        return result;
    result.valid = true;
    result.component_a = ids.front();
    result.component_b = ids.size() > 1 ? ids[1] : (ids.front() == 1 ? 2u : 1u);
    const int pair_total = weights[result.component_a - 1] + weights[result.component_b - 1];
    result.mix_b_percent = ids.size() > 1 ? int(std::lround(100.0 * weights[result.component_b - 1] / pair_total)) : 0;
    result.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids(ids);
    result.gradient_component_weights = encoded_weights;
    result.preview_color = GUI::blend_multi_filament_mixer(palette, std::vector<double>(weights.begin(), weights.begin() + count));
    result.delta_e = GUI::color_delta_e00(target, result.preview_color);
    return result;
}

// Use the same ingredient expansion for head budgeting and recipe reuse. Zero
// weights in old four-channel rows must not reserve otherwise unused heads.
std::vector<unsigned int> tracery_recipe_ingredients(const Slic3r::MixedFilament& row, size_t physical_count)
{
    using namespace Slic3r;
    const auto ids = MixedFilamentManager::decode_gradient_component_ids(row.gradient_component_ids, 0);
    std::vector<unsigned int> result;
    if (!ids.empty()) {
        std::string text = row.gradient_component_weights;
        std::replace(text.begin(), text.end(), '/', ' ');
        std::istringstream input(text);
        std::vector<int> weights;
        int value;
        while (input >> value)
            weights.push_back(value);
        for (size_t i = 0; i < ids.size(); ++i)
            if (weights.size() != ids.size() || weights[i] > 0)
                result.push_back(ids[i]);
    } else {
        if (row.mix_b_percent < 100 || row.gradient_enabled || !row.manual_pattern.empty())
            result.push_back(row.component_a);
        if (row.mix_b_percent > 0 || row.gradient_enabled || !row.manual_pattern.empty())
            result.push_back(row.component_b);
    }
    if (std::any_of(result.begin(), result.end(), [physical_count](unsigned int id) { return id == 0 || id > physical_count; }))
        return {0};
    return result;
}

std::vector<unsigned int> tracery_plate_materials(
    const std::map<const Slic3r::ModelVolume*, std::vector<unsigned int>>& retained_paint = {})
{
    using namespace Slic3r;
    using namespace Slic3r::GUI;
    auto* plater = wxGetApp().plater();
    auto* bundle = wxGetApp().preset_bundle;
    auto* plate = plater->get_partplate_list().get_curr_plate();
    const size_t count = bundle->project_config.option<ConfigOptionStrings>("filament_colour")->values.size();
    std::set<unsigned int> used;
    auto add = [&](unsigned int id) {
        if (id > 0 && id <= count)
            used.insert(id);
        else if (const auto* row = bundle->mixed_filaments.mixed_filament_from_id(id, count)) {
            const auto ingredients = tracery_recipe_ingredients(*row, count);
            if (ingredients.empty())
                used.insert(0);
            used.insert(ingredients.begin(), ingredients.end());
        } else
            used.insert(0); // Unresolvable paint must fail the head-budget check.
    };
    const auto& objects = plater->model().objects;
    for (size_t oi = 0; oi < objects.size(); ++oi) {
        const auto* object = objects[oi];
        if (!object->printable)
            continue;
        bool on_plate = false;
        for (size_t ii = 0; ii < object->instances.size(); ++ii)
            on_plate |= object->instances[ii]->printable && plate->contain_instance(int(oi), int(ii));
        if (!on_plate)
            continue;
        for (const auto* volume : object->volumes) {
            if (!volume->is_model_part() && !volume->is_modifier())
                continue;
            // The underlying material may still be used in the object's core.
            if (volume->is_model_part() || volume->config.has("extruder"))
                add(unsigned(std::max(1, volume->extruder_id())));
            for (const char* key : {"wall_filament", "solid_infill_filament", "sparse_infill_filament"})
                if (const auto* value = volume->config.option(key); value && value->getInt() > 0)
                    add(unsigned(value->getInt()));
            if (const auto retained = retained_paint.find(volume); retained != retained_paint.end()) {
                for (unsigned int id : retained->second)
                    add(id);
            } else {
                const auto& states = volume->mmu_segmentation_facets.get_data().used_states;
                for (size_t state = 1; state < states.size(); ++state)
                    if (states[state])
                        add(unsigned(state));
            }
        }
        DynamicPrintConfig inherited = bundle->prints.get_edited_preset().config;
        inherited.apply(*plate->config());
        auto option = [&](const char* key) -> const ConfigOption* {
            const auto* value = object->config.option(key);
            return value ? value : inherited.option(key);
        };
        for (const char* key : {"wall_filament", "solid_infill_filament", "sparse_infill_filament"}) {
            const auto* value = object->config.option(key);
            if (!value && object->config.extruder() == 0) value = inherited.option(key);
            if (value && value->getInt() > 0) add(unsigned(value->getInt()));
        }
        const auto* supports = option("enable_support");
        const auto* raft = option("raft_layers");
        if ((supports && supports->getBool()) || (raft && raft->getInt() > 0))
            for (const char* key : {"support_filament", "support_interface_filament"})
                if (const auto* value = option(key); value && value->getInt() > 0)
                    add(unsigned(value->getInt()));
        for (const auto& range : object->layer_config_ranges)
            for (const char* key : {"extruder", "wall_filament", "solid_infill_filament", "sparse_infill_filament"})
                if (const auto* value = range.second.option(key); value && value->getInt() > 0)
                    add(unsigned(value->getInt()));

    }
    if (const auto codes = plater->model().plates_custom_gcodes.find(plate->get_index());
        codes != plater->model().plates_custom_gcodes.end())
        for (const auto& item : codes->second.gcodes)
            if (item.type == CustomGCode::Type::ToolChange && item.extruder > 0)
                add(unsigned(item.extruder));
    return {used.begin(), used.end()};
}

struct TraceryEditorRecipe {
    std::array<int, 4> weights = { 0, 0, 0, 0 };
    wxColour            preview;
};

std::vector<TraceryEditorRecipe> build_tracery_editor_palette()
{
    using namespace Slic3r;
    using namespace Slic3r::GUI;
    const auto* bundle = wxGetApp().preset_bundle;
    if (!bundle)
        return {};
    const auto used_black = tracery_black_materials(*bundle, tracery_plate_materials());
    const unsigned int black_id = used_black.empty() ? 0 : used_black.front();
    const auto mode = tracery_plate_material_mode(*bundle, black_id);
    std::vector<wxColour> colours{wxColour("#08ABFB"), wxColour("#D93B90"), wxColour("#F9ED3D")};
    if (mode == TraceryMaterialMode::CMYW)
        colours.emplace_back("#FFFFFF");
    else if (mode == TraceryMaterialMode::CMYK)
        colours.emplace_back(bundle->project_config.option<ConfigOptionStrings>("filament_colour")->values.at(black_id - 1));
    std::vector<TraceryEditorRecipe> palette;
    // Use the same material mode as final recipe generation, including
    // translucent black. Never match a CMYK source against a white-only palette.
    constexpr int step = 5;
    for (int c = 0; c <= 100; c += step)
        for (int m = 0; m <= 100 - c; m += step)
            for (int y = 0; y <= 100 - c - m; y += step) {
                const std::array<int, 4> weights = { c, m, y, 100 - c - m - y };
                if (!legal_tracery_weights(weights, mode))
                    continue;
                std::vector<double> mix_weights(weights.begin(), weights.begin() + colours.size());
                palette.push_back({ weights, Slic3r::GUI::blend_multi_filament_mixer(colours, mix_weights) });
            }
    return palette;
}

double tracery_recipe_balance_spread(const std::array<int, 4>& weights)
{
    int minimum = 101;
    int maximum = 0;
    for (const int weight : weights)
        if (weight > 0) {
            minimum = std::min(minimum, weight);
            maximum = std::max(maximum, weight);
        }
    // Compare only active materials: an absent component is not an imbalance.
    return minimum <= 0 ? 0.0 : double(maximum - minimum);
}

} // namespace

namespace Slic3r::GUI {

struct GLGizmoTracery::GenerationInput {
    struct SurfaceTriangle {
        size_t    mesh_idx;
        int       original_facet_id = -1;
        Vec3f     a;
        Vec3f     b;
        Vec3f     c;
        Vec3f     world_a;
        Vec3f     world_b;
        Vec3f     world_c;
        Vec3f     normal;
        ColorRGBA source_color;
    };
    struct ColorSource {
        ColorRGBA                   color;
        indexed_triangle_set        facets;
        std::unique_ptr<AABBMesh>   aabb;
    };

    const ModelObject*                                          model_object;
    std::vector<SurfaceTriangle>&                               surface_triangles;
    std::vector<std::vector<std::unique_ptr<ColorSource>>>&    color_sources;
    std::vector<Transform3d>&                                   volume_transforms;
    double&                                                     surface_area;
};

struct GLGizmoTracery::WorkTriangle {
    size_t    mesh_idx;
    int       original_facet_id = -1;
    Vec3f     a;
    Vec3f     b;
    Vec3f     c;
    Vec3f     world_a;
    Vec3f     world_b;
    Vec3f     world_c;
    Vec3f     normal;
    ColorRGBA source_color;
    size_t    color_group;
    float     area;
};

struct GLGizmoTracery::ColorGroup {
    size_t    component_id;
    ColorRGBA color;
};

struct GLGizmoTracery::VoronoiColorRegion {
    size_t              color_group = 0;
    std::vector<size_t> face_indices;
    double              world_area = 0.0;
    size_t              fragment_budget = 0;
};

struct GLGizmoTracery::ColorRegionIntegrity {
    size_t immutable_hash = 0;
    size_t output_hash = 0;
    size_t work_triangle_count = 0;
    size_t grouped_triangle_count = 0;
    size_t group_count = 0;
    double work_area = 0.0;
    double grouped_area = 0.0;
    bool   valid = false;
    bool   immutable_input_preserved = true;
};

struct GLGizmoTracery::FragmentSeed {
    Vec3f  position;
    size_t color_group;
    size_t stable_id;
};

struct GLGizmoTracery::CpuGeometry {
    indexed_triangle_set surface;
    // Parallel to surface.indices. A Voronoi polygon is triangulated into
    // several faces, all of which retain the source model facet.
    std::vector<int>     source_facet_ids;

    bool empty() const { return surface.indices.empty(); }
    void add_vertex(const Vec3f& vertex) { surface.vertices.emplace_back(vertex); }
    void add_triangle(unsigned int first, unsigned int second, unsigned int third, int source_facet_id)
    {
        surface.indices.emplace_back(stl_triangle_vertex_indices{int(first), int(second), int(third)});
        source_facet_ids.emplace_back(source_facet_id);
    }
};

struct GLGizmoTracery::FragmentGenerationIntegrity {
    size_t input_hash = 0;
    size_t output_hash = 0;
    size_t seed_count = 0;
    size_t assigned_triangle_count = 0;
    size_t nonempty_geometry_count = 0;
    bool   valid = false;
    bool   immutable_input_preserved = true;
};

struct GLGizmoTracery::FragmentColorAssignmentIntegrity {
    size_t input_hash = 0;
    size_t output_hash = 0;
    size_t color_count = 0;
    bool   valid = false;
    bool   immutable_input_preserved = true;
};

struct GLGizmoTracery::PublishGeneratedFragmentsIntegrity {
    size_t expected_hash = 0;
    size_t published_hash = 0;
    size_t expected_fragments = 0;
    size_t published_fragments = 0;
    size_t expected_faces = 0;
    size_t published_faces = 0;
    bool valid = false;
};

GLGizmoTracery::GLGizmoTracery(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoPainterBase(parent, icon_filename, sprite_id)
    , m_current_tool(ImGui::CircleButtonIcon)
{}

bool GLGizmoTracery::on_init()
{
    return true;
}

bool GLGizmoTracery::gizmo_event(SLAGizmoEventType action, const Vec2d& mouse_position, bool shift_down, bool alt_down, bool control_down)
{
    refresh_source_selection_policy();
    if (action == SLAGizmoEventType::LeftDown || action == SLAGizmoEventType::RightDown) {
        // Holding Shift has the same effect as clearing the selection in the
        // painter base. Remember the operation for the commit that runs on
        // mouse-up, when the base class has already applied the stroke.
        m_active_stroke_erases = shift_down ||
            (action == SLAGizmoEventType::LeftDown ? m_eraser_mode : !m_eraser_mode);
    }

    const bool handled = GLGizmoPainterBase::gizmo_event(action, mouse_position, shift_down, alt_down, control_down);

    if (action == SLAGizmoEventType::LeftUp || action == SLAGizmoEventType::RightUp)
        m_active_stroke_erases = false;

    return handled;
}

std::string GLGizmoTracery::on_get_name() const
{
    return _u8L("Stained glass");
}

void GLGizmoTracery::render_painter_gizmo()
{
    refresh_source_selection_policy();
    // This is an editing overlay only. The final stained-glass colours still
    // come exclusively from ordinary MMU paint; no generated fragment is
    // rendered here.
    const Selection& selection = m_parent.get_selection();
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));
    render_triangles(selection);
    m_c->object_clipper()->render_cut();
    m_c->instances_hider()->render_cut();
    render_cursor();
    glsafe(::glDisable(GL_BLEND));
}

EnforcerBlockerType GLGizmoTracery::get_paint_state(bool, bool) const
{
    // A private marker lets the common material selector report the exact
    // leaves touched by any tool without changing its material partition.
    return EnforcerBlockerType::ExtruderMax;
}

TriangleSelectorGUI* GLGizmoTracery::get_edit_selector(size_t mesh_idx)
{
    if (mesh_idx < m_fill_reference_selectors.size())
        return m_fill_reference_selectors[mesh_idx].get();
    return GLGizmoPainterBase::get_edit_selector(mesh_idx);
}

const TriangleSelectorGUI* GLGizmoTracery::get_edit_selector(size_t mesh_idx) const
{
    if (mesh_idx < m_fill_reference_selectors.size())
        return m_fill_reference_selectors[mesh_idx].get();
    return GLGizmoPainterBase::get_edit_selector(mesh_idx);
}

size_t GLGizmoTracery::get_edit_selector_count() const
{
    return m_fill_reference_selectors.size();
}

void GLGizmoTracery::apply_seed_fill_to_edit_selector(size_t mesh_idx, EnforcerBlockerType new_state)
{
    if (mesh_idx < m_fill_reference_selectors.size() && mesh_idx < m_triangle_selectors.size()) {
        // Preserve the material selector verbatim. Only its transient seed
        // selection is copied into the companion stained-glass flag layer.
        m_fill_reference_selectors[mesh_idx]->apply_seed_fill_to(*m_triangle_selectors[mesh_idx],
            m_active_stroke_erases ? EnforcerBlockerType::NONE : EnforcerBlockerType::ENFORCER);
        m_triangle_selectors[mesh_idx]->request_update_render_data(true);
        return;
    }
    GLGizmoPainterBase::apply_seed_fill_to_edit_selector(mesh_idx, new_state);
}

#ifndef NDEBUG
void GLGizmoTracery::start_display_diagnostics()
{
    try {
        m_display_diagnostics_dir = std::filesystem::path(wxStandardPaths::Get().GetDocumentsDir().ToStdWstring()) /
            "TraceryDiagnostics" /
            ("display-run-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(m_display_diagnostics_dir);
        std::ofstream readme(m_display_diagnostics_dir / "README.txt", std::ios::binary);
        readme << "Focused stained-glass diagnostics. Normal rendering is not changed.\n"
               << "events.tsv contains a bounded warp probe plus commit/bake records.\n"
               << "tracery_voronoi_bake_volume_*.tsv follows generated Voronoi regions through 2D projection, selector overlap and persisted MMU paint.\n"
               << "No framebuffer capture or duplicate integrity pass is enabled; Voronoi bake logs at most eight uncovered selector leaves per source facet.\n"
               << "warp_probe_parameters records raw density and derived warp parameters.\n"
               << "warp_probe_seed_compute records scalar phases, sine values, offsets, and float bits.\n"
               << "warp_probe_seed_storage and warp_probe_seed_post_kd distinguish calculation errors from later memory corruption.\n";
        if (!readme)
            throw std::runtime_error("Cannot create display diagnostic README");
        m_display_diagnostics_phase = "editing";
        m_display_diagnostics_last_canvas_state.clear();
        m_display_diagnostics_captured.clear();
        m_display_diagnostics_active = true;
        // The focused probes below intentionally avoid framebuffer capture
        // and per-frame render tracing, which made diagnostic runs too slow.
        display_canvas_probe.active = false;
        m_parent.set_as_dirty();
        wxMessageBox(wxString(m_display_diagnostics_dir.wstring()), _L("Display diagnostics saved to"), wxOK | wxICON_INFORMATION);
    } catch (const std::exception& error) {
        m_display_diagnostics_active = false;
        display_canvas_probe.active = false;
        wxMessageBox(wxString::FromUTF8(error.what()), _L("Display diagnostics"), wxOK | wxICON_WARNING);
    }
}

void GLGizmoTracery::stop_display_diagnostics()
{
    m_display_diagnostics_active = false;
    display_canvas_probe.active = false;
}

bool GLGizmoTracery::global_display_diagnostics_active()
{
    return display_canvas_probe.active;
}

void GLGizmoTracery::record_global_display_diagnostic_canvas(const void* canvas, const std::string& detail)
{
    if (!display_canvas_probe.active)
        return;
    const auto canvas_id = reinterpret_cast<std::uintptr_t>(canvas);
    if (display_canvas_probe.last_canvas_states[canvas_id] == detail)
        return;
    display_canvas_probe.last_canvas_states[canvas_id] = detail;
    write_display_canvas_probe_event("global_canvas_route", "canvas=" + std::to_string(canvas_id) + "," + detail);
}

void GLGizmoTracery::capture_global_display_diagnostic_frame(const void* canvas, const char* stage)
{
    if (!display_canvas_probe.active ||
        (display_canvas_probe.phase != "after_exit" && display_canvas_probe.phase != "mix_update" &&
         display_canvas_probe.phase != "after_mix"))
        return;
    const std::string key = display_canvas_probe.phase + "_canvas_" +
        std::to_string(reinterpret_cast<std::uintptr_t>(canvas)) + '_' + stage;
    if (!display_canvas_probe.captured_frames.insert(key).second)
        return;

    GLint viewport[4] = {};
    GLint pack_alignment = 4;
    ::glGetIntegerv(GL_VIEWPORT, viewport);
    ::glGetIntegerv(GL_PACK_ALIGNMENT, &pack_alignment);
    const int width = viewport[2];
    const int height = viewport[3];
    if (width <= 0 || height <= 0) {
        write_display_canvas_probe_event("global_capture_skipped", key + ":empty_viewport");
        return;
    }

    std::vector<unsigned char> pixels(size_t(width) * size_t(height) * 3);
    ::glPixelStorei(GL_PACK_ALIGNMENT, 1);
    ::glReadPixels(viewport[0], viewport[1], width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    const GLenum read_error = ::glGetError();
    ::glPixelStorei(GL_PACK_ALIGNMENT, pack_alignment);
    if (read_error != GL_NO_ERROR) {
        write_display_canvas_probe_event("global_capture_failed", key + ":gl_error=" + std::to_string(read_error));
        return;
    }

    wxImage image(width, height);
    unsigned char* image_data = image.GetData();
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const size_t source = (size_t(height - y - 1) * size_t(width) + size_t(x)) * 3;
            const size_t target = (size_t(y) * size_t(width) + size_t(x)) * 3;
            for (int channel = 0; channel < 3; ++channel)
                image_data[target + size_t(channel)] = pixels[source + size_t(channel)];
        }
    const std::filesystem::path path = display_canvas_probe.directory / (key + ".png");
    write_display_canvas_probe_event(image.SaveFile(wxString(path.wstring()), wxBITMAP_TYPE_PNG) ?
        "global_capture_saved" : "global_capture_failed", key);
}

void GLGizmoTracery::record_display_diagnostic_state(const char* event, const std::string& detail)
{
    if (!m_display_diagnostics_active)
        return;
    std::ofstream out(m_display_diagnostics_dir / "events.tsv", std::ios::app | std::ios::binary);
    if (!out)
        return;

    const Model* model = m_parent.get_model();
    size_t fragments = 0;
    for (const auto& volume : m_generated_fragments)
        fragments += volume.size();
    std::string object_ids;
    if (model != nullptr) {
        for (size_t index = 0; index < model->objects.size(); ++index) {
            const ModelObject* object = model->objects[index];
            if (!object_ids.empty()) object_ids += ',';
            object_ids += std::to_string(object->id().id);
        }
    }
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    out << milliseconds << '\t' << m_display_diagnostics_phase << '\t' << event
        << "\tdetail=" << detail
        << "\tediting=" << (m_state == On)
        << "\tmask_id=" << m_mask_object_id.id
        << "\tmask_index=" << m_mask_object_index
        << "\tobject_ids=" << object_ids
        << "\tmask_entries=" << m_mask_data.size()
        << "\tlive_selectors=" << m_triangle_selectors.size()
        << "\tgenerated_volumes=" << m_generated_fragments.size()
        << "\tfragments=" << fragments
        << "\tfragment_colours=" << m_fragment_colors.size()
        << "\tmixed_ids=" << m_fragment_mixed_filament_ids.size() << '\n';
}

void GLGizmoTracery::record_display_diagnostic_canvas(int object_index, bool picking_enabled, bool stencil_available,
                                                        int matching_volumes, bool editing)
{
    if (!m_display_diagnostics_active)
        return;
    const std::string state = "object_index=" + std::to_string(object_index) +
        ",picking_enabled=" + std::to_string(picking_enabled) +
        ",stencil=" + std::to_string(stencil_available) +
        ",matching_volumes=" + std::to_string(matching_volumes) +
        ",editing=" + std::to_string(editing);
    if (state != m_display_diagnostics_last_canvas_state) {
        m_display_diagnostics_last_canvas_state = state;
        record_display_diagnostic_state("canvas_branch", state);
    }
}

void GLGizmoTracery::capture_display_diagnostic_frame(const char* stage, bool stencil_buffer)
{
    if (!m_display_diagnostics_active)
        return;
    const std::string key = m_display_diagnostics_phase + '_' + stage;
    if (!m_display_diagnostics_captured.insert(key).second)
        return;

    GLint viewport[4] = {};
    GLint pack_alignment = 4;
    ::glGetIntegerv(GL_VIEWPORT, viewport);
    ::glGetIntegerv(GL_PACK_ALIGNMENT, &pack_alignment);
    const int width = viewport[2];
    const int height = viewport[3];
    if (width <= 0 || height <= 0) {
        record_display_diagnostic_state("capture_skipped", key + ":empty_viewport");
        return;
    }

    const int channels = stencil_buffer ? 1 : 3;
    std::vector<unsigned char> pixels(size_t(width) * size_t(height) * size_t(channels));
    ::glPixelStorei(GL_PACK_ALIGNMENT, 1);
    ::glReadPixels(viewport[0], viewport[1], width, height,
                   stencil_buffer ? GL_STENCIL_INDEX : GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    const GLenum read_error = ::glGetError();
    ::glPixelStorei(GL_PACK_ALIGNMENT, pack_alignment);
    if (read_error != GL_NO_ERROR) {
        record_display_diagnostic_state("capture_failed", key + ":gl_error=" + std::to_string(read_error));
        return;
    }

    wxImage image(width, height);
    unsigned char* image_data = image.GetData();
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const size_t source = (size_t(height - y - 1) * size_t(width) + size_t(x)) * size_t(channels);
            const size_t target = (size_t(y) * size_t(width) + size_t(x)) * 3;
            for (int channel = 0; channel < 3; ++channel)
                image_data[target + size_t(channel)] = stencil_buffer ?
                    ((pixels[source] & 0x01U) == 0 ? 0 : 255) : pixels[source + size_t(channel)];
        }
    const std::filesystem::path path = m_display_diagnostics_dir / (key + ".png");
    record_display_diagnostic_state(image.SaveFile(wxString(path.wstring()), wxBITMAP_TYPE_PNG) ?
        "capture_saved" : "capture_failed", key);
}
#endif

void GLGizmoTracery::on_render_input_window(float x, float y, float bottom_limit)
{
    if (!m_c->selection_info()->model_object())
        return;

    y = std::min(y, bottom_limit - m_imgui->scaled(22.f));
#if BBS_TOOLBAR_ON_TOP
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
#else
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 1.0f, 0.0f);
#endif

    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    const auto push_tool_style = [](bool selected) {
        const ImVec4 accent = ImGuiWrapper::COL_ORCA;
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(accent.x, accent.y, accent.z, selected ? 0.40f : 0.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(accent.x, accent.y, accent.z, 0.40f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(accent.x, accent.y, accent.z, 0.50f));
        ImGui::PushStyleColor(ImGuiCol_Border, selected ? accent : ImGui::GetStyleColorVec4(ImGuiCol_Border));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, selected ? 1.f : ImGui::GetStyle().FrameBorderSize);
    };
    const auto pop_tool_style = [] {
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
    };

    m_imgui->text(_L("Stained glass"));
    m_imgui->text_colored(ImGuiWrapper::COL_WINDOW_BG, _L("Stained glass is generated when you finish painting."));
    ImGui::Separator();
    m_imgui->text(_L("Painting mode"));
    const std::array<wchar_t, 2> mode_icons = m_is_dark_mode ? std::array<wchar_t, 2>{
        ImGui::StainedSurfaceModeDarkIcon, ImGui::StainedThroughModeDarkIcon
    } : std::array<wchar_t, 2>{
        ImGui::StainedSurfaceModeIcon, ImGui::StainedThroughModeIcon
    };
    const std::array<wxString, 2> mode_tips = {
        _L("Normal: paint the visible surface"), _L("Through: paint both sides of the model")
    };
    for (size_t i = 0; i < mode_icons.size(); ++i) {
        if (i != 0)
            ImGui::SameLine();
        const bool selected = m_paint_mode == (i == 0 ? PaintMode::SURFACE : PaintMode::THROUGH);
        push_tool_style(selected);
        if (ImGui::Button(into_u8(std::wstring(1, mode_icons[i])).c_str()))
            m_paint_mode = i == 0 ? PaintMode::SURFACE : PaintMode::THROUGH;
        pop_tool_style();
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(mode_tips[i], ImGui::GetFontSize() * 20.0f);
    }
    ImGui::Separator();
    m_imgui->text(_L("Operation"));
    const std::array<wchar_t, 2> operation_icons = m_is_dark_mode ? std::array<wchar_t, 2>{
        ImGui::PaintOperationDarkIcon, ImGui::EraseOperationDarkIcon
    } : std::array<wchar_t, 2>{
        ImGui::PaintOperationIcon, ImGui::EraseOperationIcon
    };
    const std::array<wxString, 2> operation_labels = { _L("Paint"), _L("Erase") };
    const std::array<wxString, 2> operation_tips = {
        _L("Paint stained-glass areas"), _L("Erase stained-glass areas")
    };
    for (size_t i = 0; i < operation_icons.size(); ++i) {
        if (i != 0)
            ImGui::SameLine(0.0f, m_imgui->scaled(1.5f));
        const std::string label = into_u8(std::wstring(1, operation_icons[i])) + "##tracery_operation_" + std::to_string(i);
        const bool selected = m_eraser_mode == (i == 1);
        push_tool_style(selected);
        if (ImGui::Button(label.c_str()))
            m_eraser_mode = i == 1;
        pop_tool_style();
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(operation_tips[i], ImGui::GetFontSize() * 20.0f);
        ImGui::SameLine();
        m_imgui->text(operation_labels[i]);
    }
    ImGui::Separator();
    m_imgui->text(_L("Tool type"));

    const std::array<wchar_t, 4> tool_ids = {
        ImGui::CircleButtonIcon, ImGui::SphereButtonIcon, ImGui::TriangleButtonIcon, ImGui::FillButtonIcon
    };
    const std::array<wchar_t, 4> display_icons = m_is_dark_mode ? std::array<wchar_t, 4>{
        ImGui::CircleButtonDarkIcon, ImGui::SphereButtonDarkIcon, ImGui::TriangleButtonDarkIcon, ImGui::FillButtonDarkIcon
    } : std::array<wchar_t, 4>{
        ImGui::CircleButtonIcon, ImGui::SphereButtonIcon, ImGui::TriangleButtonIcon, ImGui::FillButtonIcon
    };
    const std::array<wxString, 4> tool_tips = {
        _L("Surface brush"), _L("Volume brush"), _L("Face paint"), _L("Fill connected area")
    };
    for (size_t i = 0; i < tool_ids.size(); ++i) {
        if (i != 0)
            ImGui::SameLine();
        const std::wstring label(1, display_icons[i]);
        const bool selected = m_current_tool == tool_ids[i];
        push_tool_style(selected);
        if (ImGui::Button(into_u8(label).c_str()))
            m_current_tool = tool_ids[i];
        pop_tool_style();
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(tool_tips[i], ImGui::GetFontSize() * 20.0f);
    }

    if (m_current_tool == ImGui::CircleButtonIcon) {
        m_cursor_type = TriangleSelector::CursorType::CIRCLE;
        m_tool_type = ToolType::BRUSH;
    } else if (m_current_tool == ImGui::SphereButtonIcon) {
        m_cursor_type = TriangleSelector::CursorType::SPHERE;
        m_tool_type = ToolType::BRUSH;
    } else if (m_current_tool == ImGui::TriangleButtonIcon) {
        m_cursor_type = TriangleSelector::CursorType::POINTER;
        m_tool_type = ToolType::BRUSH;
    } else {
        m_cursor_type = TriangleSelector::CursorType::POINTER;
        m_tool_type = ToolType::BUCKET_FILL;
    }

    if (m_tool_type == ToolType::BRUSH && m_cursor_type != TriangleSelector::CursorType::POINTER) {
        ImGui::AlignTextToFramePadding();
        m_imgui->text(_L("Brush size"));
        ImGui::SameLine();
        ImGui::PushItemWidth(m_imgui->scaled(8.f));
        m_imgui->bbl_slider_float_style("##tracery_brush_size", &m_cursor_radius, CursorRadiusMin, CursorRadiusMax, "%.2f", 1.0f, true);
    }

    if (m_tool_type == ToolType::BUCKET_FILL) {
        const bool edge_detection_changed = m_imgui->bbl_checkbox(_L("Edge detection"), m_detect_geometry_edge);
        if (m_detect_geometry_edge) {
            if (edge_detection_changed && m_smart_fill_angle < SmartFillAngleMin)
                m_smart_fill_angle = 30.f;
            ImGui::AlignTextToFramePadding();
            m_imgui->text(_L("Smart fill angle"));
            ImGui::SameLine();
            ImGui::PushItemWidth(m_imgui->scaled(8.f));
            const bool angle_changed = m_imgui->bbl_slider_float_style("##tracery_smart_fill_angle", &m_smart_fill_angle,
                SmartFillAngleMin, SmartFillAngleMax, "%.0f", 1.0f, true);
            if (edge_detection_changed || angle_changed) {
                for (const auto& selector : m_fill_reference_selectors) {
                    selector->seed_fill_unselect_all_triangles();
                    selector->request_update_render_data();
                }
            }
        } else {
            m_smart_fill_angle = -1.f;
            if (edge_detection_changed) {
                for (const auto& selector : m_fill_reference_selectors) {
                    selector->seed_fill_unselect_all_triangles();
                    selector->request_update_render_data();
                }
            }
        }
    }

    ImGui::Separator();
    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Fragment density"));
    ImGui::SameLine();
    ImGui::PushItemWidth(m_imgui->scaled(8.f));
    m_imgui->bbl_slider_float_style("##tracery_density", &m_density_input, 0.f, 1.f, "%.0f%%", 1.0f, true);
    const bool density_edit_finished = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Refraction strength"));
    ImGui::SameLine();
    ImGui::PushItemWidth(m_imgui->scaled(8.f));
    m_imgui->bbl_slider_float_style("##tracery_refraction", &m_refraction_input, 0.f, 1.f, "%.0f%%", 1.0f, true);
    const bool refraction_edit_finished = ImGui::IsItemDeactivatedAfterEdit();
    if (density_edit_finished || refraction_edit_finished)
        regenerate_stained_glass(false);

    ImGui::Separator();
    if (m_imgui->button(_L("Randomize stained glass")))
        regenerate_stained_glass(true);
    ImGui::Separator();
#ifndef NDEBUG
    if (m_imgui->button(m_display_diagnostics_active ? "Stop display diagnostics" : "Start display diagnostics")) {
        if (m_display_diagnostics_active)
            stop_display_diagnostics();
        else
            start_display_diagnostics();
    }
#endif
    ImGui::Separator();
    if (m_imgui->button(_L("Clear stained glass"))) {
        erase_all_stained_glass();
    }
    ImGui::SameLine();
    m_imgui->text_colored(ImGuiWrapper::COL_WINDOW_BG,
        m_eraser_mode ? _L("Left mouse button: erase") : _L("Right mouse button: erase"));

    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();
}

void GLGizmoTracery::regenerate_stained_glass(bool randomize)
{
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Regenerate stained glass", UndoRedo::SnapshotType::GizmoAction);
    m_fragment_density = m_density_input;
    m_refraction_strength = m_refraction_input;
    const bool has_selection = std::any_of(m_triangle_selectors.begin(), m_triangle_selectors.end(),
        [](const auto& selector) { return selector->has_facets(EnforcerBlockerType::ENFORCER); });
    if (has_selection) {
        m_active_stroke_erases = false;
        m_regeneration_requested = true;
        if (randomize)
            ++m_regeneration_version;
        update_model_object();
    }
    m_parent.set_as_dirty();
}

void GLGizmoTracery::erase_all_stained_glass()
{
    TraceryPerformanceScope performance("erase_all_stained_glass");
    ModelObject* object = m_c->selection_info()->model_object();
    bool mesh_changed = false;
    if (object == nullptr || !restore_original_tracery_meshes(*object, mesh_changed))
        return;
    performance.stage("clear.refresh_editor");
    clear_stained_glass_editor();
    update_from_model_object(false);
    if (mesh_changed)
        invalidate_painting_raycast();
    m_seed_fill_last_mesh_id = -1;
    performance.stage("clear.notify");
    notify_mmu_paint_changed(*object);
    performance.stage("clear.geometry_refresh");
    if (mesh_changed) {
        const auto& objects = wxGetApp().model().objects;
        const auto  found   = std::find(objects.begin(), objects.end(), object);
        if (found != objects.end())
            wxGetApp().plater()->changed_mesh(int(found - objects.begin()));
    }
    performance.stage("clear.mark_dirty");
    m_parent.set_as_dirty();
}

bool GLGizmoTracery::restore_original_tracery_meshes(ModelObject& object, bool& mesh_changed)
{
    TraceryPerformanceScope performance("clear_all_stained_glass");
    std::vector<std::pair<ModelVolume*, TraceryPartitionResult>> restored;
    std::vector<std::pair<ModelVolume*, std::string>> vector_clears;
    std::vector<std::pair<ModelVolume*, TriangleSelector::TriangleSplittingData>> paint_restores;
    try {
        performance.stage("clear.prepare");
        size_t index = 0;
        for (ModelVolume* volume : object.volumes) {
            if (!volume->is_model_part())
                continue;
            const auto snapshot    = m_mesh_restore_states.find(volume->id());
            auto annotations = [&] {
                TraceryPerformanceScope::Sample sample(performance, "clear.collect_annotations");
                return collect_volume_annotations(*volume);
            }();
            if (!volume->tracery_edit_data.empty()) {
                auto document = [&] {
                    TraceryPerformanceScope::Sample sample(performance, "clear.decode_document");
                    return TraceryDocument::decode(volume->tracery_edit_data);
                }();
                auto channels = document.last_annotations();
                std::copy(annotations.begin(), annotations.end(), channels.begin());
                channels[6] = {};
                if (!document.is_vector())
                    throw std::runtime_error("Legacy stained-glass mesh editing is disabled. Open an unmodified model to use vector stained glass.");
                {
                    TraceryPerformanceScope::Sample sample(performance, "clear.clear_document");
                    document.clear_vector_glass(volume->mesh(), channels);
                }
                auto data = [&] {
                    TraceryPerformanceScope::Sample sample(performance, "clear.encode_document");
                    return document.encode();
                }();
                vector_clears.emplace_back(volume, std::move(data));
            } else if (snapshot != m_mesh_restore_states.end()) {
                restored.emplace_back(volume, restore_tracery_mesh(snapshot->second, volume->mesh(), annotations));
            } else if (index < m_stained_glass_base_paint_data.size()) {
                // Legacy/session paint without a mesh snapshot can only restore colour.
                if (annotations[0] != m_stained_glass_base_paint_data[index])
                    paint_restores.emplace_back(volume, m_stained_glass_base_paint_data[index]);
            }
            ++index;
        }
    } catch (const std::exception& error) {
        wxMessageBox(from_u8(error.what()), _L("Stained glass"), wxOK | wxICON_ERROR);
        return false;
    }
    performance.stage("clear.snapshot");
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Erase all stained glass", UndoRedo::SnapshotType::GizmoAction);
    performance.stage("clear.commit");
    for (auto& [volume, data] : vector_clears)
        volume->tracery_edit_data = std::move(data);
    for (auto& [volume, paint] : paint_restores)
        restore_tracery_base_paint(*volume, paint);
    for (auto& [volume, partition] : restored) {
        mesh_changed = true;
        const ObjectID old_id = volume->id();
        replace_tracery_mesh(*volume, partition);
        m_mesh_restore_states.erase(old_id);
    }
    return true;
}

void GLGizmoTracery::clear_stained_glass_editor()
{
    m_edit_document_hashes.clear();
    m_selection_modified = true;
    m_mask_data.clear();
    m_previous_mask_data.clear();
    m_stained_glass_base_paint_data.clear();
    m_tracery_selections.clear();
    m_generated_fragments.clear();
    m_generated_voronoi_regions.clear();
    m_fragment_colors.clear();
    m_fragment_mixed_filament_ids.clear();
    m_transition_recipe_targets.clear();
    m_pending_selection_fragment_count = 0;
    m_incremental_generation           = false;
    m_regeneration_requested           = false;
    m_generation_integrity_valid       = true;
}

void GLGizmoTracery::update_model_object()
{
    const TraceryPerformanceScope performance("stroke");
    // The selectors already contain the brush preview, but m_mask_data still
    // describes the last committed stroke. Keep the complete editor checkpoint
    // until every volume's generated result has been prepared.
    auto state = std::tie(m_mask_object_id, m_mask_object_index, m_mask_data, m_previous_mask_data,
                         m_stained_glass_base_paint_data, m_tracery_selections, m_generated_fragments,
                         m_generated_voronoi_regions, m_fragment_colors, m_fragment_mixed_filament_ids,
                         m_transition_recipe_targets, m_pending_selection_fragment_count, m_pending_selection_mode,
                         m_incremental_generation, m_selection_modified, m_generation_integrity_valid,
                         m_regeneration_requested, m_regeneration_version);
    auto checkpoint = std::apply([](const auto&... values) { return std::make_tuple(values...); }, state);
    bool restored = false;
    const std::function<void()> rollback = [&] {
        if (restored)
            return;
        state = std::move(checkpoint);
        restored = true;
        update_from_model_object(false);
        m_parent.set_as_dirty();
    };
    try {
        m_selection_modified = true;
        if (m_regeneration_requested) {
            m_previous_mask_data = m_mask_data;
            m_incremental_generation = false;
            m_pending_selection_mode = m_paint_mode;
        } else {
            commit_material_selection();
            capture_selection_mask();
            m_incremental_generation = !m_active_stroke_erases;
        }
        generate_paint_plan();
        if (m_incremental_generation && m_fragment_colors.empty())
            save_selection_mask();
        else
            apply_stained_glass_colors(rollback);
    } catch (const std::exception& error) {
        rollback();
        wxMessageBox(from_u8(error.what()), _L("Stained glass"), wxOK | wxICON_ERROR);
        return;
    }
    m_parent.set_as_dirty();
}

void GLGizmoTracery::refresh_source_selection_policy(bool filter_masks)
{
    const auto* bundle = wxGetApp().preset_bundle;
    const auto* object = m_c && m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (!bundle || !object)
        return;
    size_t index = 0;
    for (const auto* volume : object->volumes) {
        if (!volume->is_model_part())
            continue;
        if (index >= m_fill_reference_selectors.size() || index >= m_triangle_selectors.size())
            break;
        const auto blocked = tracery_unselectable_states(*bundle, unsigned(std::max(1, volume->extruder_id())));
        const bool changed = index >= m_source_selection_policy.size() || m_source_selection_policy[index] != blocked;
        if (index >= m_source_selection_policy.size())
            m_source_selection_policy.resize(index + 1);
        m_source_selection_policy[index] = blocked;
        m_fill_reference_selectors[index]->set_unselectable_states(blocked);
        if (changed)
            m_fill_reference_selectors[index]->request_update_render_data();
        if ((filter_masks || changed) && !blocked.empty()) {
            TriangleSelector source(volume->mesh());
            source.deserialize(index < m_stained_glass_base_paint_data.size() ?
                m_stained_glass_base_paint_data[index] : volume->mmu_segmentation_facets.get_data(), false);
            tracery_filter_source_selection(*m_triangle_selectors[index], source, blocked);
            m_triangle_selectors[index]->request_update_render_data(true);
            if (index < m_mask_data.size())
                m_mask_data[index] = m_triangle_selectors[index]->serialize();
        }
        ++index;
    }
}

void GLGizmoTracery::commit_material_selection()
{
    constexpr auto marker = EnforcerBlockerType::ExtruderMax;
    const ModelObject* object = m_c->selection_info()->model_object();
    if (object == nullptr)
        return;
    size_t mesh_idx = 0;
    for (const ModelVolume* volume : object->volumes) {
        if (!volume->is_model_part())
            continue;
        TriangleSelectorPatch& material = *m_fill_reference_selectors[mesh_idx];
        TriangleSelectorGUI& mask = *m_triangle_selectors[mesh_idx];
        if (!material.has_facets(marker)) {
            ++mesh_idx;
            continue;
        }
        TriangleSelector previous_mask(volume->mesh());
        previous_mask.deserialize(mask.serialize(), false);
        mask.deserialize(material.serialize(), false);
        mask.set_leaf_states([&](const Vec3f& point, int facet) {
            return material.state_at(point, facet) == marker ?
                (m_active_stroke_erases ? EnforcerBlockerType::NONE : EnforcerBlockerType::ENFORCER) :
                previous_mask.state_at(point, facet);
        });
        TriangleSelector base_paint(volume->mesh());
        const auto& base = mesh_idx < m_stained_glass_base_paint_data.size() ?
            m_stained_glass_base_paint_data[mesh_idx] : volume->mmu_segmentation_facets.get_data();
        base_paint.deserialize(base, false);
        material.set_leaf_states([&](const Vec3f& point, int facet) {
            return material.state_at(point, facet) == marker ? base_paint.state_at(point, facet) :
                                                               material.state_at(point, facet);
        });
        material.request_update_render_data(true);
        ++mesh_idx;
    }
}

void GLGizmoTracery::capture_selection_mask()
{
    const TraceryPerformanceScope performance("selection_and_generation");
    const ModelObject* model_object = m_c != nullptr && m_c->selection_info() != nullptr ?
        m_c->selection_info()->model_object() : nullptr;
    if (model_object == nullptr)
        return;

    m_mask_object_id = model_object->id();
    if (const Model* model = m_parent.get_model()) {
        m_mask_object_index = -1;
        for (size_t index = 0; index < model->objects.size(); ++index) {
            if (model->objects[index] == model_object) {
                m_mask_object_index = int(index);
                break;
            }
        }
    }
    // Capture the ordinary paint exactly once, before the first generated
    // result is committed. Subsequent edits must not sample the previous
    // stained-glass result as if it were the original model colour.
    if (m_stained_glass_base_paint_data.empty()) {
        m_stained_glass_base_paint_data.reserve(m_triangle_selectors.size());
        for (const ModelVolume* volume : model_object->volumes) {
            if (volume->is_model_part())
                m_stained_glass_base_paint_data.emplace_back(volume->mmu_segmentation_facets.get_data());
        }
    }
    m_previous_mask_data = m_mask_data;
    m_mask_data.clear();
    m_mask_data.reserve(m_triangle_selectors.size());
    for (const std::unique_ptr<TriangleSelectorGUI>& selector : m_triangle_selectors)
        m_mask_data.emplace_back(selector->serialize());
    m_incremental_generation = false;
    m_pending_selection_mode = m_paint_mode;
}

void GLGizmoTracery::save_selection_mask()
{
    auto* object = m_c->selection_info()->model_object();
    std::vector<std::pair<ModelVolume*, std::string>> updates;
    size_t index = 0;
    for (auto* volume : object->volumes) {
        if (!volume->is_model_part()) continue;
        auto channels = collect_partition_annotations(*volume, index++);
        auto doc = volume->tracery_edit_data.empty() ? TraceryDocument::capture(volume->mesh(), channels) :
                                                     TraceryDocument::decode(volume->tracery_edit_data);
        if (doc.base_mesh().its.vertices == volume->mesh().its.vertices && doc.base_mesh().its.indices == volume->mesh().its.indices) {
            doc.apply_vector_edit(volume->mesh(), channels, {}, TraceryEdit::Paint);
            doc.set_selection_reference(m_fill_reference_selectors.at(index - 1)->serialize());
            updates.emplace_back(volume, doc.encode());
        }
    }
    for (auto& [volume, data] : updates) volume->tracery_edit_data = std::move(data);
}

bool GLGizmoTracery::collect_generation_input(GenerationInput& input)
{
    using SurfaceTriangle = GenerationInput::SurfaceTriangle;
    using ColorSource = GenerationInput::ColorSource;

    const Model* canvas_model = m_parent.get_model();
    int object_idx = -1;
    if (canvas_model != nullptr) {
        for (size_t idx = 0; idx < canvas_model->objects.size(); ++idx) {
            if (canvas_model->objects[idx] == input.model_object) {
                object_idx = int(idx);
                break;
            }
        }
    }
    const auto volume_source_color = [this, object_idx](int volume_idx) {
        if (object_idx >= 0) {
            for (const GLVolume* display_volume : m_parent.get_volumes().volumes) {
                if (display_volume->composite_id.object_id == object_idx && display_volume->composite_id.volume_id == volume_idx) {
                    ColorRGBA color = display_volume->color;
                    color.a(1.0f);
                    return color;
                }
            }
        }
        return GLVolume::NEUTRAL_COLOR;
    };

    const std::vector<ColorRGBA> extruder_colors = ::get_extruders_colors();
    size_t mesh_idx = 0;
    for (size_t volume_idx = 0; volume_idx < input.model_object->volumes.size(); ++volume_idx) {
        const ModelVolume* volume = input.model_object->volumes[volume_idx];
        if (!volume->is_model_part())
            continue;
        if (mesh_idx >= m_triangle_selectors.size())
            break;
        collect_volume_generation_input(input, mesh_idx, volume_idx, extruder_colors, volume_source_color(int(volume_idx)));
        ++mesh_idx;
    }

    if (input.surface_triangles.empty()) {
        return false;
    }
    return true;
}

void GLGizmoTracery::collect_volume_generation_input(
    GenerationInput& input, size_t mesh_idx, size_t volume_idx,
    const std::vector<ColorRGBA>& extruder_colors, const ColorRGBA& source_color) const
{
    using ColorSource = GenerationInput::ColorSource;
    TraceryPerformanceScope performance("selection_input_volume");
    performance.count("volume_index", volume_idx);
    performance.count("incremental", m_incremental_generation ? 1 : 0);
    performance.stage("selection_input.extract_current");
    const ModelVolume* volume = input.model_object->volumes[volume_idx];
    if (m_incremental_generation && mesh_idx < m_previous_mask_data.size() &&
        m_triangle_selectors[mesh_idx]->serialize() == m_previous_mask_data[mesh_idx]) {
        // Keep transform indices aligned with the other model parts.
        input.volume_transforms.emplace_back(volume->get_matrix());
        performance.count("unchanged_selection", 1);
        return;
    }
    performance.count("unchanged_selection", 0);
    const auto selected = m_triangle_selectors[mesh_idx]->get_facets_strict_with_sources(EnforcerBlockerType::ENFORCER);
    assert(selected.facets.indices.size() == selected.source_facet_ids.size());
    performance.count("selected_facets", selected.facets.indices.size());
    struct PreviousFacet {
        int source_facet_id = -1;
        Vec3f a = Vec3f::Zero();
        Vec3f b = Vec3f::Zero();
        Vec3f c = Vec3f::Zero();
    };
    std::vector<PreviousFacet> previous_facets;
    TraceryPreviousFacetIndex previous_index_by_source;
    performance.stage("selection_input.prepare_previous");
    if (m_incremental_generation && mesh_idx < m_previous_mask_data.size()) {
        TriangleSelector previous_selector(volume->mesh());
        previous_selector.deserialize(m_previous_mask_data[mesh_idx], false);
        const auto previous = previous_selector.get_facets_strict_with_sources(EnforcerBlockerType::ENFORCER);
        previous_facets.reserve(previous.facets.indices.size());
        for (size_t index = 0; index < previous.facets.indices.size() && index < previous.source_facet_ids.size(); ++index) {
            const stl_triangle_vertex_indices& face = previous.facets.indices[index];
            if (face[0] < 0 || face[1] < 0 || face[2] < 0 ||
                size_t(face[0]) >= previous.facets.vertices.size() ||
                size_t(face[1]) >= previous.facets.vertices.size() || size_t(face[2]) >= previous.facets.vertices.size())
                continue;
            previous_facets.push_back({ previous.source_facet_ids[index], previous.facets.vertices[size_t(face[0])],
                previous.facets.vertices[size_t(face[1])], previous.facets.vertices[size_t(face[2])] });
            previous_index_by_source.add(previous.source_facet_ids[index], previous_facets.size() - 1);
        }
    }
    performance.count("previous_facets", previous_facets.size());
    size_t scanned_previous = 0;
    const auto already_selected = [&previous_facets, &previous_index_by_source, &performance, &scanned_previous](int source_facet_id, const Vec3f& point) {
        const auto& candidates = previous_index_by_source.candidates(source_facet_id);
        for (size_t candidate_index = 0; candidate_index < candidates.size(); ++candidate_index) {
            const PreviousFacet& previous = previous_facets[candidates[candidate_index]];
            const Vec3f normal = (previous.b - previous.a).cross(previous.c - previous.a);
            const float normal_length = normal.norm();
            if (normal_length <= 1e-12f || std::abs(normal.dot(point - previous.a)) > normal_length * 1e-5f)
                continue;
            const float first = normal.dot((previous.b - previous.a).cross(point - previous.a));
            const float second = normal.dot((previous.c - previous.b).cross(point - previous.b));
            const float third = normal.dot((previous.a - previous.c).cross(point - previous.c));
            constexpr float tolerance = 1e-7f;
            if ((first >= -tolerance && second >= -tolerance && third >= -tolerance) ||
                (first <= tolerance && second <= tolerance && third <= tolerance)) {
                if (performance.enabled()) scanned_previous += candidate_index + 1;
                return true;
            }
        }
        if (performance.enabled()) scanned_previous += candidates.size();
        return false;
    };
    performance.stage("selection_input.prepare_colors");
    const Transform3d& transform = volume->get_matrix();
    input.volume_transforms.emplace_back(transform);
    std::vector<indexed_triangle_set> facets_per_color;
    const bool trace_color = tracery_color_trace_enabled();
    if (mesh_idx < m_stained_glass_base_paint_data.size()) {
        TriangleSelector selector(volume->mesh());
        selector.deserialize(m_stained_glass_base_paint_data[mesh_idx], false);
        selector.get_facets(facets_per_color);
    } else if (!volume->mmu_segmentation_facets.empty())
        volume->mmu_segmentation_facets.get_facets(*volume, facets_per_color);
    for (size_t color_idx = 0; color_idx < facets_per_color.size(); ++color_idx) {
        if (facets_per_color[color_idx].indices.empty())
            continue;
        ColorRGBA color = source_color;
        if (color_idx == 0 && volume->extruder_id() > 0 && size_t(volume->extruder_id()) <= extruder_colors.size())
            color = adjust_color_for_rendering(extruder_colors[size_t(volume->extruder_id() - 1)]);
        else if (color_idx > 0 && color_idx <= extruder_colors.size())
            color = adjust_color_for_rendering(extruder_colors[color_idx - 1]);
        color.a(1.0f);
        if (trace_color)
            std::fprintf(stderr, "TRACERY_COLOR source volume=%zu mesh=%zu state=%zu rgb=%06X facets=%zu snapshot=%d extruder=%d\n",
                volume_idx, mesh_idx, color_idx, tracery_color_rgb(color), facets_per_color[color_idx].indices.size(),
                int(mesh_idx < m_stained_glass_base_paint_data.size()), volume->extruder_id());
        auto source = std::make_unique<ColorSource>();
        source->color = color;
        source->facets = std::move(facets_per_color[color_idx]);
        source->aabb = std::make_unique<AABBMesh>(source->facets);
        input.color_sources[mesh_idx].emplace_back(std::move(source));
    }
    performance.count("color_sources", input.color_sources[mesh_idx].size());
    performance.stage("selection_input.filter_previous");
    std::vector<size_t> remaining;
    remaining.reserve(selected.facets.indices.size());
    for (size_t index = 0; index < selected.facets.indices.size(); ++index) {
        const stl_triangle_vertex_indices& face = selected.facets.indices[index];
        const Vec3f a = selected.facets.vertices[face[0]], b = selected.facets.vertices[face[1]], c = selected.facets.vertices[face[2]];
        if (!already_selected(selected.source_facet_ids[index], (a + b + c) / 3.0f))
            remaining.push_back(index);
    }
    performance.count("previous_entries_scanned", scanned_previous);
    performance.count("excluded_facets", selected.facets.indices.size() - remaining.size());
    performance.count("remaining_facets", remaining.size());
    performance.stage("selection_input.color_and_emit");
    std::map<unsigned, size_t> sampled_colors;
    size_t ambiguous_samples = 0;
    size_t no_source_samples = 0;
    for (size_t index : remaining) {
        const stl_triangle_vertex_indices& face = selected.facets.indices[index];
        const Vec3f a = selected.facets.vertices[face[0]], b = selected.facets.vertices[face[1]], c = selected.facets.vertices[face[2]];
        Vec3f normal = (b - a).cross(c - a);
        normal = normal.squaredNorm() > 0.0f ? normal.normalized() : Vec3f::UnitZ();
        const Vec3f world_a = (transform * a.cast<double>()).cast<float>();
        const Vec3f world_b = (transform * b.cast<double>()).cast<float>();
        const Vec3f world_c = (transform * c.cast<double>()).cast<float>();
        ColorRGBA color = source_color;
        double nearest = std::numeric_limits<double>::max();
        bool ambiguous = false;
        for (const auto& source : input.color_sources[mesh_idx]) {
            const double distance = source->aabb->squared_distance(((a + b + c) / 3.0f).cast<double>());
            if (distance < nearest) {
                ambiguous = trace_color && nearest < std::numeric_limits<double>::max() &&
                    std::abs(distance - nearest) <= 1e-12 && tracery_color_rgb(color) != tracery_color_rgb(source->color);
                nearest = distance;
                color = source->color;
            } else if (trace_color && std::abs(distance - nearest) <= 1e-12 &&
                       tracery_color_rgb(color) != tracery_color_rgb(source->color))
                ambiguous = true;
        }
        if (trace_color) {
            ++sampled_colors[tracery_color_rgb(color)];
            if (input.color_sources[mesh_idx].empty()) ++no_source_samples;
            if (ambiguous && ambiguous_samples++ < 12)
                std::fprintf(stderr, "TRACERY_COLOR ambiguous volume=%zu facet=%d rgb=%06X distance2=%.12g center=%.6f,%.6f,%.6f\n",
                    volume_idx, selected.source_facet_ids[index], tracery_color_rgb(color), nearest,
                    double((a.x() + b.x() + c.x()) / 3), double((a.y() + b.y() + c.y()) / 3), double((a.z() + b.z() + c.z()) / 3));
        }
        input.surface_area += 0.5 * double((world_b - world_a).cross(world_c - world_a).norm());
        input.surface_triangles.push_back({ mesh_idx, selected.source_facet_ids[index], a, b, c,
            world_a, world_b, world_c, normal, color });
    }
    if (trace_color) {
        std::fprintf(stderr, "TRACERY_COLOR sampling volume=%zu selected=%zu emitted=%zu ambiguous=%zu no_source=%zu fallback_rgb=%06X\n",
            volume_idx, selected.facets.indices.size(), remaining.size(), ambiguous_samples, no_source_samples, tracery_color_rgb(source_color));
        for (const auto& entry : sampled_colors)
            std::fprintf(stderr, "TRACERY_COLOR sampled volume=%zu rgb=%06X facets=%zu\n", volume_idx, entry.first, entry.second);
    }
}

bool GLGizmoTracery::build_color_regions_from_input_faces(
    const GenerationInput& input, std::vector<ColorGroup>& color_groups,
    std::vector<VoronoiColorRegion>& regions) const
{
    TraceryPerformanceScope performance("color_regions_detail");
    performance.stage("color_regions.initialize");
    const auto& faces = input.surface_triangles;
    if (faces.empty())
        return false;
    TraceryDisjointSet components(faces.size());
    const auto same_color = [](const ColorRGBA& first, const ColorRGBA& second) {
        return (Vec4f(first.r(), first.g(), first.b(), first.a()) -
                Vec4f(second.r(), second.g(), second.b(), second.a())).cwiseAbs().maxCoeff() <= 1e-5f;
    };
    const auto edge_key = [](size_t mesh_idx, const Vec3f& first, const Vec3f& second) {
        const auto point = [](const Vec3f& value) {
            return std::array<int64_t, 3>{ int64_t(std::llround(value.x() * 100000.0f)),
                                           int64_t(std::llround(value.y() * 100000.0f)),
                                           int64_t(std::llround(value.z() * 100000.0f)) };
        };
        auto a = point(first);
        auto b = point(second);
        if (b < a)
            std::swap(a, b);
        return EdgeKey{ mesh_idx, { a[0], a[1], a[2], b[0], b[1], b[2] } };
    };
    std::map<EdgeKey, size_t> owners;
    performance.count("input_faces", faces.size());
    performance.stage("color_regions.connect_edges");
    for (size_t index = 0; index < faces.size(); ++index) {
        const auto& face = faces[index];
        const std::array<std::pair<Vec3f, Vec3f>, 3> edges = {{{ face.a, face.b }, { face.b, face.c }, { face.c, face.a }}};
        for (const auto& [first, second] : edges) {
            const EdgeKey key = edge_key(face.mesh_idx, first, second);
            if (const auto found = owners.find(key); found != owners.end()) {
                if (same_color(face.source_color, faces[found->second].source_color))
                    components.unite(index, found->second);
            } else
                owners.emplace(key, index);
        }
    }
    std::map<size_t, size_t> region_by_root;
    performance.count("unique_edges", owners.size());
    performance.stage("color_regions.group_faces");
    color_groups.clear();
    regions.clear();
    for (size_t index = 0; index < faces.size(); ++index) {
        const size_t face_root = components.root(index);
        auto [entry, inserted] = region_by_root.emplace(face_root, regions.size());
        if (inserted) {
            color_groups.push_back({ color_groups.size(), faces[index].source_color });
            regions.push_back({ color_groups.size() - 1, {}, 0.0, 0 });
        }
        VoronoiColorRegion& region = regions[entry->second];
        region.face_indices.emplace_back(index);
        const auto& face = faces[index];
        region.world_area += 0.5 * double((face.world_b - face.world_a).cross(face.world_c - face.world_a).norm());
    }
    performance.count("connected_regions", regions.size());
    performance.stage("color_regions.cleanup");
    owners.clear();
    return !regions.empty();
}

void GLGizmoTracery::allocate_region_fragment_budgets(
    std::vector<VoronoiColorRegion>& regions, float maximum_fragment_diameter) const
{
    // Direct Voronoi generation allocates from surface area. Density limits
    // the maximum number of seeds for one connected colour region; it does
    // not force small regions to be split into a minimum number of shards.
    const size_t maximum_fragments_per_region =
        std::clamp<size_t>(size_t(std::lround(8.0f + 16.0f * m_fragment_density)), 8, 24);
    const double maximum_piece_area = std::max(0.0001, 0.70 * double(maximum_fragment_diameter) * maximum_fragment_diameter);
    for (VoronoiColorRegion& region : regions) {
        const size_t area_count = std::max<size_t>(1, size_t(std::ceil(region.world_area / maximum_piece_area)));
        region.fragment_budget = std::max<size_t>(1, std::min(maximum_fragments_per_region, area_count));
    }
    size_t total = 0;
    for (const VoronoiColorRegion& region : regions)
        total += region.fragment_budget;
    constexpr size_t maximum_fragments = 2048;
    if (total > maximum_fragments && regions.size() <= maximum_fragments) {
        const double scale = double(maximum_fragments) / double(total);
        total = 0;
        for (VoronoiColorRegion& region : regions) {
            region.fragment_budget = std::max<size_t>(1, size_t(region.fragment_budget * scale));
            total += region.fragment_budget;
        }
        while (total > maximum_fragments) {
            auto largest = std::max_element(regions.begin(), regions.end(),
                [](const auto& first, const auto& second) { return first.fragment_budget < second.fragment_budget; });
            if (largest->fragment_budget <= 1)
                break;
            --largest->fragment_budget;
            --total;
        }
    }
}

void GLGizmoTracery::generate_region_seeds(
    const GenerationInput& input, const std::vector<VoronoiColorRegion>& regions,
    std::vector<FragmentSeed>& seeds) const
{
    seeds.clear();
    for (const VoronoiColorRegion& region : regions) {
        if (region.fragment_budget == 0 || region.face_indices.empty())
            continue;
        const std::vector<Vec3f> candidates = sample_region_seed_candidates(input, region);
        std::vector<float> nearest_distance(candidates.size(), std::numeric_limits<float>::max());
        for (size_t local_seed = 0; local_seed < region.fragment_budget; ++local_seed) {
            size_t selected = local_seed == 0 ? region.color_group % candidates.size() : 0;
            float farthest = -1.0f;
            if (local_seed > 0)
                for (size_t index = 0; index < candidates.size(); ++index) {
                    nearest_distance[index] = std::min(nearest_distance[index],
                        (candidates[index] - seeds.back().position).squaredNorm());
                    if (nearest_distance[index] > farthest) {
                        farthest = nearest_distance[index];
                        selected = index;
                    }
                }
            const size_t stable_id = (region.color_group * size_t(2654435761U)) ^ local_seed ^
                                     (m_regeneration_version * size_t(0x85ebca6bU));
            seeds.push_back({ candidates[selected], region.color_group, stable_id });
        }
    }
}

std::vector<Vec3f> GLGizmoTracery::sample_region_seed_candidates(
    const GenerationInput& input, const VoronoiColorRegion& region) const
{
    const size_t samples_per_face = std::max<size_t>(4,
        size_t(std::ceil(double(region.fragment_budget * 2) / region.face_indices.size())));
    std::vector<Vec3f> candidates;
    candidates.reserve(region.face_indices.size() * samples_per_face);
    for (size_t face_index : region.face_indices) {
        const auto& face = input.surface_triangles[face_index];
        for (size_t sample = 0; sample < samples_per_face; ++sample) {
            float u = (float(sample) + 0.5f) / float(samples_per_face);
            float v = std::fmod((float(sample) + 1.0f) * 0.61803398875f, 1.0f);
            if (u + v > 1.0f) {
                u = 1.0f - u;
                v = 1.0f - v;
            }
            candidates.emplace_back(face.world_a + u * (face.world_b - face.world_a) +
                                    v * (face.world_c - face.world_a));
        }
    }
    return candidates;
}

namespace {

struct TraceryVoronoiVertex {
    Vec3f local;
    Vec3f world;
    Vec3f warped;
};

Vec3f tracery_warp_position(const Vec3f& point, float /* target_edge */)
{
    // A planar Voronoi bisector must remain a straight shared edge.
    return point;
}

std::vector<TraceryVoronoiVertex> clip_tracery_half_plane(const std::vector<TraceryVoronoiVertex>& polygon,
                                                          const Vec3d&                             direction,
                                                          double                                   limit)
{
    std::vector<TraceryVoronoiVertex> output;
    if (polygon.empty())
        return output;
    output.reserve(polygon.size() + 1);
    for (size_t index = 0; index < polygon.size(); ++index) {
        const auto&  first        = polygon[index];
        const auto&  second       = polygon[(index + 1) % polygon.size()];
        const double first_value  = first.warped.cast<double>().dot(direction) - limit;
        const double second_value = second.warped.cast<double>().dot(direction) - limit;
        if (first_value <= 0.0f)
            output.emplace_back(first);
        if ((first_value <= 0.0f) != (second_value <= 0.0f)) {
            const float fraction = float(std::clamp(first_value / (first_value - second_value), 0.0, 1.0));
            output.push_back({first.local + fraction * (second.local - first.local), first.world + fraction * (second.world - first.world),
                              first.warped + fraction * (second.warped - first.warped)});
        }
    }
    return output;
}

using TraceryBisectors = std::map<std::pair<size_t, size_t>, std::pair<Vec3d, double>>;

TraceryBisectors build_tracery_bisectors(const std::vector<Vec3f>& seeds, const std::vector<size_t>& indices)
{
    TraceryBisectors result;
    for (size_t a = 0; a < indices.size(); ++a) {
        for (size_t b = a + 1; b < indices.size(); ++b) {
            const auto  key       = std::minmax(indices[a], indices[b]);
            const Vec3d first     = seeds[key.first].cast<double>();
            const Vec3d second    = seeds[key.second].cast<double>();
            const Vec3d direction = second - first;
            result.emplace(key, std::make_pair(direction, direction.dot((first + second) * 0.5)));
        }
    }
    return result;
}

std::optional<TriangleSelector::TraceryFacet2DData> make_tracery_facet_plane(
    const indexed_triangle_set& mesh, int source_facet_id)
{
    if (source_facet_id < 0 || size_t(source_facet_id) >= mesh.indices.size())
        return std::nullopt;
    const stl_triangle_vertex_indices& face = mesh.indices[size_t(source_facet_id)];
    if (face[0] < 0 || face[1] < 0 || face[2] < 0 || size_t(face[0]) >= mesh.vertices.size() ||
        size_t(face[1]) >= mesh.vertices.size() || size_t(face[2]) >= mesh.vertices.size())
        return std::nullopt;
    const Vec3d origin = mesh.vertices[size_t(face[0])].cast<double>();
    Vec3d axis_u = mesh.vertices[size_t(face[1])].cast<double>() - origin;
    const Vec3d edge_v = mesh.vertices[size_t(face[2])].cast<double>() - origin;
    Vec3d normal = axis_u.cross(edge_v);
    if (axis_u.squaredNorm() <= 1e-18 || normal.squaredNorm() <= 1e-18)
        return std::nullopt;
    axis_u.normalize();
    normal.normalize();
    Vec3d axis_v = normal.cross(axis_u);
    axis_v.normalize();
    return TriangleSelector::TraceryFacet2DData{ source_facet_id, origin, axis_u, axis_v, {} };
}

} // namespace

void GLGizmoTracery::generate_voronoi_regions(
    const GenerationInput& input, const std::vector<VoronoiColorRegion>& regions,
    const std::vector<FragmentSeed>& seeds,
    std::vector<std::vector<GeneratedVoronoiRegion>>& output) const
{
    output.assign(m_triangle_selectors.size(), {});
    const float target_edge = 6.5f - 4.0f * m_fragment_density;
    std::vector<Vec3f> warped_seeds;
    warped_seeds.reserve(seeds.size());
    for (const FragmentSeed& seed : seeds)
        warped_seeds.emplace_back(tracery_warp_position(seed.position, target_edge));
    for (const VoronoiColorRegion& region : regions) {
        std::vector<size_t> region_seeds;
        for (size_t index = 0; index < seeds.size(); ++index)
            if (seeds[index].color_group == region.color_group)
                region_seeds.emplace_back(index);
        const TraceryBisectors bisectors = build_tracery_bisectors(warped_seeds, region_seeds);
        for (size_t face_index : region.face_indices) {
            const auto& face = input.surface_triangles[face_index];
            if (face.mesh_idx >= output.size())
                continue;
            const std::array<TraceryVoronoiVertex, 3> source = {{{ face.a, face.world_a, tracery_warp_position(face.world_a, target_edge) },
                                                                    { face.b, face.world_b, tracery_warp_position(face.world_b, target_edge) },
                                                                    { face.c, face.world_c, tracery_warp_position(face.world_c, target_edge) }}};
            for (size_t seed : region_seeds) {
                std::vector<TraceryVoronoiVertex> polygon(source.begin(), source.end());
                for (size_t other : region_seeds) {
                    if (other == seed || polygon.empty())
                        continue;
                    const auto& boundary = bisectors.at(std::minmax(seed, other));
                    const double side = seed < other ? 1. : -1.;
                    polygon = clip_tracery_half_plane(polygon, side * boundary.first, side * boundary.second);
                }
                if (polygon.size() < 3)
                    continue;
                GeneratedVoronoiRegion generated{ seed, face.original_facet_id, {} };
                generated.local_polygon.reserve(polygon.size());
                for (const TraceryVoronoiVertex& vertex : polygon)
                    generated.local_polygon.emplace_back(vertex.local);
                output[face.mesh_idx].emplace_back(std::move(generated));
            }
        }
    }
}

void GLGizmoTracery::build_fragment_topology(
    const std::vector<std::vector<GeneratedVoronoiRegion>>& regions, size_t fragment_count,
    std::vector<std::set<size_t>>& neighbours) const
{
    neighbours.assign(fragment_count, {});
    const auto point = [](const Vec3f& value) {
        return std::array<int64_t, 3>{ int64_t(std::llround(value.x() * 100000.0f)),
                                       int64_t(std::llround(value.y() * 100000.0f)),
                                       int64_t(std::llround(value.z() * 100000.0f)) };
    };
    std::map<EdgeKey, size_t> owners;
    for (size_t volume_index = 0; volume_index < regions.size(); ++volume_index)
        for (const GeneratedVoronoiRegion& region : regions[volume_index]) {
            if (region.fragment_id >= fragment_count || region.local_polygon.size() < 3)
                continue;
            for (size_t index = 0; index < region.local_polygon.size(); ++index) {
                auto first = point(region.local_polygon[index]);
                auto second = point(region.local_polygon[(index + 1) % region.local_polygon.size()]);
                if (second < first)
                    std::swap(first, second);
                const EdgeKey edge{ volume_index, { first[0], first[1], first[2], second[0], second[1], second[2] } };
                const auto [owner, inserted] = owners.emplace(edge, region.fragment_id);
                if (!inserted && owner->second != region.fragment_id) {
                    neighbours[region.fragment_id].insert(owner->second);
                    neighbours[owner->second].insert(region.fragment_id);
                }
            }
        }
}

void GLGizmoTracery::assign_voronoi_fragment_colors(
    const std::vector<ColorGroup>& color_groups, const std::vector<FragmentSeed>& seeds,
    const std::vector<std::set<size_t>>& neighbours)
{
    const size_t no_group = color_groups.size();
    std::vector<float> boundary_distance(seeds.size(), std::numeric_limits<float>::max());
    std::vector<size_t> boundary_neighbor(seeds.size(), no_group);
    const float target_edge = 6.5f - 4.0f * m_fragment_density;
    assign_fragment_colors(color_groups, seeds, neighbours, boundary_distance, boundary_neighbor,
                           seeds.size(), target_edge, m_refraction_strength, m_fragment_colors, false, nullptr);
}

void GLGizmoTracery::publish_generated_regions(std::vector<std::vector<GeneratedVoronoiRegion>>&& regions)
{
    m_generated_voronoi_regions = std::move(regions);
}

void GLGizmoTracery::build_work_surface(const GenerationInput& input, float boundary_sample_edge,
                                        std::vector<WorkTriangle>& work_triangles) const
{
    using SurfaceTriangle = GenerationInput::SurfaceTriangle;
    using ColorSource = GenerationInput::ColorSource;

    const auto& color_sources = input.color_sources;
    const auto sampled_source_color = [&color_sources](size_t mesh_idx, const Vec3f& local_position,
                                                        const ColorRGBA& fallback) {
        if (mesh_idx >= color_sources.size() || color_sources[mesh_idx].empty())
            return fallback;
        ColorRGBA result = fallback;
        double closest_distance = std::numeric_limits<double>::max();
        for (const std::unique_ptr<ColorSource>& source : color_sources[mesh_idx]) {
            const double distance = source->aabb->squared_distance(local_position.cast<double>());
            if (distance < closest_distance) {
                closest_distance = distance;
                result = source->color;
            }
        }
        return result;
    };

    work_triangles.clear();
    work_triangles.reserve(input.surface_triangles.size() * 24);
    for (const SurfaceTriangle& source : input.surface_triangles) {
        const float longest_edge = std::max({ (source.world_b - source.world_a).norm(), (source.world_c - source.world_b).norm(), (source.world_a - source.world_c).norm() });
        const int subdivisions = std::clamp(int(std::ceil(longest_edge / boundary_sample_edge)), 1, 96);
        const float inv_subdivisions = 1.0f / float(subdivisions);
        const auto point = [inv_subdivisions](const Vec3f& a, const Vec3f& b, const Vec3f& c, int i, int j) {
            return a + (b - a) * (float(i) * inv_subdivisions) + (c - a) * (float(j) * inv_subdivisions);
        };
        for (int i = 0; i < subdivisions; ++i) {
            for (int j = 0; j < subdivisions - i; ++j) {
                const Vec3f a = point(source.a, source.b, source.c, i, j);
                const Vec3f b = point(source.a, source.b, source.c, i + 1, j);
                const Vec3f c = point(source.a, source.b, source.c, i, j + 1);
                const Vec3f world_a = point(source.world_a, source.world_b, source.world_c, i, j);
                const Vec3f world_b = point(source.world_a, source.world_b, source.world_c, i + 1, j);
                const Vec3f world_c = point(source.world_a, source.world_b, source.world_c, i, j + 1);
                const ColorRGBA color = sampled_source_color(source.mesh_idx, (a + b + c) / 3.0f, source.source_color);
                work_triangles.push_back({ source.mesh_idx, source.original_facet_id, a, b, c, world_a, world_b, world_c, source.normal, color, 0,
                    0.5f * (world_b - world_a).cross(world_c - world_a).norm() });
                if (j < subdivisions - i - 1) {
                    const Vec3f d = point(source.a, source.b, source.c, i + 1, j + 1);
                    const Vec3f world_d = point(source.world_a, source.world_b, source.world_c, i + 1, j + 1);
                    const ColorRGBA color = sampled_source_color(source.mesh_idx, (b + d + c) / 3.0f, source.source_color);
                    work_triangles.push_back({ source.mesh_idx, source.original_facet_id, b, d, c, world_b, world_d, world_c, source.normal, color, 0,
                        0.5f * (world_d - world_b).cross(world_c - world_b).norm() });
                }
            }
        }
    }
}

GLGizmoTracery::ColorRegionIntegrity GLGizmoTracery::inspect_color_regions(
    const std::vector<WorkTriangle>& work_triangles, const std::vector<ColorGroup>& color_groups,
    const std::vector<std::vector<size_t>>& group_triangles, const std::vector<double>& group_areas) const
{
    const auto mix_hash = [](size_t& hash, size_t value) {
        hash = (hash * size_t(0x9e3779b1U)) ^ value;
    };
    const auto mix_float = [&mix_hash](size_t& hash, float value) {
        mix_hash(hash, std::hash<float>{}(value));
    };
    const auto mix_color = [&mix_float](size_t& hash, const ColorRGBA& color) {
        mix_float(hash, color.r());
        mix_float(hash, color.g());
        mix_float(hash, color.b());
        mix_float(hash, color.a());
    };

    ColorRegionIntegrity result;
    result.work_triangle_count = work_triangles.size();
    result.group_count = color_groups.size();
    result.immutable_hash = size_t(0x85ebca6bU);
    bool source_ids_valid = true;
    for (const WorkTriangle& triangle : work_triangles) {
        mix_hash(result.immutable_hash, triangle.mesh_idx);
        mix_hash(result.immutable_hash, size_t(triangle.original_facet_id));
        if (triangle.original_facet_id < 0)
            source_ids_valid = false;
        for (const Vec3f* point : { &triangle.a, &triangle.b, &triangle.c, &triangle.world_a, &triangle.world_b,
                                    &triangle.world_c, &triangle.normal }) {
            mix_float(result.immutable_hash, point->x());
            mix_float(result.immutable_hash, point->y());
            mix_float(result.immutable_hash, point->z());
        }
        mix_color(result.immutable_hash, triangle.source_color);
        mix_float(result.immutable_hash, triangle.area);
        result.work_area += triangle.area;
    }

    result.output_hash = size_t(0xc2b2ae35U);
    mix_hash(result.output_hash, color_groups.size());
    for (const ColorGroup& group : color_groups) {
        mix_hash(result.output_hash, group.component_id);
        mix_color(result.output_hash, group.color);
    }
    const bool group_shapes_match = group_triangles.size() == color_groups.size() && group_areas.size() == color_groups.size();
    mix_hash(result.output_hash, group_triangles.size());
    mix_hash(result.output_hash, group_areas.size());
    std::vector<unsigned char> seen(work_triangles.size(), 0);
    bool assignments_valid = group_shapes_match && source_ids_valid;
    for (size_t group = 0; group < group_triangles.size(); ++group) {
        const double area = group < group_areas.size() ? group_areas[group] : 0.0;
        result.grouped_area += area;
        mix_hash(result.output_hash, group_triangles[group].size());
        mix_hash(result.output_hash, std::hash<double>{}(area));
        for (size_t triangle_index : group_triangles[group]) {
            mix_hash(result.output_hash, triangle_index);
            if (triangle_index >= work_triangles.size() || group >= color_groups.size() || seen[triangle_index] != 0) {
                assignments_valid = false;
                continue;
            }
            const WorkTriangle& triangle = work_triangles[triangle_index];
            if (triangle.color_group != group || triangle.source_color != color_groups[group].color)
                assignments_valid = false;
            seen[triangle_index] = 1;
            ++result.grouped_triangle_count;
        }
    }
    for (size_t index = 0; index < work_triangles.size(); ++index) {
        const WorkTriangle& triangle = work_triangles[index];
        mix_hash(result.output_hash, triangle.color_group);
        if (triangle.color_group >= color_groups.size() || seen[index] != 1)
            assignments_valid = false;
    }
    const double area_epsilon = std::max(1e-5, std::abs(result.work_area) * 1e-6);
    result.valid = assignments_valid && result.grouped_triangle_count == result.work_triangle_count &&
        std::abs(result.grouped_area - result.work_area) <= area_epsilon;
    return result;
}

GLGizmoTracery::ColorRegionIntegrity GLGizmoTracery::build_color_regions(
    std::vector<WorkTriangle>& work_triangles, std::vector<ColorGroup>& color_groups,
    std::vector<std::vector<size_t>>& group_triangles, std::vector<double>& group_areas,
    std::vector<size_t>& work_parents, std::map<EdgeKey, size_t>& work_edge_owners,
    std::map<size_t, std::array<int64_t, 4>>& work_anchors, std::map<size_t, size_t>& work_group_indices,
    bool verify_integrity) const
{
    const size_t immutable_hash_before = verify_integrity ?
        inspect_color_regions(work_triangles, color_groups, group_triangles, group_areas).immutable_hash : 0;
    const auto quantize_point = [](const Vec3f& point) {
        return std::array<int64_t, 3>{
            int64_t(std::llround(point.x() * 100000.0f)),
            int64_t(std::llround(point.y() * 100000.0f)),
            int64_t(std::llround(point.z() * 100000.0f))
        };
    };
    const auto edge_key = [&quantize_point](size_t mesh_idx, const Vec3f& first, const Vec3f& second) {
        std::array<int64_t, 3> a = quantize_point(first);
        std::array<int64_t, 3> b = quantize_point(second);
        if (b < a)
            std::swap(a, b);
        return EdgeKey{ mesh_idx, { a[0], a[1], a[2], b[0], b[1], b[2] } };
    };

    color_groups.clear();
    work_parents.resize(work_triangles.size());
    for (size_t index = 0; index < work_parents.size(); ++index)
        work_parents[index] = index;
    const auto find_work_root = [&work_parents](size_t node) {
        while (work_parents[node] != node) {
            work_parents[node] = work_parents[work_parents[node]];
            node = work_parents[node];
        }
        return node;
    };
    const auto merge_work = [&find_work_root, &work_parents](size_t first, size_t second) {
        first = find_work_root(first);
        second = find_work_root(second);
        if (first != second)
            work_parents[second] = first;
    };
    work_edge_owners.clear();
    for (size_t index = 0; index < work_triangles.size(); ++index) {
        const WorkTriangle& triangle = work_triangles[index];
        const std::array<EdgeKey, 3> edges = {
            edge_key(triangle.mesh_idx, triangle.a, triangle.b),
            edge_key(triangle.mesh_idx, triangle.b, triangle.c),
            edge_key(triangle.mesh_idx, triangle.c, triangle.a)
        };
        for (const EdgeKey& edge : edges) {
            const auto owner = work_edge_owners.find(edge);
            if (owner == work_edge_owners.end())
                work_edge_owners.emplace(edge, index);
            else if (work_triangles[owner->second].source_color == triangle.source_color)
                merge_work(index, owner->second);
        }
    }

    work_anchors.clear();
    for (size_t index = 0; index < work_triangles.size(); ++index) {
        const std::array<int64_t, 3> point = quantize_point(work_triangles[index].a);
        const std::array<int64_t, 4> anchor = { int64_t(work_triangles[index].mesh_idx), point[0], point[1], point[2] };
        const size_t root = find_work_root(index);
        const auto current = work_anchors.find(root);
        if (current == work_anchors.end() || anchor < current->second)
            work_anchors[root] = anchor;
    }
    work_group_indices.clear();
    for (size_t index = 0; index < work_triangles.size(); ++index) {
        const size_t root = find_work_root(index);
        auto group = work_group_indices.find(root);
        if (group == work_group_indices.end()) {
            size_t stable_id = 0;
            for (int64_t value : work_anchors[root])
                stable_id = (stable_id * size_t(0x9e3779b1U)) ^ size_t(value);
            group = work_group_indices.emplace(root, color_groups.size()).first;
            color_groups.emplace_back(ColorGroup{ stable_id, work_triangles[index].source_color });
        }
        work_triangles[index].color_group = group->second;
    }

    group_triangles.clear();
    group_triangles.resize(color_groups.size());
    group_areas.assign(color_groups.size(), 0.0);
    for (size_t index = 0; index < work_triangles.size(); ++index) {
        const WorkTriangle& triangle = work_triangles[index];
        group_triangles[triangle.color_group].emplace_back(index);
        group_areas[triangle.color_group] += triangle.area;
    }

    if (!verify_integrity)
        return {};
    ColorRegionIntegrity result = inspect_color_regions(work_triangles, color_groups, group_triangles, group_areas);
    result.immutable_input_preserved = result.immutable_hash == immutable_hash_before;
    result.valid = result.valid && result.immutable_input_preserved;
    return result;
}

GLGizmoTracery::FragmentGenerationIntegrity GLGizmoTracery::inspect_fragment_generation(
    const std::vector<WorkTriangle>& work_triangles, const std::vector<ColorGroup>& color_groups,
    const std::vector<std::vector<size_t>>& group_triangles, const std::vector<double>& group_areas,
    float target_edge, size_t fragment_count, const std::vector<FragmentSeed>& seeds,
    const std::vector<std::vector<CpuGeometry>>& geometry, const std::vector<size_t>& fragment_for_triangle,
    const std::vector<std::set<size_t>>& fragment_neighbours, const std::vector<float>& fragment_boundary_distance,
    const std::vector<size_t>& fragment_boundary_neighbor) const
{
    const auto mix_hash = [](size_t& hash, size_t value) {
        hash = (hash * size_t(0x9e3779b1U)) ^ value;
    };
    const auto mix_float = [&mix_hash](size_t& hash, float value) {
        mix_hash(hash, std::hash<float>{}(value));
    };
    const auto mix_color = [&mix_float](size_t& hash, const ColorRGBA& color) {
        mix_float(hash, color.r());
        mix_float(hash, color.g());
        mix_float(hash, color.b());
        mix_float(hash, color.a());
    };

    FragmentGenerationIntegrity result;
    result.input_hash = size_t(0x27d4eb2fU);
    mix_float(result.input_hash, target_edge);
    mix_hash(result.input_hash, work_triangles.size());
    bool source_ids_valid = true;
    std::vector<std::set<int>> source_facets_by_mesh(m_triangle_selectors.size());
    for (const WorkTriangle& triangle : work_triangles) {
        mix_hash(result.input_hash, triangle.mesh_idx);
        mix_hash(result.input_hash, size_t(triangle.original_facet_id));
        if (triangle.mesh_idx >= source_facets_by_mesh.size() || triangle.original_facet_id < 0)
            source_ids_valid = false;
        else
            source_facets_by_mesh[triangle.mesh_idx].insert(triangle.original_facet_id);
        mix_hash(result.input_hash, triangle.color_group);
        for (const Vec3f* point : { &triangle.a, &triangle.b, &triangle.c, &triangle.world_a, &triangle.world_b,
                                    &triangle.world_c, &triangle.normal }) {
            mix_float(result.input_hash, point->x());
            mix_float(result.input_hash, point->y());
            mix_float(result.input_hash, point->z());
        }
        mix_color(result.input_hash, triangle.source_color);
        mix_float(result.input_hash, triangle.area);
    }
    mix_hash(result.input_hash, color_groups.size());
    for (const ColorGroup& group : color_groups) {
        mix_hash(result.input_hash, group.component_id);
        mix_color(result.input_hash, group.color);
    }
    mix_hash(result.input_hash, group_triangles.size());
    for (const auto& group : group_triangles) {
        mix_hash(result.input_hash, group.size());
        for (size_t triangle : group)
            mix_hash(result.input_hash, triangle);
    }
    mix_hash(result.input_hash, group_areas.size());
    for (double area : group_areas)
        mix_hash(result.input_hash, std::hash<double>{}(area));

    result.seed_count = seeds.size();
    result.output_hash = size_t(0x165667b1U);
    mix_hash(result.output_hash, fragment_count);
    mix_hash(result.output_hash, seeds.size());
    bool valid = source_ids_valid && fragment_count != 0 && seeds.size() == fragment_count &&
        geometry.size() == m_triangle_selectors.size() && fragment_for_triangle.size() == work_triangles.size() &&
        fragment_neighbours.size() == fragment_count && fragment_boundary_distance.size() == fragment_count &&
        fragment_boundary_neighbor.size() == fragment_count;
    for (size_t index = 0; index < seeds.size(); ++index) {
        const FragmentSeed& seed = seeds[index];
        mix_float(result.output_hash, seed.position.x());
        mix_float(result.output_hash, seed.position.y());
        mix_float(result.output_hash, seed.position.z());
        mix_hash(result.output_hash, seed.color_group);
        mix_hash(result.output_hash, seed.stable_id);
        if (seed.color_group >= color_groups.size())
            valid = false;
    }
    for (size_t volume = 0; volume < geometry.size(); ++volume) {
        const auto& per_volume = geometry[volume];
        mix_hash(result.output_hash, per_volume.size());
        if (per_volume.size() != fragment_count)
            valid = false;
        for (const CpuGeometry& data : per_volume) {
            mix_hash(result.output_hash, data.surface.vertices.size());
            mix_hash(result.output_hash, data.surface.indices.size());
            mix_hash(result.output_hash, data.source_facet_ids.size());
            if (!data.empty())
                ++result.nonempty_geometry_count;
            if (data.source_facet_ids.size() != data.surface.indices.size())
                valid = false;
            for (const Vec3f& vertex : data.surface.vertices) {
                mix_float(result.output_hash, vertex.x());
                mix_float(result.output_hash, vertex.y());
                mix_float(result.output_hash, vertex.z());
            }
            for (size_t face_index = 0; face_index < data.surface.indices.size(); ++face_index) {
                const stl_triangle_vertex_indices& face = data.surface.indices[face_index];
                if (face_index < data.source_facet_ids.size()) {
                    const int source_facet_id = data.source_facet_ids[face_index];
                    mix_hash(result.output_hash, size_t(source_facet_id));
                    if (source_facet_id < 0 || volume >= source_facets_by_mesh.size() ||
                        !source_facets_by_mesh[volume].count(source_facet_id))
                        valid = false;
                }
                for (int vertex : face) {
                    mix_hash(result.output_hash, size_t(vertex));
                    if (vertex < 0 || size_t(vertex) >= data.surface.vertices.size())
                        valid = false;
                }
            }
        }
    }
    for (size_t triangle = 0; triangle < fragment_for_triangle.size(); ++triangle) {
        const size_t fragment = fragment_for_triangle[triangle];
        mix_hash(result.output_hash, fragment);
        if (fragment == fragment_count)
            continue;
        if (fragment >= fragment_count || seeds[fragment].color_group != work_triangles[triangle].color_group)
            valid = false;
        else
            ++result.assigned_triangle_count;
    }
    for (size_t fragment = 0; fragment < fragment_neighbours.size(); ++fragment) {
        mix_hash(result.output_hash, fragment_neighbours[fragment].size());
        for (size_t neighbour : fragment_neighbours[fragment]) {
            mix_hash(result.output_hash, neighbour);
            if (neighbour >= fragment_count || neighbour == fragment ||
                !fragment_neighbours[neighbour].count(fragment))
                valid = false;
        }
        mix_float(result.output_hash, fragment_boundary_distance[fragment]);
        mix_hash(result.output_hash, fragment_boundary_neighbor[fragment]);
        if (fragment_boundary_neighbor[fragment] > color_groups.size())
            valid = false;
    }
    valid = valid && result.assigned_triangle_count == work_triangles.size();
    result.valid = valid;
    return result;
}

GLGizmoTracery::FragmentColorAssignmentIntegrity GLGizmoTracery::inspect_fragment_color_assignment(
    const std::vector<ColorGroup>& color_groups, const std::vector<FragmentSeed>& seeds,
    const std::vector<std::set<size_t>>& fragment_neighbours,
    const std::vector<float>& fragment_boundary_distance,
    const std::vector<size_t>& fragment_boundary_neighbor,
    size_t fragment_count, float target_edge, float refraction_strength,
    const std::vector<ColorRGBA>& fragment_colors) const
{
    const auto mix_hash = [](size_t& hash, size_t value) {
        hash = (hash * size_t(0x9e3779b1U)) ^ value;
    };
    const auto mix_float = [&mix_hash](size_t& hash, float value) {
        mix_hash(hash, std::hash<float>{}(value));
    };
    const auto mix_color = [&mix_float](size_t& hash, const ColorRGBA& color) {
        mix_float(hash, color.r());
        mix_float(hash, color.g());
        mix_float(hash, color.b());
        mix_float(hash, color.a());
    };

    FragmentColorAssignmentIntegrity result;
    result.input_hash = size_t(0x9e3779b9U);
    mix_hash(result.input_hash, fragment_count);
    mix_float(result.input_hash, target_edge);
    mix_float(result.input_hash, refraction_strength);
    mix_hash(result.input_hash, color_groups.size());
    for (const ColorGroup& group : color_groups) {
        mix_hash(result.input_hash, group.component_id);
        mix_color(result.input_hash, group.color);
    }
    mix_hash(result.input_hash, seeds.size());
    for (const FragmentSeed& seed : seeds) {
        mix_float(result.input_hash, seed.position.x());
        mix_float(result.input_hash, seed.position.y());
        mix_float(result.input_hash, seed.position.z());
        mix_hash(result.input_hash, seed.color_group);
        mix_hash(result.input_hash, seed.stable_id);
    }
    mix_hash(result.input_hash, fragment_neighbours.size());
    for (const auto& neighbours : fragment_neighbours) {
        mix_hash(result.input_hash, neighbours.size());
        for (size_t neighbour : neighbours)
            mix_hash(result.input_hash, neighbour);
    }
    mix_hash(result.input_hash, fragment_boundary_distance.size());
    for (float distance : fragment_boundary_distance)
        mix_float(result.input_hash, distance);
    mix_hash(result.input_hash, fragment_boundary_neighbor.size());
    for (size_t neighbour : fragment_boundary_neighbor)
        mix_hash(result.input_hash, neighbour);

    result.color_count = fragment_colors.size();
    result.output_hash = size_t(0x85ebca77U);
    mix_hash(result.output_hash, fragment_colors.size());
    bool valid = seeds.size() == fragment_count && fragment_neighbours.size() == fragment_count &&
        fragment_boundary_distance.size() == fragment_count && fragment_boundary_neighbor.size() == fragment_count &&
        fragment_colors.size() == fragment_count;
    for (size_t fragment = 0; fragment < seeds.size(); ++fragment) {
        if (seeds[fragment].color_group >= color_groups.size())
            valid = false;
        if (fragment >= fragment_neighbours.size() || fragment >= fragment_boundary_neighbor.size()) {
            valid = false;
            continue;
        }
        for (size_t neighbour : fragment_neighbours[fragment])
            if (neighbour >= fragment_count || neighbour >= fragment_neighbours.size() || neighbour == fragment ||
                !fragment_neighbours[neighbour].count(fragment))
                valid = false;
        if (fragment_boundary_neighbor[fragment] > color_groups.size())
            valid = false;
    }
    for (const ColorRGBA& color : fragment_colors) {
        mix_color(result.output_hash, color);
        if (!std::isfinite(color.r()) || !std::isfinite(color.g()) || !std::isfinite(color.b()) || !std::isfinite(color.a()) ||
            color.r() < 0.0f || color.r() > 1.0f || color.g() < 0.0f || color.g() > 1.0f ||
            color.b() < 0.0f || color.b() > 1.0f || color.a() != 1.0f)
            valid = false;
    }
    result.valid = valid;
    return result;
}

GLGizmoTracery::PublishGeneratedFragmentsIntegrity GLGizmoTracery::inspect_published_fragments(size_t fragment_count) const
{
    const auto mix_hash = [](size_t& hash, size_t value) {
        hash = (hash * size_t(0x9e3779b1U)) ^ value;
    };
    const auto mix_float = [&mix_hash](size_t& hash, float value) { mix_hash(hash, std::hash<float>{}(value)); };
    PublishGeneratedFragmentsIntegrity result;
    result.published_hash = size_t(0xc2b2ae3dU);
    mix_hash(result.published_hash, m_generated_fragments.size());
    result.valid = true;
    for (size_t volume = 0; volume < m_generated_fragments.size(); ++volume) {
        const auto& fragments = m_generated_fragments[volume];
        mix_hash(result.published_hash, fragments.size());
        size_t previous = 0;
        bool has_previous = false;
        for (const GeneratedFragment& fragment : fragments) {
            mix_hash(result.published_hash, volume);
            mix_hash(result.published_hash, fragment.fragment_id);
            mix_hash(result.published_hash, fragment.surface.vertices.size());
            mix_hash(result.published_hash, fragment.surface.indices.size());
            mix_hash(result.published_hash, fragment.source_facet_ids.size());
            ++result.published_fragments;
            result.published_faces += fragment.surface.indices.size();
            if (fragment.fragment_id >= fragment_count || fragment.surface.indices.empty() ||
                fragment.source_facet_ids.size() != fragment.surface.indices.size() ||
                (has_previous && fragment.fragment_id <= previous))
                result.valid = false;
            previous = fragment.fragment_id;
            has_previous = true;
            for (const Vec3f& vertex : fragment.surface.vertices) {
                mix_float(result.published_hash, vertex.x());
                mix_float(result.published_hash, vertex.y());
                mix_float(result.published_hash, vertex.z());
            }
            for (size_t face_index = 0; face_index < fragment.surface.indices.size(); ++face_index) {
                const stl_triangle_vertex_indices& face = fragment.surface.indices[face_index];
                if (face_index < fragment.source_facet_ids.size()) {
                    const int source_facet_id = fragment.source_facet_ids[face_index];
                    mix_hash(result.published_hash, size_t(source_facet_id));
                    if (source_facet_id < 0)
                        result.valid = false;
                }
                for (int vertex : face) {
                    mix_hash(result.published_hash, size_t(vertex));
                    if (vertex < 0 || size_t(vertex) >= fragment.surface.vertices.size())
                        result.valid = false;
                }
            }
        }
    }
    return result;
}

bool GLGizmoTracery::generate_fragments(
    const std::vector<WorkTriangle>& work_triangles, const std::vector<ColorGroup>& color_groups,
    const std::vector<std::vector<size_t>>& group_triangles, const std::vector<double>& group_areas,
    float target_edge, float maximum_fragment_diameter,
    std::map<EdgeKey, size_t>& work_edge_owners, size_t& fragment_count, std::vector<FragmentSeed>& seeds,
    std::vector<std::vector<CpuGeometry>>& geometry, std::vector<size_t>& fragment_for_triangle,
    std::vector<std::set<size_t>>& fragment_neighbours, std::vector<float>& fragment_boundary_distance,
    std::vector<size_t>& fragment_boundary_neighbor, bool verify_integrity,
    FragmentGenerationIntegrity* builder_integrity)
{
    const size_t input_hash_before = verify_integrity ? inspect_fragment_generation(
        work_triangles, color_groups, group_triangles, group_areas, target_edge, 0, seeds, geometry,
        fragment_for_triangle, fragment_neighbours, fragment_boundary_distance, fragment_boundary_neighbor).input_hash : 0;
    std::vector<size_t> group_fragment_counts(color_groups.size(), 0);
    const size_t minimum_fragments_per_region = std::clamp<size_t>(
        size_t(std::lround(8.0f + 16.0f * m_fragment_density)), 8, 24);
    for (size_t group = 0; group < group_triangles.size(); ++group) {
        if (group_triangles[group].empty())
            continue;
        // Each disconnected colour region owns its fragment budget. Adding a
        // different island cannot take fragments away from an existing one.
        // Never collapse a small brush stroke into a single solid patch:
        // otherwise there is no stained-glass pattern to see at all. Larger
        // regions receive extra fragments by area, while the fine temporary
        // mesh remains the upper bound on useful seeds.
        const double maximum_piece_area = std::max(0.0001, 0.70 * double(maximum_fragment_diameter) * double(maximum_fragment_diameter));
        const size_t area_count = std::max<size_t>(1, size_t(std::ceil(group_areas[group] / maximum_piece_area)));
        group_fragment_counts[group] = std::min(group_triangles[group].size(),
            std::max(minimum_fragments_per_region, area_count));
    }
    fragment_count = 0;
    for (size_t count : group_fragment_counts)
        fragment_count += count;
    // A full-surface bucket fill must not allocate an unbounded number of
    // separate GL meshes. Preserve every colour island while scaling only
    // exceptionally large fragment budgets to an interactive upper bound.
    constexpr size_t max_fragments = 2048;
    if (fragment_count > max_fragments && color_groups.size() <= max_fragments) {
        const double scale = double(max_fragments) / double(fragment_count);
        fragment_count = 0;
        for (size_t &count : group_fragment_counts) {
            if (count > 0) count = std::max<size_t>(1, size_t(double(count) * scale));
            fragment_count += count;
        }
        while (fragment_count > max_fragments) {
            auto largest = std::max_element(group_fragment_counts.begin(), group_fragment_counts.end());
            if (*largest <= 1) break;
            --*largest;
            --fragment_count;
        }
    }

    seeds.clear();
    seeds.reserve(fragment_count);
    for (size_t group = 0; group < group_triangles.size(); ++group) {
        const std::vector<size_t>& triangles = group_triangles[group];
        if (triangles.empty())
            continue;
        const size_t sample_count = std::min<size_t>(triangles.size(), 4096);
        std::vector<size_t> samples(sample_count);
        std::vector<float> nearest_distances(sample_count, std::numeric_limits<float>::max());
        for (size_t sample = 0; sample < sample_count; ++sample)
            samples[sample] = triangles[sample * triangles.size() / sample_count];
        for (size_t fragment = 0; fragment < group_fragment_counts[group]; ++fragment) {
            const ColorGroup& color_group = color_groups[group];
            const size_t stable_id = (color_group.component_id * size_t(2654435761U)) ^
                (size_t(std::lround(color_group.color.r() * 255.0f)) << 24) ^
                (size_t(std::lround(color_group.color.g() * 255.0f)) << 16) ^
                (size_t(std::lround(color_group.color.b() * 255.0f)) << 8) ^ fragment ^
                (m_regeneration_version * size_t(0x85ebca6bU));
            // Each new seed updates the cached distance of every candidate.
            // Rechecking every earlier seed made a large bucket fill quadratic
            // in the fragment count and blocked the UI.
            if (fragment == 0) {
                const WorkTriangle& triangle = work_triangles[samples[stable_id % sample_count]];
                seeds.emplace_back(FragmentSeed{ (triangle.world_a + triangle.world_b + triangle.world_c) / 3.0f, group, stable_id });
                continue;
            }
            size_t selected = samples[0];
            float best_distance = -1.0f;
            for (size_t sample = 0; sample < sample_count; ++sample) {
                const WorkTriangle& candidate = work_triangles[samples[sample]];
                const Vec3f position = (candidate.world_a + candidate.world_b + candidate.world_c) / 3.0f;
                nearest_distances[sample] = std::min(nearest_distances[sample], (position - seeds.back().position).squaredNorm());
                if (nearest_distances[sample] > best_distance) {
                    best_distance = nearest_distances[sample];
                    selected = samples[sample];
                }
            }
            const WorkTriangle& triangle = work_triangles[selected];
            seeds.emplace_back(FragmentSeed{ (triangle.world_a + triangle.world_b + triangle.world_c) / 3.0f, group, stable_id });
        }
    }

    fragment_count = seeds.size();
    if (fragment_count == 0) {
        return false;
    }

    geometry.assign(m_triangle_selectors.size(), std::vector<CpuGeometry>(fragment_count));

    struct WarpComputation {
        Vec3f result;
        std::array<float, 3> phase;
        std::array<float, 3> sine;
        std::array<float, 3> offset;
    };
    const float warp_frequency = 1.35f / std::max(target_edge, 0.001f);
    const float warp_amount = 0.f; // Preserve straight Voronoi bisectors.
    const auto compute_warp = [warp_frequency, warp_amount](const Vec3f& point) {
        // Keep the diagnostic fields, but use an identity transform so the
        // generated planar bisectors remain straight.
        WarpComputation calculation;
        calculation.phase = {
            (point.y() + 0.31f * point.z()) * warp_frequency,
            (point.z() + 0.27f * point.x()) * warp_frequency + 1.7f,
            (point.x() + 0.23f * point.y()) * warp_frequency + 3.1f
        };
        calculation.sine = {
            std::sin(calculation.phase[0]), std::sin(calculation.phase[1]), std::sin(calculation.phase[2])
        };
        calculation.offset = {
            warp_amount * calculation.sine[0], warp_amount * calculation.sine[1], warp_amount * calculation.sine[2]
        };
        calculation.result = point + Vec3f(calculation.offset[0], calculation.offset[1], calculation.offset[2]);
        return calculation;
    };
    const auto warped_position = [&compute_warp](const Vec3f& point) { return compute_warp(point).result; };

#ifndef NDEBUG
    // This is deliberately a fixed, small probe. It records the complete
    // floating-point path before any KD-tree operation while leaving the
    // generation algorithm and its inputs untouched.
    const bool trace_warp_probe = m_display_diagnostics_active;
    const auto float_bits = [](float value) {
        uint32_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value));
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    };
    const auto append_float = [&float_bits](std::ostringstream& stream, const char* name, float value) {
        stream << name << '=' << std::setprecision(9) << value << "(0x" << std::hex << float_bits(value) << std::dec << ')';
    };
    const auto append_vector = [&append_float](std::ostringstream& stream, const char* name, const Vec3f& value) {
        stream << name << "={";
        append_float(stream, "x", value.x()); stream << ',';
        append_float(stream, "y", value.y()); stream << ',';
        append_float(stream, "z", value.z()); stream << '}';
    };
    const auto describe_warp = [&append_float, &append_vector](const Vec3f& point, const WarpComputation& calculation) {
        std::ostringstream stream;
        append_vector(stream, "point", point); stream << ',';
        append_float(stream, "phase_x", calculation.phase[0]); stream << ',';
        append_float(stream, "phase_y", calculation.phase[1]); stream << ',';
        append_float(stream, "phase_z", calculation.phase[2]); stream << ',';
        append_float(stream, "sin_x", calculation.sine[0]); stream << ',';
        append_float(stream, "sin_y", calculation.sine[1]); stream << ',';
        append_float(stream, "sin_z", calculation.sine[2]); stream << ',';
        append_float(stream, "offset_x", calculation.offset[0]); stream << ',';
        append_float(stream, "offset_y", calculation.offset[1]); stream << ',';
        append_float(stream, "offset_z", calculation.offset[2]); stream << ',';
        append_vector(stream, "result", calculation.result);
        return stream.str();
    };
#endif
#ifndef NDEBUG
    if (trace_warp_probe) {
        std::ostringstream parameters;
        append_float(parameters, "fragment_density", m_fragment_density); parameters << ',';
        append_float(parameters, "target_edge", target_edge); parameters << ',';
        append_float(parameters, "warp_frequency", warp_frequency); parameters << ',';
        append_float(parameters, "warp_amount", warp_amount);
        record_display_diagnostic_state("warp_probe_parameters", parameters.str());
    }
#endif

    std::vector<Vec3f> warped_seeds;
    warped_seeds.reserve(seeds.size());
#ifndef NDEBUG
    std::array<WarpComputation, 2> seed_probe_calculations;
    const size_t seed_probe_count = std::min<size_t>(seed_probe_calculations.size(), seeds.size());
#endif
    for (size_t seed = 0; seed < seeds.size(); ++seed) {
        const WarpComputation calculation = compute_warp(seeds[seed].position);
#ifndef NDEBUG
        if (seed < seed_probe_count) {
            seed_probe_calculations[seed] = calculation;
            if (trace_warp_probe) {
                const WarpComputation repeat = compute_warp(seeds[seed].position);
                record_display_diagnostic_state("warp_probe_seed_compute",
                    "seed=" + std::to_string(seed) + ",same_result=" +
                    std::to_string(float_bits(calculation.result.x()) == float_bits(repeat.result.x()) &&
                                   float_bits(calculation.result.y()) == float_bits(repeat.result.y()) &&
                                   float_bits(calculation.result.z()) == float_bits(repeat.result.z())) + "," +
                    describe_warp(seeds[seed].position, calculation));
            }
        }
#endif
        warped_seeds.emplace_back(calculation.result);
    }
#ifndef NDEBUG
    if (trace_warp_probe) {
        for (size_t seed = 0; seed < seed_probe_count; ++seed) {
            std::ostringstream stored;
            append_vector(stored, "expected", seed_probe_calculations[seed].result); stored << ',';
            append_vector(stored, "stored", warped_seeds[seed]);
            record_display_diagnostic_state("warp_probe_seed_storage",
                "seed=" + std::to_string(seed) + ",same_result=" +
                std::to_string(float_bits(seed_probe_calculations[seed].result.x()) == float_bits(warped_seeds[seed].x()) &&
                               float_bits(seed_probe_calculations[seed].result.y()) == float_bits(warped_seeds[seed].y()) &&
                               float_bits(seed_probe_calculations[seed].result.z()) == float_bits(warped_seeds[seed].z())) + "," + stored.str());
        }
    }
#endif

    const auto seed_coordinate = [&warped_seeds](size_t index, size_t dimension) {
        return warped_seeds[index][dimension];
    };
    using SeedTree = KDTreeIndirect<3, float, decltype(seed_coordinate)>;
    std::vector<SeedTree> group_seed_trees;
    std::vector<std::vector<size_t>> group_seed_indices(group_triangles.size());
    group_seed_trees.reserve(group_triangles.size());
    for (size_t group = 0; group < group_triangles.size(); ++group) {
        std::vector<size_t>& indices = group_seed_indices[group];
        for (size_t index = 0; index < seeds.size(); ++index)
            if (seeds[index].color_group == group)
                indices.emplace_back(index);
        group_seed_trees.emplace_back(seed_coordinate, indices);
    }
#ifndef NDEBUG
    if (trace_warp_probe) {
        for (size_t seed = 0; seed < seed_probe_count; ++seed) {
            std::ostringstream post_tree;
            append_vector(post_tree, "expected", seed_probe_calculations[seed].result); post_tree << ',';
            append_vector(post_tree, "after_kd_tree", warped_seeds[seed]);
            record_display_diagnostic_state("warp_probe_seed_post_kd",
                "seed=" + std::to_string(seed) + ",same_result=" +
                std::to_string(float_bits(seed_probe_calculations[seed].result.x()) == float_bits(warped_seeds[seed].x()) &&
                               float_bits(seed_probe_calculations[seed].result.y()) == float_bits(warped_seeds[seed].y()) &&
                               float_bits(seed_probe_calculations[seed].result.z()) == float_bits(warped_seeds[seed].z())) + "," + post_tree.str());
        }
        if (!work_triangles.empty()) {
            const WorkTriangle& triangle = work_triangles.front();
            const WarpComputation first = compute_warp(triangle.world_a);
            const WarpComputation repeat = compute_warp(triangle.world_a);
            record_display_diagnostic_state("warp_probe_work_vertex",
                "triangle=0,vertex=a,same_result=" +
                std::to_string(float_bits(first.result.x()) == float_bits(repeat.result.x()) &&
                               float_bits(first.result.y()) == float_bits(repeat.result.y()) &&
                               float_bits(first.result.z()) == float_bits(repeat.result.z())) + "," +
                describe_warp(triangle.world_a, first));
        }
    }
#endif

    struct ClipVertex {
        Vec3f local;
        Vec3f world;
        Vec3f warped;
    };
    const auto interpolate_vertex = [](const ClipVertex& first, const ClipVertex& second, float t) {
        return ClipVertex{
            first.local + (second.local - first.local) * t,
            first.world + (second.world - first.world) * t,
            first.warped + (second.warped - first.warped) * t
        };
    };
    const auto clip_to_half_plane = [&interpolate_vertex](const std::vector<ClipVertex>& polygon,
                                                            const Vec3f& direction, float limit) {
        std::vector<ClipVertex> result;
        if (polygon.empty())
            return result;
        result.reserve(polygon.size() + 1);
        for (size_t index = 0; index < polygon.size(); ++index) {
            const ClipVertex& first = polygon[index];
            const ClipVertex& second = polygon[(index + 1) % polygon.size()];
            const float first_value = first.warped.dot(direction) - limit;
            const float second_value = second.warped.dot(direction) - limit;
            const bool first_inside = first_value <= 0.0f;
            const bool second_inside = second_value <= 0.0f;
            if (first_inside)
                result.emplace_back(first);
            if (first_inside != second_inside) {
                const float t = first_value / (first_value - second_value);
                result.emplace_back(interpolate_vertex(first, second, std::clamp(t, 0.0f, 1.0f)));
            }
        }
        return result;
    };

    fragment_for_triangle.assign(work_triangles.size(), fragment_count);
    for (size_t triangle_index = 0; triangle_index < work_triangles.size(); ++triangle_index) {
        const WorkTriangle& triangle = work_triangles[triangle_index];
        if (triangle.mesh_idx >= geometry.size() || triangle.color_group >= group_seed_trees.size() ||
            group_seed_indices[triangle.color_group].empty())
            continue;
        const std::array<ClipVertex, 3> vertices = {{
            { triangle.a, triangle.world_a, warped_position(triangle.world_a) },
            { triangle.b, triangle.world_b, warped_position(triangle.world_b) },
            { triangle.c, triangle.world_c, warped_position(triangle.world_c) }
        }};
        const Vec3f warped_center = (vertices[0].warped + vertices[1].warped + vertices[2].warped) / 3.0f;
        const SeedTree& seed_tree = group_seed_trees[triangle.color_group];
        size_t nearest = find_closest_point(seed_tree, warped_center);
        if (nearest >= fragment_count || seeds[nearest].color_group != triangle.color_group) {
            float best_distance = std::numeric_limits<float>::max();
            nearest = fragment_count;
            for (size_t index : group_seed_indices[triangle.color_group]) {
                const float distance = (warped_seeds[index] - warped_center).squaredNorm();
                if (distance < best_distance) {
                    best_distance = distance;
                    nearest = index;
                }
            }
            if (nearest == fragment_count)
                continue;
        }
        fragment_for_triangle[triangle_index] = nearest;

        // If the centre's nearest seed is distance d away and the triangle's
        // radius is r, no seed farther than d + 2r can own any part of it.
        // Query that exact neighbourhood instead of comparing every triangle
        // with every seed. All candidates still share the same clipped border.
        const float triangle_radius = std::max({
            (vertices[0].warped - warped_center).norm(),
            (vertices[1].warped - warped_center).norm(),
            (vertices[2].warped - warped_center).norm()
        });
        const float candidate_radius = (warped_seeds[nearest] - warped_center).norm() + 2.0f * triangle_radius + 0.0001f;
        std::vector<size_t> candidates = find_nearby_points(seed_tree, warped_center, candidate_radius);
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](size_t index) {
            return index >= fragment_count || seeds[index].color_group != triangle.color_group;
        }), candidates.end());
        candidates.emplace_back(nearest);
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
        for (const size_t candidate : candidates) {
            std::vector<ClipVertex> polygon(vertices.begin(), vertices.end());
            for (const size_t other : candidates) {
                if (other == candidate || polygon.empty())
                    continue;
                const Vec3f direction = warped_seeds[other] - warped_seeds[candidate];
                const float limit = 0.5f * (warped_seeds[other].squaredNorm() - warped_seeds[candidate].squaredNorm());
                polygon = clip_to_half_plane(polygon, direction, limit);
            }
            if (polygon.size() < 3)
                continue;
            CpuGeometry& data = geometry[triangle.mesh_idx][candidate];
            const unsigned int first = unsigned(data.surface.vertices.size());
            for (const ClipVertex& vertex : polygon)
                data.add_vertex(vertex.local);
            for (unsigned int index = 1; index + 1 < polygon.size(); ++index)
                data.add_triangle(first, first + index, first + index + 1, triangle.original_facet_id);
        }
    }

    struct BoundarySample {
        size_t first_triangle;
        size_t second_triangle;
    };
    // Reuse the region edge map, preserving the same object lifetime as the
    // pre-split path while rebuilding it for post-clipping adjacency.
    const auto quantize_point = [](const Vec3f& point) {
        return std::array<int64_t, 3>{
            int64_t(std::llround(point.x() * 100000.0f)),
            int64_t(std::llround(point.y() * 100000.0f)),
            int64_t(std::llround(point.z() * 100000.0f))
        };
    };
    const auto edge_key = [&quantize_point](size_t mesh_idx, const Vec3f& first, const Vec3f& second) {
        std::array<int64_t, 3> a = quantize_point(first);
        std::array<int64_t, 3> b = quantize_point(second);
        if (b < a)
            std::swap(a, b);
        return EdgeKey{ mesh_idx, { a[0], a[1], a[2], b[0], b[1], b[2] } };
    };
    std::vector<BoundarySample> boundaries;
    std::vector<std::vector<std::pair<size_t, float>>> work_adjacency(work_triangles.size());
    work_edge_owners.clear();
    for (size_t index = 0; index < work_triangles.size(); ++index) {
        const WorkTriangle& triangle = work_triangles[index];
        const std::array<EdgeKey, 3> local_edges = {
            edge_key(triangle.mesh_idx, triangle.a, triangle.b),
            edge_key(triangle.mesh_idx, triangle.b, triangle.c),
            edge_key(triangle.mesh_idx, triangle.c, triangle.a)
        };
        for (size_t edge_index = 0; edge_index < local_edges.size(); ++edge_index) {
            const auto owner = work_edge_owners.find(local_edges[edge_index]);
            if (owner == work_edge_owners.end()) {
                work_edge_owners.emplace(local_edges[edge_index], index);
            } else {
                const size_t other_index = owner->second;
                const WorkTriangle& other = work_triangles[other_index];
                const Vec3f center = (triangle.world_a + triangle.world_b + triangle.world_c) / 3.0f;
                const Vec3f other_center = (other.world_a + other.world_b + other.world_c) / 3.0f;
                const float step = std::max(0.0001f, (center - other_center).norm());
                work_adjacency[index].emplace_back(other_index, step);
                work_adjacency[other_index].emplace_back(index, step);
                if (other.color_group != triangle.color_group)
                    boundaries.push_back({ other_index, index });
            }
        }
    }

    // Approximate the visible fragment adjacency from adjacent source faces.
    // This catches normal shard boundaries without treating merely nearby,
    // folded model faces as neighbours.
    fragment_neighbours.assign(fragment_count, {});
    for (size_t triangle = 0; triangle < work_adjacency.size(); ++triangle) {
        const size_t fragment = fragment_for_triangle[triangle];
        if (fragment >= fragment_count)
            continue;
        for (const auto& adjacent : work_adjacency[triangle]) {
            const size_t other_fragment = fragment_for_triangle[adjacent.first];
            if (other_fragment < fragment_count && other_fragment != fragment)
                fragment_neighbours[fragment].insert(other_fragment);
        }
    }

    // Propagate each real colour boundary across only its own colour region.
    // This is a surface-distance field: folded or nearby model faces cannot
    // borrow colour through empty space.
    const size_t no_group = color_groups.size();
    std::vector<float> boundary_distance(work_triangles.size(), std::numeric_limits<float>::max());
    std::vector<size_t> boundary_neighbor(work_triangles.size(), no_group);
    using DistanceNode = std::pair<float, size_t>;
    std::priority_queue<DistanceNode, std::vector<DistanceNode>, std::greater<DistanceNode>> open_nodes;
    const auto seed_boundary = [&boundary_distance, &boundary_neighbor, &open_nodes](size_t triangle, size_t neighbor) {
        if (boundary_distance[triangle] > 0.0f) {
            boundary_distance[triangle] = 0.0f;
            boundary_neighbor[triangle] = neighbor;
            open_nodes.emplace(0.0f, triangle);
        }
    };
    for (const BoundarySample& boundary : boundaries) {
        seed_boundary(boundary.first_triangle, work_triangles[boundary.second_triangle].color_group);
        seed_boundary(boundary.second_triangle, work_triangles[boundary.first_triangle].color_group);
    }
    while (!open_nodes.empty()) {
        const auto [distance, triangle] = open_nodes.top();
        open_nodes.pop();
        if (distance != boundary_distance[triangle])
            continue;
        for (const auto& [next, step] : work_adjacency[triangle]) {
            if (work_triangles[next].color_group != work_triangles[triangle].color_group)
                continue;
            const float next_distance = distance + step;
            if (next_distance < boundary_distance[next]) {
                boundary_distance[next] = next_distance;
                boundary_neighbor[next] = boundary_neighbor[triangle];
                open_nodes.emplace(next_distance, next);
            }
        }
    }

    fragment_boundary_distance.assign(fragment_count, std::numeric_limits<float>::max());
    fragment_boundary_neighbor.assign(fragment_count, no_group);
    for (size_t triangle = 0; triangle < work_triangles.size(); ++triangle) {
        const size_t fragment = fragment_for_triangle[triangle];
        if (fragment == fragment_count || boundary_distance[triangle] >= fragment_boundary_distance[fragment])
            continue;
        fragment_boundary_distance[fragment] = boundary_distance[triangle];
        fragment_boundary_neighbor[fragment] = boundary_neighbor[triangle];
    }

    if (builder_integrity != nullptr) {
        if (verify_integrity) {
            *builder_integrity = inspect_fragment_generation(work_triangles, color_groups, group_triangles, group_areas,
                target_edge, fragment_count, seeds, geometry, fragment_for_triangle, fragment_neighbours,
                fragment_boundary_distance, fragment_boundary_neighbor);
            builder_integrity->immutable_input_preserved = builder_integrity->input_hash == input_hash_before;
            builder_integrity->valid = builder_integrity->valid && builder_integrity->immutable_input_preserved;
        } else {
            *builder_integrity = {};
        }
    }
    return true;
}

bool GLGizmoTracery::assign_fragment_colors(
    const std::vector<ColorGroup>& color_groups, const std::vector<FragmentSeed>& seeds,
    const std::vector<std::set<size_t>>& fragment_neighbours,
    const std::vector<float>& fragment_boundary_distance,
    const std::vector<size_t>& fragment_boundary_neighbor,
    size_t fragment_count, float target_edge, float refraction_strength,
    std::vector<ColorRGBA>& fragment_colors, bool verify_integrity,
    FragmentColorAssignmentIntegrity* builder_integrity)
{
    const size_t input_hash_before = verify_integrity ? inspect_fragment_color_assignment(
        color_groups, seeds, fragment_neighbours, fragment_boundary_distance, fragment_boundary_neighbor,
        fragment_count, target_edge, refraction_strength, fragment_colors).input_hash : 0;
    const size_t no_group = color_groups.size();
    std::vector<ColorRGBA> base_colors;
    base_colors.reserve(fragment_count);
    for (const FragmentSeed& seed : seeds)
        base_colors.emplace_back(color_groups[seed.color_group].color);
    const std::vector<TraceryEditorRecipe> editor_palette = build_tracery_editor_palette();
    if (editor_palette.empty())
        return false;
    std::vector<ColorRGBA> desired_colors(fragment_count);
    for (size_t fragment = 0; fragment < fragment_count; ++fragment) {
        ColorRGBA boundary_color = base_colors[fragment];
        const size_t neighbor = fragment_boundary_neighbor[fragment];
        if (neighbor != no_group)
            boundary_color = color_groups[neighbor].color;
        const float boundary_band = target_edge * 1.7f;
        const float linear_proximity = fragment_boundary_distance[fragment] < boundary_band ?
            1.0f - fragment_boundary_distance[fragment] / boundary_band : 0.0f;
        const float proximity = linear_proximity * linear_proximity * (3.0f - 2.0f * linear_proximity);
        const float boundary_mix = proximity * (0.16f + 0.42f * refraction_strength);
        desired_colors[fragment] = glass_color(base_colors[fragment], boundary_color, boundary_mix, seeds[fragment].stable_id);
    }

    struct CandidateRecipe {
        size_t palette_index;
        double delta_e;
        double balance_spread;
    };
    std::vector<std::vector<CandidateRecipe>> fragment_candidates(fragment_count);
    // Keep the same just-noticeable threshold used when reusing visual recipe
    // slots. Within this window, a more even material split is preferable to
    // a marginally closer preview colour.
    constexpr double balanced_recipe_delta_e_window = 2.0;
    for (size_t fragment = 0; fragment < fragment_count; ++fragment) {
        const wxColour target = tracery_wx_color(desired_colors[fragment]);
        auto& candidates = fragment_candidates[fragment];
        candidates.reserve(editor_palette.size());
        double minimum_delta_e = std::numeric_limits<double>::infinity();
        for (size_t recipe = 0; recipe < editor_palette.size(); ++recipe) {
            const double delta_e = color_delta_e00(target, editor_palette[recipe].preview);
            minimum_delta_e = std::min(minimum_delta_e, delta_e);
            candidates.push_back({ recipe, delta_e, tracery_recipe_balance_spread(editor_palette[recipe].weights) });
        }
        std::sort(candidates.begin(), candidates.end(), [minimum_delta_e, balanced_recipe_delta_e_window](const CandidateRecipe& first, const CandidateRecipe& second) {
            const bool first_is_visually_close = first.delta_e <= minimum_delta_e + balanced_recipe_delta_e_window;
            const bool second_is_visually_close = second.delta_e <= minimum_delta_e + balanced_recipe_delta_e_window;
            if (first_is_visually_close != second_is_visually_close)
                return first_is_visually_close;
            if (first_is_visually_close && first.balance_spread != second.balance_spread)
                return first.balance_spread < second.balance_spread;
            if (first.delta_e != second.delta_e)
                return first.delta_e < second.delta_e;
            return first.palette_index < second.palette_index;
        });
        if (candidates.size() > 24)
            candidates.resize(24);
    }

    std::vector<size_t> order(fragment_count);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&fragment_neighbours](size_t first, size_t second) {
        return fragment_neighbours[first].size() > fragment_neighbours[second].size();
    });
    std::vector<size_t> selected_recipe(fragment_count, size_t(-1));
    constexpr double neighbour_minimum_delta_e = 12.0;
    for (const size_t fragment : order) {
        size_t fallback = size_t(-1);
        double fallback_distance = -1.0;
        for (const CandidateRecipe& candidate : fragment_candidates[fragment]) {
            double minimum_distance = std::numeric_limits<double>::infinity();
            for (const size_t neighbour : fragment_neighbours[fragment])
                if (selected_recipe[neighbour] != size_t(-1))
                    minimum_distance = std::min(minimum_distance, color_delta_e00(
                        editor_palette[candidate.palette_index].preview, editor_palette[selected_recipe[neighbour]].preview));
            if (minimum_distance >= neighbour_minimum_delta_e) {
                selected_recipe[fragment] = candidate.palette_index;
                break;
            }
            if (minimum_distance > fallback_distance) {
                fallback = candidate.palette_index;
                fallback_distance = minimum_distance;
            }
        }
        // A colour family may genuinely be too narrow for the requested shard
        // density. Keep the most distinct legal in-family candidate instead of
        // escaping to an unrelated hue.
        if (selected_recipe[fragment] == size_t(-1))
            selected_recipe[fragment] = fallback == size_t(-1) ? fragment_candidates[fragment].front().palette_index : fallback;
    }

    // The slicer has a finite virtual-filament budget. Retain the common
    // editor recipes first, then project rare choices to the closest retained
    // legal colour instead of creating one material row per shard.
    constexpr size_t maximum_editor_recipes = 24;
    std::map<size_t, size_t> recipe_usage;
    for (const size_t recipe : selected_recipe)
        ++recipe_usage[recipe];
    if (recipe_usage.size() > maximum_editor_recipes) {
        std::vector<std::pair<size_t, size_t>> ranked(recipe_usage.begin(), recipe_usage.end());
        std::sort(ranked.begin(), ranked.end(), [](const auto& first, const auto& second) { return first.second > second.second; });
        std::set<size_t> retained;
        for (size_t index = 0; index < maximum_editor_recipes; ++index)
            retained.insert(ranked[index].first);
        for (size_t& recipe : selected_recipe) {
            if (retained.count(recipe))
                continue;
            const wxColour& source = editor_palette[recipe].preview;
            recipe = *std::min_element(retained.begin(), retained.end(), [&](size_t first, size_t second) {
                return color_delta_e00(source, editor_palette[first].preview) < color_delta_e00(source, editor_palette[second].preview);
            });
        }
    }

    fragment_colors.resize(fragment_count);
    const bool trace_color = tracery_color_trace_enabled();
    std::map<size_t, size_t> traced_group_fragments;
    size_t traced_fragments = 0;
    for (size_t fragment = 0; fragment < fragment_count; ++fragment) {
        const TraceryEditorRecipe& recipe = editor_palette[selected_recipe[fragment]];
        const wxColour& preview = recipe.preview;
        const ColorRGBA color(float(preview.Red()) / 255.0f, float(preview.Green()) / 255.0f,
            float(preview.Blue()) / 255.0f, 1.0f);
        fragment_colors[fragment] = color;
        if (trace_color && traced_fragments < 256 && traced_group_fragments[seeds[fragment].color_group]++ < 12) {
            std::fprintf(stderr, "TRACERY_COLOR fragment=%zu group=%zu seed=%zu base=%06X target=%06X preview=%06X palette=%zu delta_e=%.3f\n",
                fragment, seeds[fragment].color_group, seeds[fragment].stable_id, tracery_color_rgb(base_colors[fragment]),
                tracery_color_rgb(desired_colors[fragment]), tracery_color_rgb(color), selected_recipe[fragment],
                color_delta_e00(tracery_wx_color(desired_colors[fragment]), preview));
            ++traced_fragments;
        }
    }

    if (builder_integrity != nullptr) {
        if (verify_integrity) {
            *builder_integrity = inspect_fragment_color_assignment(color_groups, seeds, fragment_neighbours,
                fragment_boundary_distance, fragment_boundary_neighbor, fragment_count, target_edge,
                refraction_strength, fragment_colors);
            builder_integrity->immutable_input_preserved = builder_integrity->input_hash == input_hash_before;
            builder_integrity->valid = builder_integrity->valid && builder_integrity->immutable_input_preserved;
        } else {
            *builder_integrity = {};
        }
    }
    return true;
}

void GLGizmoTracery::publish_generated_fragments(
    std::vector<std::vector<CpuGeometry>>& geometry,
    const std::vector<Transform3d>& volume_transforms, size_t fragment_count,
    bool verify_integrity, PublishGeneratedFragmentsIntegrity* builder_integrity)
{
    const auto mix_hash = [](size_t& hash, size_t value) {
        hash = (hash * size_t(0x9e3779b1U)) ^ value;
    };
    const auto mix_float = [&mix_hash](size_t& hash, float value) { mix_hash(hash, std::hash<float>{}(value)); };
    PublishGeneratedFragmentsIntegrity expected;
    expected.expected_hash = size_t(0xc2b2ae3dU);
    mix_hash(expected.expected_hash, geometry.size());
    bool layout_valid = geometry.size() == volume_transforms.size() &&
        geometry.size() == m_generated_fragments.size() && m_fragment_colors.size() == fragment_count;
    for (const auto& fragments : m_generated_fragments)
        if (!fragments.empty())
            layout_valid = false;
    for (size_t volume = 0; volume < geometry.size(); ++volume) {
        if (geometry[volume].size() != fragment_count)
            layout_valid = false;
        size_t count = 0;
        for (const CpuGeometry& data : geometry[volume])
            if (!data.empty())
                ++count;
        mix_hash(expected.expected_hash, count);
        for (size_t fragment = 0; fragment < geometry[volume].size(); ++fragment) {
            const CpuGeometry& data = geometry[volume][fragment];
            if (data.empty())
                continue;
            mix_hash(expected.expected_hash, volume);
            mix_hash(expected.expected_hash, fragment);
            mix_hash(expected.expected_hash, data.surface.vertices.size());
            mix_hash(expected.expected_hash, data.surface.indices.size());
            mix_hash(expected.expected_hash, data.source_facet_ids.size());
            ++expected.expected_fragments;
            expected.expected_faces += data.surface.indices.size();
            if (data.source_facet_ids.size() != data.surface.indices.size())
                layout_valid = false;
            for (const Vec3f& vertex : data.surface.vertices) {
                mix_float(expected.expected_hash, vertex.x());
                mix_float(expected.expected_hash, vertex.y());
                mix_float(expected.expected_hash, vertex.z());
            }
            for (size_t face_index = 0; face_index < data.surface.indices.size(); ++face_index) {
                const stl_triangle_vertex_indices& face = data.surface.indices[face_index];
                if (face_index < data.source_facet_ids.size()) {
                    const int source_facet_id = data.source_facet_ids[face_index];
                    mix_hash(expected.expected_hash, size_t(source_facet_id));
                    if (source_facet_id < 0)
                        layout_valid = false;
                }
                for (int vertex : face) {
                    mix_hash(expected.expected_hash, size_t(vertex));
                    if (vertex < 0 || size_t(vertex) >= data.surface.vertices.size())
                        layout_valid = false;
                }
            }
        }
    }
    for (size_t fragment = 0; fragment < fragment_count; ++fragment) {
        for (size_t volume = 0; volume < geometry.size(); ++volume) {
            CpuGeometry& data = geometry[volume][fragment];
            if (data.empty())
                continue;
            indexed_triangle_set surface = std::move(data.surface);
            m_generated_fragments[volume].emplace_back(
                GeneratedFragment{ fragment, std::move(surface), std::move(data.source_facet_ids) });
        }
    }
    if (builder_integrity != nullptr) {
        if (verify_integrity) {
            const PublishGeneratedFragmentsIntegrity published = inspect_published_fragments(fragment_count);
            expected.published_hash = published.published_hash;
            expected.published_fragments = published.published_fragments;
            expected.published_faces = published.published_faces;
            expected.valid = layout_valid && published.valid &&
                expected.expected_hash == expected.published_hash &&
                expected.expected_fragments == expected.published_fragments &&
                expected.expected_faces == expected.published_faces;
            *builder_integrity = expected;
        } else {
            *builder_integrity = {};
        }
    }
}

void GLGizmoTracery::generate_paint_plan()
{
    TraceryPerformanceScope performance("vector_generation");
    performance.stage("generation.selection_policy");
    // Revalidate restored masks and changed filament presets before generation.
    // Back projections are generated later and never pass through this filter.
    refresh_source_selection_policy(true);
    m_regeneration_requested = false;
    if (!m_incremental_generation)
        m_tracery_selections.clear();
    m_generated_fragments.clear();
    m_generated_voronoi_regions.clear();
    m_fragment_colors.clear();
    m_fragment_mixed_filament_ids.clear();
    m_pending_selection_fragment_count = 0;
    m_transition_recipe_targets.clear();
    m_generation_integrity_valid = true;
    const ModelObject* model_object = m_c != nullptr && m_c->selection_info() != nullptr ?
        m_c->selection_info()->model_object() : nullptr;
    if (model_object == nullptr || model_object->id() != m_mask_object_id) {
        return;
    }

    if (m_active_stroke_erases) {
        performance.stage("generation.erase_projection");
        if (m_pending_selection_mode == PaintMode::THROUGH) {
            size_t index = 0;
            for (const ModelVolume* volume : model_object->volumes) {
                if (!volume->is_model_part()) continue;
                m_generated_voronoi_regions.resize(index + 1);
                if (index < m_previous_mask_data.size() && index < m_mask_data.size())
                    for (const auto& facet : TraceryDocument::erased_regions(volume->mesh(), m_previous_mask_data[index], m_mask_data[index]))
                        for (const auto& region : facet.regions) {
                            GeneratedVoronoiRegion erased;
                            erased.source_facet_id = facet.source_facet_id;
                            erased.mixed_filament_id = 1; // geometry only; erase restores base paint
                            for (const Vec2d& p : region.polygon)
                                erased.local_polygon.push_back((facet.origin + p.x() * facet.axis_u + p.y() * facet.axis_v).cast<float>());
                            m_generated_voronoi_regions[index].push_back(std::move(erased));
                        }
                ++index;
            }
        }
        return;
    }

    using SurfaceTriangle = GenerationInput::SurfaceTriangle;
    using ColorSource = GenerationInput::ColorSource;
    std::vector<ColorGroup> color_groups;
    std::vector<std::vector<std::unique_ptr<ColorSource>>> color_sources(m_triangle_selectors.size());
    std::vector<SurfaceTriangle> surface_triangles;
    std::vector<Transform3d> volume_transforms;
    double surface_area = 0.0;
    GenerationInput input{ model_object, surface_triangles, color_sources, volume_transforms, surface_area };

    performance.stage("generation.collect_input");
    if (!collect_generation_input(input))
        return;
    performance.count("surface_triangles", surface_triangles.size());
    performance.stage("generation.color_regions");
    std::vector<VoronoiColorRegion> regions;
    if (!build_color_regions_from_input_faces(input, color_groups, regions))
        return;
    const float maximum_fragment_diameter = (6.5f - 4.0f * m_fragment_density) * 1.35f;
    allocate_region_fragment_budgets(regions, maximum_fragment_diameter);
    std::vector<FragmentSeed> seeds;
    performance.stage("generation.seeds");
    generate_region_seeds(input, regions, seeds);
    performance.count("seeds", seeds.size());
    if (seeds.empty())
        return;
    std::vector<std::vector<GeneratedVoronoiRegion>> generated;
    performance.stage("generation.voronoi");
    generate_voronoi_regions(input, regions, seeds, generated);
    size_t generated_count = 0;
    for (const auto& volume : generated)
        generated_count += volume.size();
    performance.count("generated_regions", generated_count);
    performance.stage("generation.topology");
    std::vector<std::set<size_t>> fragment_neighbours;
    build_fragment_topology(generated, seeds.size(), fragment_neighbours);
    performance.stage("generation.colors");
    assign_voronoi_fragment_colors(color_groups, seeds, fragment_neighbours);
    m_pending_selection_fragment_count = m_fragment_colors.size();
    performance.stage("generation.publish");
    publish_generated_regions(std::move(generated));
}

void GLGizmoTracery::apply_stained_glass_colors(const std::function<void()>& rollback_preparation)
{
    TraceryPerformanceScope performance("apply");
    performance.stage("apply.compute");
    if (!m_generation_integrity_valid) {
        if (rollback_preparation)
            rollback_preparation();
        return;
    }

    // An incremental stroke fully inside an existing selection has no new
    // leaves to bake. Crucially, do not restore the base snapshot here.
    if (m_incremental_generation && m_fragment_colors.empty())
        return;

    const auto previous_colours = m_fragment_colors;
    const auto previous_regions = m_generated_voronoi_regions;
    const auto previous_targets = m_transition_recipe_targets;
    auto restore_pending = [&] {
        m_fragment_colors = previous_colours;
        m_generated_voronoi_regions = previous_regions;
        m_transition_recipe_targets = previous_targets;
    };
    bool commit_started = false;
    try {
        if (m_pending_selection_mode == PaintMode::THROUGH)
            generate_through_voronoi_regions();
        apply_incremental_boundary_transitions();
        auto plan = prepare_stained_glass_apply();
        if (!plan) {
            restore_pending();
            if (rollback_preparation)
                rollback_preparation();
            return;
        }
        // Restoring the base paint used to build geometry inside commit, after
        // the editor mask had changed. Prepare it here just like plate conversion
        // so an exception leaves all live volumes untouched.
        if (!plan->model && plan->mode != StainedGlassApplyPlan::Mode::ConvertPlate) {
            auto* object = find_masked_model_object(wxGetApp().plater()->model());
            if (!object)
                throw std::runtime_error("Stained-glass object is no longer available");
            size_t index = 0;
            for (ModelVolume* volume : object->volumes) {
                if (!volume->is_model_part())
                    continue;
                const bool has_regions = index < m_generated_voronoi_regions.size() && !m_generated_voronoi_regions[index].empty();
                if (has_regions || (!m_incremental_generation && index < m_stained_glass_base_paint_data.size()))
                    plan->partitions.push_back(prepare_volume_tracery_partition(
                        *volume, index, nullptr,
                        plan->mode == StainedGlassApplyPlan::Mode::BakeFragments ? &plan->fragment_mixed_filament_ids : nullptr));
                ++index;
            }
        }
        commit_started = true;
        commit_stained_glass_apply(std::move(*plan));
    } catch (const std::exception& error) {
        if (!commit_started) {
            restore_pending();
            if (rollback_preparation)
                rollback_preparation();
        }
        performance.mark_failed();
        performance.stage("apply.error_dialog_wait");
        wxMessageBox(from_u8(error.what()), _L("Stained glass"), wxOK | wxICON_ERROR);
    }
}

void GLGizmoTracery::apply_incremental_boundary_transitions()
{
    if (!m_incremental_generation || m_tracery_selections.empty() ||
        m_pending_selection_fragment_count == 0 || m_fragment_colors.empty())
        return;

    const float transition_width = std::clamp((6.5f - 4.0f * m_fragment_density) * 0.18f, 0.20f, 0.85f);
    const float transition_width_sq = transition_width * transition_width;
    Model& model = wxGetApp().plater()->model();
    const ModelObject* object = find_masked_model_object(model);
    if (object == nullptr)
        return;
    std::vector<const indexed_triangle_set*> volume_meshes;
    for (const ModelVolume* volume : object->volumes)
        if (volume->is_model_part())
            volume_meshes.emplace_back(&volume->mesh().its);
    std::vector<std::vector<Vec3i32>> volume_face_neighbours(volume_meshes.size());
    for (size_t volume = 0; volume < volume_meshes.size(); ++volume)
        volume_face_neighbours[volume] = its_face_neighbors(*volume_meshes[volume]);
    size_t compatible_facet_pairs = 0;
    size_t cross_facet_pairs = 0;
    const auto facets_share_transition_boundary = [&volume_meshes, &volume_face_neighbours,
                                                     &compatible_facet_pairs, &cross_facet_pairs](
                                                        size_t volume, int first_id, int second_id) {
        if (volume >= volume_meshes.size() || first_id < 0 || second_id < 0)
            return false;
        const indexed_triangle_set& mesh = *volume_meshes[volume];
        if (size_t(first_id) >= mesh.indices.size() || size_t(second_id) >= mesh.indices.size())
            return false;
        if (first_id == second_id) {
            ++compatible_facet_pairs;
            return true;
        }
        if (size_t(first_id) >= volume_face_neighbours[volume].size())
            return false;
        const Vec3i32 neighbours = volume_face_neighbours[volume][size_t(first_id)];
        if (neighbours[0] != second_id && neighbours[1] != second_id && neighbours[2] != second_id)
            return false;
        const stl_triangle_vertex_indices& first = mesh.indices[size_t(first_id)];
        const stl_triangle_vertex_indices& second = mesh.indices[size_t(second_id)];
        const auto valid = [&mesh](const stl_triangle_vertex_indices& face) {
            return face[0] >= 0 && face[1] >= 0 && face[2] >= 0 &&
                size_t(face[0]) < mesh.vertices.size() && size_t(face[1]) < mesh.vertices.size() &&
                size_t(face[2]) < mesh.vertices.size();
        };
        if (!valid(first) || !valid(second))
            return false;
        const Vec3f first_a = mesh.vertices[size_t(first[0])];
        const Vec3f first_b = mesh.vertices[size_t(first[1])];
        const Vec3f first_c = mesh.vertices[size_t(first[2])];
        const Vec3f second_a = mesh.vertices[size_t(second[0])];
        const Vec3f second_b = mesh.vertices[size_t(second[1])];
        const Vec3f second_c = mesh.vertices[size_t(second[2])];
        Vec3f first_normal = (first_b - first_a).cross(first_c - first_a);
        Vec3f second_normal = (second_b - second_a).cross(second_c - second_a);
        if (first_normal.squaredNorm() <= 1e-12f || second_normal.squaredNorm() <= 1e-12f)
            return false;
        first_normal.normalize();
        second_normal.normalize();
        if (first_normal.dot(second_normal) < 0.999f || std::abs((second_a - first_a).dot(first_normal)) > 1e-4f)
            return false;
        ++compatible_facet_pairs;
        ++cross_facet_pairs;
        return true;
    };
    const auto point_segment_distance_sq = [](const Vec3f& point, const Vec3f& first, const Vec3f& second) {
        const Vec3f edge = second - first;
        const float length_sq = edge.squaredNorm();
        if (length_sq <= 1e-12f)
            return (point - first).squaredNorm();
        const float factor = std::clamp((point - first).dot(edge) / length_sq, 0.0f, 1.0f);
        return (point - (first + edge * factor)).squaredNorm();
    };
    const auto regions_touch = [&point_segment_distance_sq, transition_width_sq, &facets_share_transition_boundary](
                                   size_t volume, const GeneratedVoronoiRegion& first, const GeneratedVoronoiRegion& second) {
        if (!facets_share_transition_boundary(volume, first.source_facet_id, second.source_facet_id) ||
            first.local_polygon.size() < 2 || second.local_polygon.size() < 2)
            return false;
        for (size_t first_index = 0; first_index < first.local_polygon.size(); ++first_index) {
            const Vec3f& first_a = first.local_polygon[first_index];
            const Vec3f& first_b = first.local_polygon[(first_index + 1) % first.local_polygon.size()];
            for (size_t second_index = 0; second_index < second.local_polygon.size(); ++second_index) {
                const Vec3f& second_a = second.local_polygon[second_index];
                const Vec3f& second_b = second.local_polygon[(second_index + 1) % second.local_polygon.size()];
                if (point_segment_distance_sq(first_a, second_a, second_b) <= transition_width_sq ||
                    point_segment_distance_sq(first_b, second_a, second_b) <= transition_width_sq ||
                    point_segment_distance_sq(second_a, first_a, first_b) <= transition_width_sq ||
                    point_segment_distance_sq(second_b, first_a, first_b) <= transition_width_sq)
                    return true;
            }
        }
        return false;
    };
    const auto blend_toward_neighbours = [](const ColorRGBA& base, const std::vector<ColorRGBA>& neighbours,
                                             float neighbour_weight) {
        Vec4f sum = Vec4f::Zero();
        for (const ColorRGBA& colour : neighbours)
            sum += Vec4f(colour.r(), colour.g(), colour.b(), colour.a());
        sum /= float(neighbours.size());
        const float base_weight = 1.0f - neighbour_weight;
        return ColorRGBA(base.r() * base_weight + sum.x() * neighbour_weight,
                          base.g() * base_weight + sum.y() * neighbour_weight,
                          base.b() * base_weight + sum.z() * neighbour_weight,
                          base.a() * base_weight + sum.w() * neighbour_weight);
    };

    std::map<size_t, std::vector<ColorRGBA>> new_neighbours;
    std::map<std::pair<size_t, size_t>, std::vector<ColorRGBA>> old_neighbours;
    for (size_t volume = 0; volume < m_generated_voronoi_regions.size(); ++volume) {
        for (const GeneratedVoronoiRegion& current : m_generated_voronoi_regions[volume]) {
            if (current.is_back_projection || current.fragment_id >= m_pending_selection_fragment_count ||
                current.fragment_id >= m_fragment_colors.size())
                continue;
            for (size_t selection_index = 0; selection_index < m_tracery_selections.size(); ++selection_index) {
                const TracerySelection& selection = m_tracery_selections[selection_index];
                if (volume >= selection.regions.size())
                    continue;
                for (const GeneratedVoronoiRegion& previous : selection.regions[volume]) {
                    if (previous.is_back_projection || previous.fragment_id >= selection.fragment_colors.size() ||
                        !regions_touch(volume, current, previous))
                        continue;
                    new_neighbours[current.fragment_id].push_back(selection.fragment_colors[previous.fragment_id]);
                    old_neighbours[{ selection_index, previous.fragment_id }].push_back(m_fragment_colors[current.fragment_id]);
                }
            }
        }
    }
    // A Voronoi fragment is discrete, not a texel. Give the first boundary
    // ring on each side a directional bias rather than painting both sides
    // with one 50:50 colour: old -> old-biased transition -> new-biased
    // transition -> new.
    constexpr float opposite_side_weight = 0.28f;
#ifndef NDEBUG
    const auto write_diagnostic = [this, transition_width, compatible_facet_pairs, cross_facet_pairs, opposite_side_weight,
                                   &new_neighbours, &old_neighbours]() {
        if (!m_display_diagnostics_active)
            return;
        const std::filesystem::path path = m_display_diagnostics_dir / "tracery_incremental_transitions.tsv";
        const bool new_file = !std::filesystem::exists(path);
        std::ofstream output(path, std::ios::app | std::ios::binary);
        if (!output)
            return;
        if (new_file)
            output << "new_boundary_fragments\told_boundary_fragments\ttransition_recipes\twidth\topposite_side_weight\tcompatible_facet_pairs\tcross_facet_pairs\n";
        output << new_neighbours.size() << '\t' << old_neighbours.size() << '\t'
               << old_neighbours.size() << '\t' << transition_width << '\t' << opposite_side_weight << '\t'
               << compatible_facet_pairs << '\t' << cross_facet_pairs << '\n';
    };
#else
    const auto write_diagnostic = [] {};
#endif
    if (new_neighbours.empty() || old_neighbours.empty()) {
        write_diagnostic();
        return;
    }

    for (const auto& [fragment, neighbours] : new_neighbours)
        if (fragment < m_fragment_colors.size())
            m_fragment_colors[fragment] = blend_toward_neighbours(m_fragment_colors[fragment], neighbours,
                                                                    opposite_side_weight);

    for (const auto& [target, neighbours] : old_neighbours) {
        const size_t selection_index = target.first;
        const size_t fragment = target.second;
        if (selection_index >= m_tracery_selections.size())
            continue;
        TracerySelection& selection = m_tracery_selections[selection_index];
        if (fragment >= selection.fragment_colors.size())
            continue;
        const ColorRGBA transition_colour = blend_toward_neighbours(selection.fragment_colors[fragment], neighbours,
                                                                       opposite_side_weight);
        m_fragment_colors.emplace_back(transition_colour);
        m_transition_recipe_targets.emplace_back(selection_index, fragment);
        const size_t transient_fragment = m_fragment_colors.size() - 1;
        for (size_t volume = 0; volume < selection.regions.size(); ++volume)
            for (const GeneratedVoronoiRegion& previous : selection.regions[volume])
                if (previous.fragment_id == fragment) {
                    GeneratedVoronoiRegion dirty = previous;
                    dirty.fragment_id = transient_fragment;
                    dirty.mixed_filament_id = 0;
                    m_generated_voronoi_regions.resize(std::max(m_generated_voronoi_regions.size(), volume + 1));
                    m_generated_voronoi_regions[volume].emplace_back(std::move(dirty));
                }
    }
    write_diagnostic();
}

std::optional<GLGizmoTracery::StainedGlassApplyPlan> GLGizmoTracery::prepare_stained_glass_apply(bool palette_only)
{
    const TraceryPerformanceScope performance("prepare");
    if (!palette_only && m_fragment_colors.empty()) {
        // An empty selector means the user erased the complete glass mask.
        // Commit the base snapshot so those facets return to their ordinary
        // paint rather than retaining the last generated leaf states.
        if (m_stained_glass_base_paint_data.empty())
            return std::nullopt;
        return StainedGlassApplyPlan{};
    }
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return std::nullopt;
    auto* colour_option = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
    if (colour_option == nullptr)
        return std::nullopt;
    StainedGlassApplyPlan plan;
    auto* plater = wxGetApp().plater();
    auto* plate = plater->get_partplate_list().get_curr_plate();
    const Model& source_model = plater->model();
    TraceryPlateInstances plate_instances;
    DynamicPrintConfig inherited = bundle->prints.get_edited_preset().config;
    inherited.apply(*plate->config());
    std::set<unsigned int> plate_material_ids;
    for (size_t oi = 0; oi < source_model.objects.size(); ++oi) {
        const auto* object = source_model.objects[oi];
        if (!object->printable) continue;
        for (size_t ii = 0; ii < object->instances.size(); ++ii)
            if (object->instances[ii]->printable && plate->contain_instance(int(oi), int(ii)))
                plate_instances[oi].insert(ii);
        if (plate_instances.count(oi)) {
            const auto ids = tracery_object_materials(*object, inherited);
            plate_material_ids.insert(ids.begin(), ids.end());
        }
    }
    // Editor restore data can refer to original colours no longer present in
    // the current baked mesh. Convert those too so erase/regenerate stays local.
    auto collect_paint = [&plate_material_ids](const auto& paint) {
        for (size_t id = 1; id < paint.used_states.size(); ++id)
            if (paint.used_states[id]) plate_material_ids.insert(unsigned(id));
    };
    for (const auto& [object_index, instances] : plate_instances)
        for (const auto* volume : source_model.objects[object_index]->volumes)
            if (!volume->tracery_edit_data.empty())
                collect_paint(TraceryDocument::decode_cached(volume->tracery_edit_data)->last_annotations()[5]);
    for (const auto& paint : m_stained_glass_base_paint_data) collect_paint(paint);
    for (const auto& [volume, state] : m_mesh_restore_states) {
        if (!state.original_annotations.empty()) collect_paint(state.original_annotations.front());
        if (!state.last_annotations.empty()) collect_paint(state.last_annotations.front());
    }
    if (const auto codes = source_model.plates_custom_gcodes.find(plate->get_index()); codes != source_model.plates_custom_gcodes.end())
        for (const auto& item : codes->second.gcodes)
            if (item.type == CustomGCode::Type::ToolChange && item.extruder > 0)
                plate_material_ids.insert(unsigned(item.extruder));
    const size_t old_physical_count = colour_option->values.size();

    const auto used_materials = tracery_plate_materials();
    const auto used_black = tracery_black_materials(*bundle, used_materials);
    if (used_black.size() > 1) {
        wxMessageBox(_L("More than one black material is used on this plate. Assign the black areas to one material before applying stained glass."),
                     _L("Stained glass"), wxOK | wxICON_WARNING);
        return std::nullopt;
    }
    const unsigned int black_id = used_black.empty() ? 0 : used_black.front();
    const auto material_mode = tracery_plate_material_mode(*bundle, black_id);
    std::vector<wxColour> base_colours = {wxColour("#08ABFB"), wxColour("#D93B90"), wxColour("#F9ED3D")};
    if (material_mode == TraceryMaterialMode::CMYW)
        base_colours.emplace_back("#FFFFFF");
    else if (material_mode == TraceryMaterialMode::CMYK)
        base_colours.emplace_back(colour_option->values[black_id - 1]);

    std::vector<std::string> projected_colours = colour_option->values;
    std::vector<unsigned int> base_ids;
    for (size_t i = 0; i < base_colours.size(); ++i) {
        if (i == 3 && material_mode == TraceryMaterialMode::CMYK) {
            base_ids.push_back(black_id);
            continue;
        }
        size_t index = 0;
        for (; index < projected_colours.size(); ++index)
            if (wxColour(projected_colours[index]) == base_colours[i] && tracery_filament_is_full_spectrum(*bundle, unsigned(index + 1)))
                break;
        // Prefer a calibrated preset, but reuse an exact existing colour when
        // that preset is unavailable. Never change a shared physical preset.
        if (index == projected_colours.size())
            for (index = 0; index < projected_colours.size(); ++index)
                if (wxColour(projected_colours[index]) == base_colours[i]) break;
        if (index == projected_colours.size()) {
            projected_colours.push_back(tracery_hex(base_colours[i]));
        }
        base_ids.push_back(unsigned(index + 1));
    }
    MixedFilamentManager projected_manager = bundle->mixed_filaments;
    projected_manager.auto_generate(projected_colours);
    // New physical slots may insert automatic rows. Preserve every pre-existing
    // recipe by stable identity before calculating the old-to-new ID mapping.
    for (auto& row : projected_manager.mixed_filaments())
        for (const auto& old : bundle->mixed_filaments.mixed_filaments())
            if (row.stable_id == old.stable_id) { row = old; break; }
    if (projected_colours.size() > MAXIMUM_EXTRUDER_NUMBER ||
        projected_manager.total_filaments(projected_colours.size()) > MAXIMUM_FILAMENT_NUMBER) {
        wxMessageBox(_L("There are not enough material slots for the stained-glass palette."),
                     _L("Stained glass"), wxOK | wxICON_WARNING);
        return std::nullopt;
    }
    auto& mixed_manager = projected_manager;
    std::vector<std::string> base_colour_strings;
    for (unsigned int id : base_ids)
        base_colour_strings.push_back(projected_colours[id - 1]);
    const size_t available_mixed = MAXIMUM_FILAMENT_NUMBER - mixed_manager.total_filaments(projected_colours.size());

    // A palette entry may resolve to an existing CMYW recipe, so do not cap
    // the candidates by free slots before recipe de-duplication.
    constexpr size_t palette_budget = 24;
    const std::vector<ColorRGBA> no_fragments;
    const auto& flower_colours = palette_only ? no_fragments : m_fragment_colors;

    // Keep the established publishing path: it owns the virtual-filament
    // budget, creates valid recipe rows and maps their returned IDs to model
    // facets. The editor only supplies its constrained preview colours.
    struct PaletteColour {
        wxColour colour;
        size_t usage = 0;
    };
    std::vector<PaletteColour> candidates;
    constexpr double merge_delta_e = 7.0;
    for (const ColorRGBA& source : flower_colours) {
        const wxColour colour = tracery_wx_color(source);
        size_t best = candidates.size();
        double best_distance = merge_delta_e;
        for (size_t index = 0; index < candidates.size(); ++index) {
            const double distance = color_delta_e00(colour, candidates[index].colour);
            if (distance < best_distance) {
                best = index;
                best_distance = distance;
            }
        }
        if (best == candidates.size())
            candidates.push_back({ colour, 1 });
        else
            ++candidates[best].usage;
    }
    std::sort(candidates.begin(), candidates.end(), [](const PaletteColour& first, const PaletteColour& second) {
        return first.usage > second.usage;
    });
    if (candidates.size() > palette_budget)
        candidates.resize(palette_budget);

    std::vector<size_t> fragment_palette(flower_colours.size(), 0);
    for (size_t fragment = 0; fragment < flower_colours.size(); ++fragment) {
        const wxColour colour = tracery_wx_color(flower_colours[fragment]);
        double best_distance = std::numeric_limits<double>::max();
        for (size_t index = 0; index < candidates.size(); ++index) {
            const double distance = color_delta_e00(colour, candidates[index].colour);
            if (distance < best_distance) {
                best_distance = distance;
                fragment_palette[fragment] = index;
            }
        }
    }
    std::vector<unsigned int> palette_filament_ids(candidates.size(), 0);
    std::vector<wxColour> palette_preview_colours;
    palette_preview_colours.reserve(candidates.size());
    for (const PaletteColour& colour : candidates)
        palette_preview_colours.emplace_back(colour.colour);
    std::vector<wxColour> target_colours = palette_preview_colours;
    const auto reindex = tracery_palette_reindex(bundle->mixed_filaments, old_physical_count,
                                                projected_manager, projected_colours.size());
    auto reindex_config = [&reindex](const DynamicPrintConfig& config) {
        DynamicPrintConfig updates;
        for (const char* key : {"wall_filament", "solid_infill_filament", "sparse_infill_filament",
                               "support_filament", "support_interface_filament"})
            if (const auto* value = config.option(key); value && value->getInt() > 0) {
                const size_t id = size_t(value->getInt());
                if (id >= reindex.size()) throw std::runtime_error("Invalid inherited material reference");
                if (int(reindex[id]) != int(id))
                    updates.set_key_value(key, new ConfigOptionInt(int(reindex[id])));
            }
        return updates;
    };
    plan.global_material_updates = reindex_config(bundle->prints.get_edited_preset().config);
    auto& plates = plater->get_partplate_list();
    for (int i = 0; i < plates.get_plate_count(); ++i)
        plan.plate_material_updates.emplace(i, reindex_config(*plates.get_plate(i)->config()));
    plan.conversion = reindex;
    std::map<unsigned int, size_t> plate_targets;
    for (unsigned int id : plate_material_ids) {
        if (id == black_id || (id <= old_physical_count && std::find(base_ids.begin(), base_ids.end(), id) != base_ids.end()))
            continue;
        wxColour target;
        if (id > 0 && id <= old_physical_count)
            target = wxColour(colour_option->values[id - 1]);
        else if (const auto* row = bundle->mixed_filaments.mixed_filament_from_id(id, old_physical_count)) {
            // Repeated strokes must not re-match already valid recipes and
            // gradually change ordinary parts or consume more virtual slots.
            if (tracery_recipe_uses_only(tracery_recipe_ingredients(*row, old_physical_count), base_ids))
                continue;
            target = wxColour(row->display_color);
        }
        if (!target.IsOk()) {
            wxMessageBox(wxString::Format(_L("Cannot resolve the colour of material F%u."), id),
                         _L("Stained glass"), wxOK | wxICON_WARNING);
            return std::nullopt;
        }
        plate_targets.emplace(id, target_colours.size());
        target_colours.push_back(target);
    }
    palette_filament_ids.resize(target_colours.size(), 0);
    palette_preview_colours = target_colours;

    constexpr size_t no_new_row = size_t(-1);
    std::vector<MixedFilamentBatchEntry> new_rows;
    std::vector<size_t> target_row_indices(target_colours.size(), no_new_row);
    struct VisualRecipeSlot {
        wxColour colour;
        unsigned int filament_id = 0;
        size_t new_row = no_new_row;
    };
    std::vector<VisualRecipeSlot> visual_slots;
    // Physical bases are valid colours too, and provide a bounded fallback even
    // when all virtual slots belong to incompatible recipes on other plates.
    for (unsigned int id : base_ids)
        visual_slots.push_back({wxColour(projected_colours[id - 1]), id, no_new_row});
    unsigned int virtual_id = unsigned(projected_colours.size());
    for (const MixedFilament& row : mixed_manager.mixed_filaments()) {
        if (row.deleted || !row.enabled)
            continue;
        ++virtual_id;
        if (!row.manual_pattern.empty() || row.gradient_enabled ||
            !tracery_recipe_uses_only(tracery_recipe_ingredients(row, projected_colours.size()), base_ids))
            continue;
        // Existing rows retain their displayed colour and recipe, including
        // rows used on another plate. Reuse never rewrites them.
        const wxColour display_colour(row.display_color);
        if (display_colour.IsOk())
            visual_slots.push_back({display_colour, virtual_id, no_new_row});
    }
    TraceryPerformanceScope recipe_performance("recipe_search_and_reuse");
    for (size_t index = 0; index < target_colours.size(); ++index) {
        MixedColorMatchRecipeResult recipe = build_tracery_recipe(base_colour_strings, target_colours[index], material_mode);
        if (!recipe.valid)
            continue;

        const auto physical_id = [&base_ids](unsigned int local_id) {
            return local_id >= 1 && local_id <= base_ids.size() ? base_ids[local_id - 1] : 0u;
        };
        recipe.component_a = physical_id(recipe.component_a);
        recipe.component_b = physical_id(recipe.component_b);
        if (!recipe.gradient_component_ids.empty()) {
            std::vector<unsigned int> ids = MixedFilamentManager::decode_gradient_component_ids(recipe.gradient_component_ids);
            for (unsigned int& id : ids)
                id = physical_id(id);
            recipe.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids(ids);
        }
        if (recipe.component_a == 0 || recipe.component_b == 0)
            continue;
        MixedFilamentBatchEntry entry;
        entry.component_a = recipe.component_a;
        entry.component_b = recipe.component_b;
        entry.mix_b_percent = recipe.mix_b_percent;
        entry.manual_pattern = recipe.manual_pattern;
        entry.gradient_component_ids = recipe.gradient_component_ids;
        entry.gradient_component_weights = recipe.gradient_component_weights;
        entry.distribution_mode = recipe.gradient_component_ids.empty() ? int(MixedFilament::Simple) : int(MixedFilament::LayerCycle);
        // Evaluate the same normalized recipe and display context used by batch
        // insertion before comparing colours. Nominal mix percentages are not
        // a reliable proxy for the final preview (notably with local Z layers).
        entry.display_color = mixed_manager.preview_custom_filament(entry, projected_colours);
        const wxColour final_colour(entry.display_color);
        if (!final_colour.IsOk())
            continue;
        std::vector<double> distances;
        distances.reserve(visual_slots.size());
        for (const auto& slot : visual_slots)
            distances.push_back(color_delta_e00(final_colour, slot.colour));
        const size_t selected = tracery_preview_slot(distances, new_rows.size() < available_mixed);
        if (selected < visual_slots.size()) {
            const auto& slot = visual_slots[selected];
            palette_filament_ids[index] = slot.filament_id;
            target_row_indices[index] = slot.new_row;
            palette_preview_colours[index] = slot.colour;
            if (distances[selected] > TRACERY_VISUAL_REUSE_DELTA_E)
                ++plan.approximate_colour_count;
            continue;
        }
        target_row_indices[index] = new_rows.size();
        palette_preview_colours[index] = final_colour;
        visual_slots.push_back({final_colour, 0, new_rows.size()});
        new_rows.emplace_back(std::move(entry));
    }

    recipe_performance.finish();
    bool has_unresolved_recipe = false;
    for (size_t index = 0; index < target_row_indices.size(); ++index) {
        if (palette_filament_ids[index] == 0 && target_row_indices[index] == no_new_row) {
            has_unresolved_recipe = true;
            break;
        }
    }
    if (new_rows.size() > available_mixed || has_unresolved_recipe) {
        wxMessageBox(_L("Unable to make recipes using the selected stained-glass materials."),
            _L("Stained glass"), wxOK | wxICON_WARNING);
        return std::nullopt;
    }
    std::vector<unsigned int> assigned_ids;
    mixed_manager.add_batch_custom_filaments(new_rows, projected_colours, &assigned_ids);
    for (auto& row : mixed_manager.mixed_filaments())
        for (const auto& old : bundle->mixed_filaments.mixed_filaments())
            if (row.stable_id == old.stable_id) { row = old; break; }
    for (size_t index = 0; index < target_colours.size(); ++index) {
        const size_t row = target_row_indices[index];
        if (palette_filament_ids[index] == 0 && row != no_new_row && row < assigned_ids.size())
            palette_filament_ids[index] = assigned_ids[row];
    }
    if (std::any_of(palette_filament_ids.begin(), palette_filament_ids.end(), [](unsigned int id) { return id == 0; })) {
        wxMessageBox(_L("Unable to assign a recipe to every stained-glass colour."),
            _L("Stained glass"), wxOK | wxICON_WARNING);
        return std::nullopt;
    }
    plan.mode = palette_only ? StainedGlassApplyPlan::Mode::ConvertPlate : StainedGlassApplyPlan::Mode::BakeFragments;
    plan.fragment_mixed_filament_ids.resize(fragment_palette.size(), 0);
    for (size_t fragment = 0; fragment < fragment_palette.size(); ++fragment) {
        plan.fragment_mixed_filament_ids[fragment] = palette_filament_ids[fragment_palette[fragment]];
        const wxColour preview = palette_preview_colours[fragment_palette[fragment]];
        plan.preview_colours.emplace_back(preview.Red() / 255.f, preview.Green() / 255.f, preview.Blue() / 255.f, 1.f);
    }

    for (const auto& [id, target] : plate_targets)
        plan.conversion.at(id) = EnforcerBlockerType(palette_filament_ids[target]);
    TraceryPerformanceScope conversion_performance("plate_conversion");
    auto conversion = prepare_tracery_plate_conversion(source_model, plate_instances, inherited, reindex,
                                                        plan.conversion, plate->get_index());
    conversion_performance.finish();
    auto allowed_materials = base_ids;
    if (black_id != 0) allowed_materials.push_back(black_id);
    for (const auto& [source, target] : conversion.target_objects)
        for (unsigned int id : tracery_object_materials(*conversion.model->objects[target], {})) {
            std::vector<unsigned int> ingredients;
            if (id > 0 && id <= projected_colours.size())
                ingredients.push_back(id);
            else if (const auto* row = mixed_manager.mixed_filament_from_id(id, projected_colours.size()))
                ingredients = tracery_recipe_ingredients(*row, projected_colours.size());
            if (!tracery_recipe_uses_only(ingredients, allowed_materials))
                throw std::runtime_error("A plate material could not be converted to the stained-glass palette");
        }
    const size_t selected_index = size_t(m_mask_object_index);
    plan.selected_object_index = conversion.target_objects.at(selected_index);
    plan.model = std::move(conversion.model);
    const auto* original = source_model.objects.at(selected_index);
    auto* converted = plan.model->objects.at(plan.selected_object_index);
    size_t model_part = 0;
    for (size_t vi = 0; vi < original->volumes.size(); ++vi) {
        auto* volume = original->volumes[vi];
        if (!volume->is_model_part()) continue;
        const bool has_regions = model_part < m_generated_voronoi_regions.size() && !m_generated_voronoi_regions[model_part].empty();
        if (!palette_only && (has_regions || (!m_incremental_generation && model_part < m_stained_glass_base_paint_data.size()))) {
            auto prepared = prepare_volume_tracery_partition(*volume, model_part, &plan.conversion,
                                                             &plan.fragment_mixed_filament_ids);
            prepared.volume = converted->volumes[vi];
            plan.partitions.emplace_back(std::move(prepared));
        }
        ++model_part;
    }
    plan.colours = projected_colours;
    for (size_t i = old_physical_count; i < projected_colours.size(); ++i)
        plan.new_physical_ids.push_back(unsigned(i + 1));
    plan.mixed_filament_definitions = mixed_manager.serialize_custom_entries();
    plan.manager = std::move(projected_manager);
    return plan;
}

GLGizmoTracery::GeneratedFragment GLGizmoTracery::generate_back_fact_tracery_date(
    const GeneratedFragment& front_fragment, const TriangleMesh& target_mesh, size_t volume_index) const
{
    GeneratedFragment back_fragment;
    back_fragment.fragment_id = front_fragment.fragment_id;
    size_t input_faces = front_fragment.surface.indices.size();
    size_t projected_faces = 0;
    size_t invalid_projections = 0;
    size_t cross_facet_splits = 0;
    size_t depth_limited = 0;
    size_t degenerate_input = 0;
    size_t degenerate_projected = 0;
    size_t invalid_vertices = 0;
    size_t ray_no_hit = 0;
    size_t ray_near_hit = 0;
    size_t ray_entry_facing_hit = 0;
    size_t source_normal_aligned_faces = 0;
    size_t source_normal_reversed_faces = 0;
    size_t source_normal_unavailable_faces = 0;
    double input_area = 0.0;
    double emitted_source_area = 0.0;
    double emitted_target_area = 0.0;
    double invalid_source_area = 0.0;
    double depth_limited_source_area = 0.0;
    double degenerate_source_area = 0.0;
    double degenerate_projected_source_area = 0.0;
    std::map<int, size_t> target_face_counts;
    std::map<int, size_t> rejected_face_counts;
    const indexed_triangle_set& target = target_mesh.its;
    if (target.vertices.empty() || target.indices.empty())
        return back_fragment;

    AABBMesh target_surface(target_mesh);
    Vec3f minimum = target.vertices.front();
    Vec3f maximum = minimum;
    for (const Vec3f& vertex : target.vertices) {
        minimum = minimum.cwiseMin(vertex);
        maximum = maximum.cwiseMax(vertex);
    }
    const float ray_epsilon = std::max(1e-5f, (maximum - minimum).norm() * 1e-6f);
    const auto triangle_area = [](const Vec3f& a, const Vec3f& b, const Vec3f& c) {
        return 0.5 * (b - a).cross(c - a).norm();
    };
#ifndef NDEBUG
    const int diagnostic_source_facet_id = front_fragment.source_facet_ids.empty() ? -1 : front_fragment.source_facet_ids.front();
    const auto record_ray_rejection = [this, volume_index, &front_fragment, diagnostic_source_facet_id](
                                          const char* reason, const Vec3f& point, const Vec3d& direction,
                                          const AABBMesh::hit_result& hit) {
        if (!m_display_diagnostics_active)
            return;
        const std::filesystem::path path = m_display_diagnostics_dir / "tracery_through_ray_rejections.tsv";
        const bool new_file = !std::filesystem::exists(path);
        std::ofstream output(path, std::ios::app | std::ios::binary);
        if (!output)
            return;
        if (new_file)
            output << "volume\tfragment\tsource_facet\treason\tpoint_x\tpoint_y\tpoint_z\tdirection_x\tdirection_y\tdirection_z\thit_facet\thit_distance\n";
        output << volume_index << '\t' << front_fragment.fragment_id << '\t' << diagnostic_source_facet_id << '\t'
               << reason << '\t' << std::setprecision(12) << point.x() << '\t' << point.y() << '\t' << point.z() << '\t'
               << direction.x() << '\t' << direction.y() << '\t' << direction.z() << '\t'
               << hit.face() << '\t' << hit.distance() << '\n';
    };
#else
    const auto record_ray_rejection = [](const char*, const Vec3f&, const Vec3d&, const AABBMesh::hit_result&) {};
#endif

    struct ProjectionResult {
        bool  valid = false;
        Vec3f position = Vec3f::Zero();
        Vec3f normal = Vec3f::Zero();
        int   face = -1;
    };
    const auto project = [&target_surface, ray_epsilon, &ray_no_hit, &ray_near_hit, &ray_entry_facing_hit,
                          &rejected_face_counts, &record_ray_rejection](const Vec3f& point, const Vec3f& normal) {
        ProjectionResult result;
        const Vec3d direction = -normal.cast<double>();
        const Vec3d origin = point.cast<double>() + direction * double(ray_epsilon);
        const AABBMesh::hit_result hit = target_surface.query_ray_hit(origin, direction);
        if (!hit.is_hit()) {
            ++ray_no_hit;
            record_ray_rejection("no_hit", point, direction, hit);
            return result;
        }
        if (hit.distance() <= double(ray_epsilon)) {
            ++ray_near_hit;
            ++rejected_face_counts[hit.face()];
            record_ray_rejection("near_hit", point, direction, hit);
            return result;
        }
        if (!hit.is_inside()) {
            ++ray_entry_facing_hit;
            ++rejected_face_counts[hit.face()];
            record_ray_rejection("entry_facing_hit", point, direction, hit);
            return result;
        }
        result.valid = true;
        result.position = hit.position().cast<float>();
        result.normal = hit.normal().cast<float>();
        result.face = hit.face();
        return result;
    };
    const auto append_projected = [&back_fragment, &projected_faces, &target_face_counts, &triangle_area,
                                   &emitted_source_area, &emitted_target_area, &degenerate_projected,
                                   &degenerate_projected_source_area](
                                      const ProjectionResult& a, const ProjectionResult& b, const ProjectionResult& c,
                                      const Vec3f& source_a, const Vec3f& source_b, const Vec3f& source_c) {
        const Vec3f projected_normal = (b.position - a.position).cross(c.position - a.position);
        if (projected_normal.squaredNorm() <= 1e-12f) {
            ++degenerate_projected;
            degenerate_projected_source_area += triangle_area(source_a, source_b, source_c);
            return;
        }
        const unsigned int first = unsigned(back_fragment.surface.vertices.size());
        back_fragment.surface.vertices.emplace_back(a.position);
        back_fragment.surface.vertices.emplace_back(b.position);
        back_fragment.surface.vertices.emplace_back(c.position);
        const Vec3f target_normal = a.normal + b.normal + c.normal;
        if (projected_normal.dot(target_normal) < 0.0f)
            back_fragment.surface.indices.emplace_back(stl_triangle_vertex_indices{ int(first), int(first + 2), int(first + 1) });
        else
            back_fragment.surface.indices.emplace_back(stl_triangle_vertex_indices{ int(first), int(first + 1), int(first + 2) });
        back_fragment.source_facet_ids.emplace_back(a.face);
        ++projected_faces;
        emitted_source_area += triangle_area(source_a, source_b, source_c);
        emitted_target_area += triangle_area(a.position, b.position, c.position);
        ++target_face_counts[a.face];
    };

    // A projected triangle may cross a target-mesh edge. Split it until an
    // emitted triangle belongs to exactly one target facet; never guess an ID
    // for a cross-facet triangle.
    std::function<void(const Vec3f&, const Vec3f&, const Vec3f&, int)> project_triangle;
    project_triangle = [&project_triangle, &project, &append_projected, &target, &triangle_area, &invalid_projections,
                        &invalid_vertices, &invalid_source_area, &cross_facet_splits, &depth_limited,
                        &depth_limited_source_area, &degenerate_input, &degenerate_source_area](
        const Vec3f& a, const Vec3f& b, const Vec3f& c, int depth) {
        Vec3f normal = (b - a).cross(c - a);
        const double source_area = triangle_area(a, b, c);
        if (normal.squaredNorm() <= 1e-12f) {
            ++degenerate_input;
            degenerate_source_area += source_area;
            return;
        }
        normal.normalize();
        const ProjectionResult projected_a = project(a, normal);
        const ProjectionResult projected_b = project(b, normal);
        const ProjectionResult projected_c = project(c, normal);
        if (!projected_a.valid || !projected_b.valid || !projected_c.valid) {
            ++invalid_projections;
            invalid_vertices += size_t(!projected_a.valid) + size_t(!projected_b.valid) + size_t(!projected_c.valid);
            invalid_source_area += source_area;
            return;
        }
        if (projected_a.face >= 0 && projected_a.face == projected_b.face && projected_a.face == projected_c.face &&
            size_t(projected_a.face) < target.indices.size()) {
            append_projected(projected_a, projected_b, projected_c, a, b, c);
            return;
        }
        // The refinement cap keeps target-edge handling bounded. Dropping the
        // unresolved remainder is conservative: it leaves existing paint
        // intact instead of assigning it to a wrong facet.
        if (depth >= 6) {
            ++depth_limited;
            depth_limited_source_area += source_area;
            return;
        }
        ++cross_facet_splits;
        const Vec3f ab = (a + b) * 0.5f;
        const Vec3f bc = (b + c) * 0.5f;
        const Vec3f ca = (c + a) * 0.5f;
        project_triangle(a, ab, ca, depth + 1);
        project_triangle(ab, b, bc, depth + 1);
        project_triangle(ca, bc, c, depth + 1);
        project_triangle(ab, bc, ca, depth + 1);
    };

    for (size_t face_index = 0; face_index < front_fragment.surface.indices.size(); ++face_index) {
        const stl_triangle_vertex_indices& face = front_fragment.surface.indices[face_index];
        if (face[0] < 0 || face[1] < 0 || face[2] < 0 ||
            size_t(face[0]) >= front_fragment.surface.vertices.size() ||
            size_t(face[1]) >= front_fragment.surface.vertices.size() ||
            size_t(face[2]) >= front_fragment.surface.vertices.size())
            continue;
        const Vec3f& a = front_fragment.surface.vertices[size_t(face[0])];
        const Vec3f& b = front_fragment.surface.vertices[size_t(face[1])];
        const Vec3f& c = front_fragment.surface.vertices[size_t(face[2])];
        const int source_facet_id = face_index < front_fragment.source_facet_ids.size() ?
            front_fragment.source_facet_ids[face_index] : -1;
        if (source_facet_id < 0 || size_t(source_facet_id) >= target.indices.size()) {
            ++source_normal_unavailable_faces;
        } else {
            const stl_triangle_vertex_indices& source_face = target.indices[size_t(source_facet_id)];
            if (source_face[0] < 0 || source_face[1] < 0 || source_face[2] < 0 ||
                size_t(source_face[0]) >= target.vertices.size() || size_t(source_face[1]) >= target.vertices.size() ||
                size_t(source_face[2]) >= target.vertices.size()) {
                ++source_normal_unavailable_faces;
            } else {
                const Vec3f source_normal = (target.vertices[size_t(source_face[1])] - target.vertices[size_t(source_face[0])]).cross(
                    target.vertices[size_t(source_face[2])] - target.vertices[size_t(source_face[0])]);
                const Vec3f generated_normal = (b - a).cross(c - a);
                if (source_normal.squaredNorm() <= 1e-12f || generated_normal.squaredNorm() <= 1e-12f)
                    ++source_normal_unavailable_faces;
                else if (source_normal.dot(generated_normal) > 0.0f)
                    ++source_normal_aligned_faces;
                else
                    ++source_normal_reversed_faces;
            }
        }
        input_area += triangle_area(a, b, c);
        project_triangle(a, b, c, 0);
    }
#ifndef NDEBUG
    if (m_display_diagnostics_active) {
        const int source_facet_id = diagnostic_source_facet_id;
        const std::filesystem::path path = m_display_diagnostics_dir / "tracery_back_projection.tsv";
        const bool new_file = !std::filesystem::exists(path);
        std::ofstream output(path, std::ios::app | std::ios::binary);
        if (output) {
            if (new_file)
                output << "volume\tfragment\tsource_facet\tinput_faces\toutput_faces\tinput_area\temitted_source_area\temitted_target_area\tinvalid_projection\tinvalid_vertices\tinvalid_source_area\tray_no_hit\tray_near_hit\tray_entry_facing_hit\tsource_normal_aligned_faces\tsource_normal_reversed_faces\tsource_normal_unavailable_faces\tcross_facet_splits\tdepth_limited\tdepth_limited_source_area\tdegenerate_input\tdegenerate_source_area\tdegenerate_projected\tdegenerate_projected_source_area\ttarget_facets\trejected_facets\n";
            std::ostringstream targets;
            for (const auto& [facet, count] : target_face_counts)
                targets << facet << ':' << count << ';';
            std::ostringstream rejected;
            for (const auto& [facet, count] : rejected_face_counts)
                rejected << facet << ':' << count << ';';
            output << volume_index << '\t' << front_fragment.fragment_id << '\t' << source_facet_id << '\t' << input_faces << '\t'
                   << projected_faces << '\t' << std::setprecision(12) << input_area << '\t'
                   << emitted_source_area << '\t' << emitted_target_area << '\t' << invalid_projections << '\t'
                   << invalid_vertices << '\t' << invalid_source_area << '\t' << ray_no_hit << '\t'
                   << ray_near_hit << '\t' << ray_entry_facing_hit << '\t' << source_normal_aligned_faces << '\t'
                   << source_normal_reversed_faces << '\t' << source_normal_unavailable_faces << '\t'
                   << cross_facet_splits << '\t'
                   << depth_limited << '\t' << depth_limited_source_area << '\t' << degenerate_input << '\t'
                   << degenerate_source_area << '\t' << degenerate_projected << '\t'
                   << degenerate_projected_source_area << '\t' << targets.str() << '\t' << rejected.str() << '\n';
        }

        if (source_facet_id >= 0 && size_t(source_facet_id) < target.indices.size()) {
            const stl_triangle_vertex_indices& source_face = target.indices[size_t(source_facet_id)];
            const auto face_is_valid = [&target](const stl_triangle_vertex_indices& face) {
                return face[0] >= 0 && face[1] >= 0 && face[2] >= 0 &&
                    size_t(face[0]) < target.vertices.size() && size_t(face[1]) < target.vertices.size() &&
                    size_t(face[2]) < target.vertices.size();
            };
            if (face_is_valid(source_face)) {
                const Vec3f source_a = target.vertices[size_t(source_face[0])];
                const Vec3f source_b = target.vertices[size_t(source_face[1])];
                const Vec3f source_c = target.vertices[size_t(source_face[2])];
                Vec3f source_normal = (source_b - source_a).cross(source_c - source_a);
                if (source_normal.squaredNorm() > 1e-12f) {
                    source_normal.normalize();
                    const std::filesystem::path geometry_path = m_display_diagnostics_dir / "tracery_through_facet_geometry.tsv";
                    const bool new_geometry_file = !std::filesystem::exists(geometry_path);
                    std::ofstream geometry(geometry_path, std::ios::app | std::ios::binary);
                    if (geometry) {
                        if (new_geometry_file)
                            geometry << "volume\tfragment\tsource_facet\ttarget_facet\tnormal_dot\tdistance_a\tdistance_b\tdistance_c\tsource_vertices\ttarget_vertices\n";
                        const auto write_triangle = [](std::ostringstream& stream, const Vec3f& a, const Vec3f& b, const Vec3f& c) {
                            stream << a.x() << ',' << a.y() << ',' << a.z() << ';'
                                   << b.x() << ',' << b.y() << ',' << b.z() << ';'
                                   << c.x() << ',' << c.y() << ',' << c.z();
                        };
                        for (const auto& [target_facet_id, unused_count] : target_face_counts) {
                            (void)unused_count;
                            if (target_facet_id < 0 || size_t(target_facet_id) >= target.indices.size())
                                continue;
                            const stl_triangle_vertex_indices& target_face = target.indices[size_t(target_facet_id)];
                            if (!face_is_valid(target_face))
                                continue;
                            const Vec3f target_a = target.vertices[size_t(target_face[0])];
                            const Vec3f target_b = target.vertices[size_t(target_face[1])];
                            const Vec3f target_c = target.vertices[size_t(target_face[2])];
                            Vec3f target_normal = (target_b - target_a).cross(target_c - target_a);
                            if (target_normal.squaredNorm() <= 1e-12f)
                                continue;
                            target_normal.normalize();
                            std::ostringstream source_vertices;
                            std::ostringstream target_vertices;
                            write_triangle(source_vertices, source_a, source_b, source_c);
                            write_triangle(target_vertices, target_a, target_b, target_c);
                            geometry << volume_index << '\t' << front_fragment.fragment_id << '\t' << source_facet_id << '\t'
                                     << target_facet_id << '\t' << std::setprecision(12) << source_normal.dot(target_normal) << '\t'
                                     << (source_a - target_a).dot(target_normal) << '\t'
                                     << (source_b - target_a).dot(target_normal) << '\t'
                                     << (source_c - target_a).dot(target_normal) << '\t'
                                     << source_vertices.str() << '\t' << target_vertices.str() << '\n';
                        }
                    }
                }
            }
        }
    }
#endif
    return back_fragment;
}

void GLGizmoTracery::generate_through_fragments()
{
    Model& model = wxGetApp().plater()->model();
    ModelObject* object = nullptr;
    for (ModelObject* candidate : model.objects)
        if (candidate->id() == m_mask_object_id) {
            object = candidate;
            break;
        }
    if (object == nullptr && m_mask_object_index >= 0 && size_t(m_mask_object_index) < model.objects.size())
        object = model.objects[size_t(m_mask_object_index)];
    if (object == nullptr || m_generated_fragments.empty())
        return;

    size_t volume_index = 0;
    for (ModelVolume* volume : object->volumes) {
        if (!volume->is_model_part())
            continue;
        if (volume_index >= m_generated_fragments.size())
            break;

        const TriangleMesh& volume_mesh = volume->mesh();
        const size_t front_fragment_count = m_generated_fragments[volume_index].size();
        for (size_t fragment_index = 0; fragment_index < front_fragment_count; ++fragment_index) {
            const GeneratedFragment& front = m_generated_fragments[volume_index][fragment_index];
            if (front.fragment_id >= m_fragment_colors.size() || front.surface.indices.empty())
                continue;

            // The helper will retain fragment_id and assign the target back
            // facet ID to every output triangle before it is appended.
            GeneratedFragment back = generate_back_fact_tracery_date(front, volume_mesh, volume_index);
            if (!back.surface.indices.empty())
                m_generated_fragments[volume_index].emplace_back(std::move(back));
        }
        ++volume_index;
    }

    // Legacy projection kept as an inactive reference while the helper above
    // supplies the target-facet-ID-aware implementation.
#if 0
    struct ProjectionKey {
        std::array<int64_t, 6> value;

        bool operator<(const ProjectionKey& other) const { return value < other.value; }
    };
    struct ProjectionResult {
        bool  valid = false;
        Vec3f position = Vec3f::Zero();
        Vec3f normal = Vec3f::Zero();
        int   face = -1;
    };
    const auto quantize = [](const Vec3f& position, const Vec3f& normal) {
        constexpr double position_scale = 100000.0;
        constexpr double normal_scale = 1000000.0;
        return ProjectionKey{{
            int64_t(std::llround(double(position.x()) * position_scale)),
            int64_t(std::llround(double(position.y()) * position_scale)),
            int64_t(std::llround(double(position.z()) * position_scale)),
            int64_t(std::llround(double(normal.x()) * normal_scale)),
            int64_t(std::llround(double(normal.y()) * normal_scale)),
            int64_t(std::llround(double(normal.z()) * normal_scale))
        }};
    };

    volume_index = 0;
    for (ModelVolume* volume : object->volumes) {
        if (!volume->is_model_part())
            continue;
        if (volume_index >= m_generated_fragments.size())
            break;

        // Work against only this volume's mesh. A through stroke must never
        // jump to a neighbouring or overlapping part of the same object.
        const TriangleMesh& volume_mesh = volume->mesh();
        const indexed_triangle_set& mesh = volume_mesh.its;
        if (mesh.vertices.empty() || mesh.indices.empty()) {
            ++volume_index;
            continue;
        }
        AABBMesh target_surface(volume_mesh);
        const std::vector<Vec3i32>& face_neighbours = target_surface.face_neighbor_index();

        Vec3f minimum = mesh.vertices.front();
        Vec3f maximum = minimum;
        for (const Vec3f& vertex : mesh.vertices) {
            minimum = minimum.cwiseMin(vertex);
            maximum = maximum.cwiseMax(vertex);
        }
        // Offset the ray into the solid so the source triangle cannot be
        // selected as its own destination. Scale the epsilon with the mesh,
        // while retaining a safe lower bound for very small models.
        const float ray_epsilon = std::max(1e-5f, (maximum - minimum).norm() * 1e-6f);

        const auto faces_are_compatible = [&face_neighbours](int first, int second) {
            if (first < 0 || second < 0)
                return false;
            if (first == second)
                return true;
            if (size_t(first) >= face_neighbours.size() || size_t(second) >= face_neighbours.size())
                return false;
            for (int edge = 0; edge < 3; ++edge) {
                if (face_neighbours[size_t(first)][edge] == second ||
                    face_neighbours[size_t(second)][edge] == first)
                    return true;
            }
            return false;
        };

        std::map<ProjectionKey, ProjectionResult> projection_cache;
        const auto project_vertex = [&target_surface, &projection_cache, &quantize, ray_epsilon](const Vec3f& point, const Vec3f& normal) {
            const ProjectionKey key = quantize(point, normal);
            auto [cached, inserted] = projection_cache.emplace(key, ProjectionResult{});
            if (!inserted)
                return cached->second;

            const Vec3d direction = -normal.cast<double>();
            const Vec3d origin = point.cast<double>() + direction * double(ray_epsilon);
            const AABBMesh::hit_result hit = target_surface.query_ray_hit(origin, direction);
            // From a point just inside a closed shell, the nearest valid hit
            // must be an exit intersection. Any ambiguous hit is rejected;
            // this conservative path prefers a small gap to misplaced paint.
            if (hit.is_hit() && hit.distance() > double(ray_epsilon) && hit.is_inside()) {
                cached->second.valid = true;
                cached->second.position = hit.position().cast<float>();
                cached->second.normal = hit.normal().cast<float>();
                cached->second.face = hit.face();
            }
            return cached->second;
        };

        const size_t front_fragment_count = m_generated_fragments[volume_index].size();
        for (size_t fragment_index = 0; fragment_index < front_fragment_count; ++fragment_index) {
            const GeneratedFragment& front = m_generated_fragments[volume_index][fragment_index];
            if (front.fragment_id >= m_fragment_colors.size() || front.surface.indices.empty())
                continue;

            indexed_triangle_set back_surface;
            for (const stl_triangle_vertex_indices& face : front.surface.indices) {
                if (face[0] < 0 || face[1] < 0 || face[2] < 0 ||
                    size_t(face[0]) >= front.surface.vertices.size() ||
                    size_t(face[1]) >= front.surface.vertices.size() ||
                    size_t(face[2]) >= front.surface.vertices.size())
                    continue;
                const Vec3f& a = front.surface.vertices[size_t(face[0])];
                const Vec3f& b = front.surface.vertices[size_t(face[1])];
                const Vec3f& c = front.surface.vertices[size_t(face[2])];
                Vec3f normal = (b - a).cross(c - a);
                if (normal.squaredNorm() <= 1e-12f)
                    continue;
                normal.normalize();

                const ProjectionResult projected_a = project_vertex(a, normal);
                const ProjectionResult projected_b = project_vertex(b, normal);
                const ProjectionResult projected_c = project_vertex(c, normal);
                if (!projected_a.valid || !projected_b.valid || !projected_c.valid ||
                    !faces_are_compatible(projected_a.face, projected_b.face) ||
                    !faces_are_compatible(projected_b.face, projected_c.face) ||
                    !faces_are_compatible(projected_c.face, projected_a.face))
                    continue;

                const Vec3f projected_normal = (projected_b.position - projected_a.position).cross(projected_c.position - projected_a.position);
                if (projected_normal.squaredNorm() <= 1e-12f)
                    continue;
                const unsigned int first = unsigned(back_surface.vertices.size());
                back_surface.vertices.emplace_back(projected_a.position);
                back_surface.vertices.emplace_back(projected_b.position);
                back_surface.vertices.emplace_back(projected_c.position);
                const Vec3f target_normal = projected_a.normal + projected_b.normal + projected_c.normal;
                if (projected_normal.dot(target_normal) < 0.0f)
                    back_surface.indices.emplace_back(stl_triangle_vertex_indices{ int(first), int(first + 2), int(first + 1) });
                else
                    back_surface.indices.emplace_back(stl_triangle_vertex_indices{ int(first), int(first + 1), int(first + 2) });
            }
            if (!back_surface.indices.empty())
                m_generated_fragments[volume_index].emplace_back(GeneratedFragment{ front.fragment_id, std::move(back_surface) });
        }
        ++volume_index;
    }
#endif
}

void GLGizmoTracery::generate_through_voronoi_regions()
{
    Model& model = wxGetApp().plater()->model();
    ModelObject* object = find_masked_model_object(model);
    if (object == nullptr)
        return;
    size_t volume_index = 0;
    for (ModelVolume* volume : object->volumes) {
        if (!volume->is_model_part())
            continue;
        if (volume_index >= m_generated_voronoi_regions.size())
            break;
        const TriangleMesh& volume_mesh = volume->mesh();
        const indexed_triangle_set& mesh = volume_mesh.its;
        AABBMesh target_surface(volume_mesh);
        Vec3f minimum = mesh.vertices.empty() ? Vec3f::Zero() : mesh.vertices.front();
        Vec3f maximum = minimum;
        for (const Vec3f& vertex : mesh.vertices) {
            minimum = minimum.cwiseMin(vertex);
            maximum = maximum.cwiseMax(vertex);
        }
        const float ray_epsilon = std::max(1e-5f, (maximum - minimum).norm() * 1e-6f);
        const float clip_epsilon = ray_epsilon * 8.0f;
        const std::vector<Vec3i32> face_neighbors = its_face_neighbors(mesh);
        const auto valid_face = [&mesh](int face_id) {
            if (face_id < 0 || size_t(face_id) >= mesh.indices.size())
                return false;
            const stl_triangle_vertex_indices& face = mesh.indices[size_t(face_id)];
            return face[0] >= 0 && face[1] >= 0 && face[2] >= 0 &&
                size_t(face[0]) < mesh.vertices.size() && size_t(face[1]) < mesh.vertices.size() &&
                size_t(face[2]) < mesh.vertices.size();
        };
        size_t analytic_multi_face_regions = 0;
        size_t analytic_target_faces = 0;
        size_t analytic_coverage_fallbacks = 0;
        const auto project_parallel_region = [&](const GeneratedVoronoiRegion& front,
                                                 std::vector<GeneratedVoronoiRegion>& output) {
            if (!valid_face(front.source_facet_id) || front.local_polygon.size() < 3)
                return false;
            const stl_triangle_vertex_indices& source_face = mesh.indices[size_t(front.source_facet_id)];
            const Vec3f source_a = mesh.vertices[size_t(source_face[0])];
            const Vec3f source_b = mesh.vertices[size_t(source_face[1])];
            const Vec3f source_c = mesh.vertices[size_t(source_face[2])];
            Vec3f source_normal = (source_b - source_a).cross(source_c - source_a);
            if (source_normal.squaredNorm() <= 1e-12f)
                return false;
            source_normal.normalize();
            Vec3f centroid = Vec3f::Zero();
            for (const Vec3f& vertex : front.local_polygon)
                centroid += vertex;
            centroid /= float(front.local_polygon.size());
            const Vec3d direction = -source_normal.cast<double>();
            const Vec3d origin = centroid.cast<double>() + direction * double(ray_epsilon);
            const AABBMesh::hit_result hit = target_surface.query_ray_hit(origin, direction);
            if (!hit.is_hit() || hit.distance() <= double(ray_epsilon) || !hit.is_inside() || !valid_face(hit.face()))
                return false;
            const stl_triangle_vertex_indices& target_face = mesh.indices[size_t(hit.face())];
            const Vec3f target_a = mesh.vertices[size_t(target_face[0])];
            const Vec3f target_b = mesh.vertices[size_t(target_face[1])];
            const Vec3f target_c = mesh.vertices[size_t(target_face[2])];
            Vec3f target_normal = (target_b - target_a).cross(target_c - target_a);
            if (target_normal.squaredNorm() <= 1e-12f)
                return false;
            target_normal.normalize();
            // The analytic path is intentionally limited to parallel, opposing
            // faces. Other geometry retains the adaptive raycast fallback.
            if (source_normal.dot(target_normal) > -0.999f)
                return false;
            const double denominator = direction.dot(target_normal.cast<double>());
            if (denominator <= 1e-8)
                return false;
            std::vector<Vec3f> polygon;
            polygon.reserve(front.local_polygon.size());
            for (const Vec3f& vertex : front.local_polygon) {
                const double distance = (target_a.cast<double>() - vertex.cast<double>()).dot(target_normal.cast<double>()) / denominator;
                if (distance <= double(ray_epsilon))
                    return false;
                polygon.emplace_back((vertex.cast<double>() + direction * distance).cast<float>());
            }
            const auto polygon_area = [&target_normal](const std::vector<Vec3f>& vertices) {
                if (vertices.size() < 3)
                    return 0.0;
                double area_twice = 0.0;
                for (size_t index = 0; index < vertices.size(); ++index)
                    area_twice += double(target_normal.dot(vertices[index].cross(vertices[(index + 1) % vertices.size()])));
                return std::abs(area_twice) * 0.5;
            };
            const auto clip_to_face = [&mesh, &valid_face, &target_normal, clip_epsilon](
                                          const std::vector<Vec3f>& input, int face_id) {
                std::vector<Vec3f> clipped = input;
                if (!valid_face(face_id))
                    return std::vector<Vec3f>{};
                const stl_triangle_vertex_indices& face = mesh.indices[size_t(face_id)];
                const std::array<Vec3f, 3> vertices = {
                    mesh.vertices[size_t(face[0])], mesh.vertices[size_t(face[1])], mesh.vertices[size_t(face[2])] };
                for (size_t edge = 0; edge < vertices.size() && !clipped.empty(); ++edge) {
                    const Vec3f& first = vertices[edge];
                    const Vec3f& second = vertices[(edge + 1) % vertices.size()];
                    std::vector<Vec3f> result;
                    result.reserve(clipped.size() + 1);
                    for (size_t index = 0; index < clipped.size(); ++index) {
                        const Vec3f& current = clipped[index];
                        const Vec3f& next = clipped[(index + 1) % clipped.size()];
                        const float current_side = target_normal.dot((second - first).cross(current - first));
                        const float next_side = target_normal.dot((second - first).cross(next - first));
                        const bool current_inside = current_side >= -clip_epsilon;
                        const bool next_inside = next_side >= -clip_epsilon;
                        if (current_inside)
                            result.emplace_back(current);
                        if (current_inside != next_inside) {
                            const float side_delta = current_side - next_side;
                            if (std::abs(side_delta) > 1e-12f)
                                result.emplace_back(current + (next - current) * (current_side / side_delta));
                        }
                    }
                    clipped = std::move(result);
                }
                return clipped;
            };

            // A back surface may be tessellated into several coplanar triangles.
            // Walk only the connected coplanar component containing the ray hit;
            // this prevents an unrelated, parallel surface from receiving paint.
            std::vector<int> target_faces;
            std::vector<bool> visited(mesh.indices.size(), false);
            std::queue<int> pending_faces;
            pending_faces.push(hit.face());
            visited[size_t(hit.face())] = true;
            while (!pending_faces.empty()) {
                const int face_id = pending_faces.front();
                pending_faces.pop();
                if (!valid_face(face_id))
                    continue;
                const stl_triangle_vertex_indices& face = mesh.indices[size_t(face_id)];
                const Vec3f face_a = mesh.vertices[size_t(face[0])];
                const Vec3f face_b = mesh.vertices[size_t(face[1])];
                const Vec3f face_c = mesh.vertices[size_t(face[2])];
                Vec3f face_normal = (face_b - face_a).cross(face_c - face_a);
                if (face_normal.squaredNorm() <= 1e-12f)
                    continue;
                face_normal.normalize();
                if (face_normal.dot(target_normal) < 0.999f ||
                    std::abs((face_a - target_a).dot(target_normal)) > clip_epsilon)
                    continue;
                target_faces.emplace_back(face_id);
                if (size_t(face_id) >= face_neighbors.size())
                    continue;
                for (int edge = 0; edge < 3; ++edge) {
                    const int neighbor = face_neighbors[size_t(face_id)][edge];
                    if (neighbor >= 0 && size_t(neighbor) < visited.size() && !visited[size_t(neighbor)]) {
                        visited[size_t(neighbor)] = true;
                        pending_faces.push(neighbor);
                    }
                }
            }
            if (target_faces.empty())
                return false;

            const double projected_area = polygon_area(polygon);
            if (projected_area <= 1e-10)
                return false;
            std::vector<GeneratedVoronoiRegion> candidate_regions;
            double covered_area = 0.0;
            for (const int face_id : target_faces) {
                const std::vector<Vec3f> clipped = clip_to_face(polygon, face_id);
                const double clipped_area = polygon_area(clipped);
                if (clipped.size() < 3 || clipped_area <= 1e-10)
                    continue;
                covered_area += clipped_area;
                for (size_t index = 1; index + 1 < clipped.size(); ++index)
                    candidate_regions.push_back({ front.fragment_id, face_id,
                        { clipped[0], clipped[index], clipped[index + 1] }, 0, true });
            }

            // Do not leave a partial analytic result behind. The tolerance only
            // accepts floating-point clipping noise; a real uncovered portion
            // falls back to the existing adaptive ray projection.
            const double coverage_tolerance = std::max(1e-7, projected_area * 1e-4);
            const bool complete_coverage = !candidate_regions.empty() &&
                std::abs(covered_area - projected_area) <= coverage_tolerance;
#ifndef NDEBUG
            if (m_display_diagnostics_active) {
                const std::filesystem::path path = m_display_diagnostics_dir / "tracery_through_analytic_projection.tsv";
                const bool new_file = !std::filesystem::exists(path);
                std::ofstream diagnostic(path, std::ios::app | std::ios::binary);
                if (diagnostic) {
                    if (new_file)
                        diagnostic << "volume\tfragment\tsource_facet\tseed_target_facet\ttarget_facets\tprojected_area\tcovered_area\ttolerance\tstatus\n";
                    std::ostringstream faces;
                    for (const int face_id : target_faces)
                        faces << face_id << ';';
                    diagnostic << volume_index << '\t' << front.fragment_id << '\t' << front.source_facet_id << '\t'
                               << hit.face() << '\t' << faces.str() << '\t' << std::setprecision(12)
                               << projected_area << '\t' << covered_area << '\t' << coverage_tolerance << '\t'
                               << (complete_coverage ? "accepted" : "fallback_incomplete_coverage") << '\n';
                }
            }
#endif
            if (!complete_coverage) {
                ++analytic_coverage_fallbacks;
                return false;
            }
            if (target_faces.size() > 1)
                ++analytic_multi_face_regions;
            analytic_target_faces += target_faces.size();
            output.insert(output.end(), std::make_move_iterator(candidate_regions.begin()),
                          std::make_move_iterator(candidate_regions.end()));
            return true;
        };
        const size_t front_region_count = m_generated_voronoi_regions[volume_index].size();
        size_t candidate_regions = 0;
        size_t skipped_small_polygon = 0;
        size_t skipped_missing_color = 0;
        size_t input_triangles = 0;
        size_t back_triangles = 0;
        size_t analytic_regions = 0;
        size_t fallback_regions = 0;
        std::vector<GeneratedVoronoiRegion> projected_regions;
        for (const GeneratedVoronoiRegion& front : m_generated_voronoi_regions[volume_index]) {
            if (front.local_polygon.size() < 3) {
                ++skipped_small_polygon;
                continue;
            }
            if (!m_active_stroke_erases && front.fragment_id >= m_fragment_colors.size()) {
                ++skipped_missing_color;
                continue;
            }
            ++candidate_regions;
            GeneratedFragment temporary;
            temporary.fragment_id = front.fragment_id;
            temporary.surface.vertices = front.local_polygon;
            for (size_t index = 1; index + 1 < front.local_polygon.size(); ++index) {
                temporary.surface.indices.emplace_back(stl_triangle_vertex_indices{ 0, int(index), int(index + 1) });
                temporary.source_facet_ids.emplace_back(front.source_facet_id);
            }
            input_triangles += temporary.surface.indices.size();
            const size_t analytic_begin = projected_regions.size();
            if (project_parallel_region(front, projected_regions)) {
                ++analytic_regions;
                back_triangles += projected_regions.size() - analytic_begin;
                continue;
            }
            ++fallback_regions;
            const GeneratedFragment back = generate_back_fact_tracery_date(temporary, volume_mesh, volume_index);
            back_triangles += back.surface.indices.size();
            for (size_t face_index = 0; face_index < back.surface.indices.size(); ++face_index) {
                const auto& face = back.surface.indices[face_index];
                if (face_index >= back.source_facet_ids.size() || face[0] < 0 || face[1] < 0 || face[2] < 0 ||
                    size_t(face[0]) >= back.surface.vertices.size() || size_t(face[1]) >= back.surface.vertices.size() ||
                    size_t(face[2]) >= back.surface.vertices.size())
                    continue;
                projected_regions.push_back({ front.fragment_id, back.source_facet_ids[face_index],
                    { back.surface.vertices[size_t(face[0])], back.surface.vertices[size_t(face[1])],
                      back.surface.vertices[size_t(face[2])] }, 0, true });
            }
        }
#ifndef NDEBUG
        if (m_display_diagnostics_active) {
            const std::filesystem::path path = m_display_diagnostics_dir / "tracery_through_voronoi.tsv";
            const bool new_file = !std::filesystem::exists(path);
            std::ofstream output(path, std::ios::app | std::ios::binary);
            if (output) {
                if (new_file)
                    output << "volume\tfront_regions\tcandidate_regions\tskipped_small_polygon\tskipped_missing_color\tinput_triangles\tanalytic_regions\tanalytic_multi_face_regions\tanalytic_target_faces\tanalytic_coverage_fallbacks\tfallback_regions\tback_triangles\tappended_regions\n";
                output << volume_index << '\t' << front_region_count << '\t' << candidate_regions << '\t'
                       << skipped_small_polygon << '\t' << skipped_missing_color << '\t' << input_triangles << '\t'
                       << analytic_regions << '\t' << analytic_multi_face_regions << '\t'
                       << analytic_target_faces << '\t' << analytic_coverage_fallbacks << '\t'
                       << fallback_regions << '\t'
                       << back_triangles << '\t' << projected_regions.size() << '\n';
            }
        }
#endif
        if (m_active_stroke_erases)
            for (auto& region : projected_regions) region.mixed_filament_id = 1;
        auto& output = m_generated_voronoi_regions[volume_index++];
        output.insert(output.end(), std::make_move_iterator(projected_regions.begin()),
                      std::make_move_iterator(projected_regions.end()));
    }
}

void GLGizmoTracery::commit_stained_glass_apply(StainedGlassApplyPlan&& plan)
{
    TraceryPerformanceScope performance("commit");
    TraceryDocument::ReadCacheScope document_cache;
    m_commit_refresh_active = true;
    m_commit_selector_key.reset();
    ScopeGuard reset_refresh_cache([this]() {
        m_commit_refresh_active = false;
        m_commit_selector_key.reset();
    });
    const bool plate_conversion = bool(plan.model);
    performance.count("plate_conversion", plate_conversion ? 1 : 0);
    performance.count("partitions", plan.partitions.size());
    performance.stage("commit.prepare_materials");
    if (plate_conversion) {
        auto* plater = wxGetApp().plater();
        auto* bundle = wxGetApp().preset_bundle;
        Model& model = plater->model();
        const auto* original = find_masked_model_object(model);
        auto* converted = plan.model->objects.at(plan.selected_object_index);
        size_t part = 0;
        std::map<ObjectID, TraceryRestoreState> restore_states;
        for (size_t vi = 0; vi < original->volumes.size(); ++vi) {
            const auto* volume = original->volumes[vi];
            if (!volume->is_model_part()) continue;
            if (part < m_stained_glass_base_paint_data.size())
                m_stained_glass_base_paint_data[part] = tracery_remap_paint(volume->mesh(),
                    m_stained_glass_base_paint_data[part], plan.conversion);
            if (auto found = m_mesh_restore_states.find(volume->id()); found != m_mesh_restore_states.end()) {
                auto state = found->second;
                state.original_annotations[0] = tracery_remap_paint(state.original_mesh, state.original_annotations[0], plan.conversion);
                state.last_annotations[0] = tracery_remap_paint(volume->mesh(), state.last_annotations[0], plan.conversion);
                restore_states.emplace(converted->volumes[vi]->id(), std::move(state));
            }
            ++part;
        }
        m_mesh_restore_states = std::move(restore_states);
        for (auto& selection : m_tracery_selections) {
            for (auto& id : selection.mixed_filament_ids) id = unsigned(plan.conversion.at(id));
            for (auto& regions : selection.regions)
                for (auto& region : regions) region.mixed_filament_id = unsigned(plan.conversion.at(region.mixed_filament_id));
        }
        for (auto& id : m_fragment_mixed_filament_ids) id = unsigned(plan.conversion.at(id));
        for (auto& regions : m_generated_voronoi_regions)
            for (auto& region : regions) region.mixed_filament_id = unsigned(plan.conversion.at(region.mixed_filament_id));
        if (plan.mode == StainedGlassApplyPlan::Mode::BakeFragments)
            m_fragment_colors = plan.preview_colours;
        // Keep the selected instance identity while moving it to its independent
        // object. Canvas reload follows that identity rather than an old index.
        const int active_instance = m_c->selection_info()->get_active_instance();
        m_parent.get_selection().add_instance(unsigned(m_mask_object_index), unsigned(std::max(0, active_instance)), true);
        const size_t old_count = bundle->filament_presets.size();
        if (plan.colours.size() != old_count)
            bundle->set_num_filaments(unsigned(plan.colours.size()),
                std::vector<std::string>(plan.colours.begin() + old_count, plan.colours.end()));
        bundle->mixed_filaments = std::move(plan.manager);
        bundle->prints.get_edited_preset().config.apply(plan.global_material_updates);
        for (const auto& [index, updates] : plan.plate_material_updates)
            plater->get_partplate_list().get_plate(index)->config()->apply(updates);
        bundle->project_config.set_key_value("mixed_filament_definitions", new ConfigOptionString(plan.mixed_filament_definitions));
        bundle->prints.get_edited_preset().config.set_key_value("mixed_filament_definitions", new ConfigOptionString(plan.mixed_filament_definitions));
        bundle->consume_last_filament_id_remap(); // the prepared model is already reindexed
        m_mask_object_index = int(plan.selected_object_index);
        m_mask_object_id = converted->id();
        // Model move-assignment would replace the live backup path and lose
        // project metadata absent from the planning copy. Only these changed.
        performance.stage("commit.swap_model");
        m_c->update(CommonGizmosDataID::None);
        model.swap_objects(*plan.model);
        model.plates_custom_gcodes.swap(plan.model->plates_custom_gcodes);
        performance.stage("commit.publish_partitions");
        for (auto& partition : plan.partitions)
            commit_tracery_mesh_update(std::move(partition));
        performance.stage("commit.reload_plate_objects");
        plater->get_partplate_list().reload_all_objects(false);
        performance.stage("commit.update_filament_config");
        plater->update_filament_colors_in_full_config();
        // Sidebar refresh can render synchronously; restore valid common data
        // and selection pointers before allowing any such callbacks.
        performance.stage("commit.reload_scene");
        m_parent.reload_scene(false, true);
        performance.stage("commit.refresh_selection_first");
        m_c->update(on_get_requirements());
        update_from_model_object(false);
        // Refresh only the UI: on_filaments_change would reindex model paint twice.
        performance.stage("commit.refresh_sidebar");
        // Final UI refresh below builds the colour panel once, after presets and
        // model pointers have settled. Intermediate renders only duplicate work.
        plater->sidebar().on_filaments_change(plan.colours.size(), false, true);
        plater->sidebar().set_stained_glass_base_filament_preset(plan.new_physical_ids, true);
        performance.stage("commit.reload_object_list");
        wxGetApp().obj_list()->reload_all_plates(false);
        performance.stage("commit.refresh_selection_second");
        m_c->update(on_get_requirements());
        update_from_model_object(false);
        invalidate_painting_raycast();
        plater->update_project_dirty_from_presets();
        if (plan.approximate_colour_count > 0)
            plater->get_notification_manager()->push_notification(GUI::format(_L(
                "Stained glass: material slots are full. Used the closest available colours for %1% palette entries."),
                plan.approximate_colour_count));
    }

#ifndef NDEBUG
    if (m_display_diagnostics_active) {
        size_t fragment_total = 0;
        size_t face_total = 0;
        size_t source_id_total = 0;
        for (const auto& per_volume : m_generated_fragments)
            for (const GeneratedFragment& fragment : per_volume) {
                ++fragment_total;
                face_total += fragment.surface.indices.size();
                source_id_total += fragment.source_facet_ids.size();
            }
        record_display_diagnostic_state("commit_apply_begin",
            "mode=" + std::string(plan.mode == StainedGlassApplyPlan::Mode::BakeFragments ? "bake" : "restore") +
            ",generated_volumes=" + std::to_string(m_generated_fragments.size()) +
            ",generated_fragments=" + std::to_string(fragment_total) +
            ",generated_faces=" + std::to_string(face_total) +
            ",source_ids=" + std::to_string(source_id_total) +
            ",plan_mixed_ids=" + std::to_string(plan.fragment_mixed_filament_ids.size()));
    }
#endif
    if (!plate_conversion && !plan.partitions.empty()) {
        performance.stage("commit.publish_partitions");
        for (auto& partition : plan.partitions)
            commit_tracery_mesh_update(std::move(partition));
        performance.stage("commit.refresh_selection");
        update_from_model_object(false);
        invalidate_painting_raycast();
        m_seed_fill_last_mesh_id = -1;
    }
    performance.stage("commit.assign_fragment_materials");
    if (plan.mode == StainedGlassApplyPlan::Mode::BakeFragments) {
        m_fragment_mixed_filament_ids = std::move(plan.fragment_mixed_filament_ids);
        for (size_t index = 0; index < m_transition_recipe_targets.size(); ++index) {
            const size_t transient_fragment = m_pending_selection_fragment_count + index;
            if (transient_fragment >= m_fragment_mixed_filament_ids.size())
                continue;
            const auto [selection_index, fragment] = m_transition_recipe_targets[index];
            if (selection_index >= m_tracery_selections.size())
                continue;
            TracerySelection& selection = m_tracery_selections[selection_index];
            if (fragment >= selection.mixed_filament_ids.size())
                continue;
            if (fragment < selection.fragment_colors.size())
                selection.fragment_colors[fragment] = m_fragment_colors[transient_fragment];
            selection.mixed_filament_ids[fragment] = m_fragment_mixed_filament_ids[transient_fragment];
            for (auto& per_volume : selection.regions)
                for (GeneratedVoronoiRegion& region : per_volume)
                    if (region.fragment_id == fragment)
                        region.mixed_filament_id = selection.mixed_filament_ids[fragment];
        }
        for (auto& per_volume : m_generated_voronoi_regions)
            for (GeneratedVoronoiRegion& region : per_volume)
                if (region.fragment_id < m_fragment_mixed_filament_ids.size())
                    region.mixed_filament_id = m_fragment_mixed_filament_ids[region.fragment_id];
        if (PresetBundle* bundle = wxGetApp().preset_bundle) {
            if (auto* definitions = bundle->project_config.option<ConfigOptionString>("mixed_filament_definitions"))
                definitions->value = std::move(plan.mixed_filament_definitions);
        }
    }

#ifndef NDEBUG
    if (m_display_diagnostics_active)
        record_display_diagnostic_state("commit_apply_before_bake",
            "mixed_ids=" + std::to_string(m_fragment_mixed_filament_ids.size()));
#endif
    performance.stage("commit.bake_or_notify");
    if (plan.mode != StainedGlassApplyPlan::Mode::ConvertPlate)
        bake_fragments_to_mmu_painting(true);
    else if (const auto* object = find_masked_model_object(wxGetApp().plater()->model()))
        notify_mmu_paint_changed(*object);
#ifndef NDEBUG
    if (m_display_diagnostics_active)
        record_display_diagnostic_state("commit_apply_after_bake",
            "remaining_generated_volumes=" + std::to_string(m_generated_fragments.size()) +
            ",mixed_ids=" + std::to_string(m_fragment_mixed_filament_ids.size()));
#endif
    performance.stage("commit.final_ui_refresh");
    wxGetApp().sidebar().update_color_mix_panel();
    wxGetApp().plater()->post_slice_state_change_update();
    m_parent.set_as_dirty();
    // Draw the settled result once while document snapshots are still retained.
    // Keep the first displayed frame in the measured commit rather than moving
    // its parsing/upload cost into an unmeasured deferred callback.
    performance.stage("commit.final_scene_render");
    if (m_parent.is_initialized())
        m_parent.render();
    performance.stage("commit.cleanup");
}

ModelObject* GLGizmoTracery::find_masked_model_object(Model& model) const
{
    for (ModelObject* candidate : model.objects)
        if (candidate->id() == m_mask_object_id)
            return candidate;
    if (m_mask_object_index >= 0 && size_t(m_mask_object_index) < model.objects.size())
        return model.objects[size_t(m_mask_object_index)];
    return nullptr;
}

std::vector<TriangleSelector::TriangleSplittingData> GLGizmoTracery::collect_partition_annotations(const ModelVolume& volume,
                                                                                                   size_t             index) const
{
    const auto empty = TriangleSelector::TriangleSplittingData{};
    const auto base  = index < m_stained_glass_base_paint_data.size() ? m_stained_glass_base_paint_data[index] :
                                                                        volume.mmu_segmentation_facets.get_data();
    return {m_incremental_generation ? volume.mmu_segmentation_facets.get_data() : base,
            volume.supported_facets.get_data(),
            volume.seam_facets.get_data(),
            volume.fuzzy_skin_facets.get_data(),
            volume.exterior_facets.get_data(),
            base,
            index < m_mask_data.size() ? m_mask_data[index] : empty,
            index < m_previous_mask_data.size() ? m_previous_mask_data[index] : empty};
}

std::vector<GLGizmoTracery::GeneratedVoronoiRegion> GLGizmoTracery::remap_partition_regions(
    const std::vector<GeneratedVoronoiRegion>& regions, TraceryRegionMapper& mapper) const
{
    TraceryPerformanceScope performance("region_mapping");
    performance.count("input_regions", regions.size());
    performance.stage("region_mapping.clip_regions");
    TraceryMappingStatistics statistics;
    std::vector<GeneratedVoronoiRegion> result;
    for (const GeneratedVoronoiRegion& region : regions) {
        for (auto& mapped : mapper.map(region.source_facet_id, region.local_polygon, statistics)) {
            GeneratedVoronoiRegion child = region;
            child.source_facet_id = mapped.facet;
            child.local_polygon = std::move(mapped.polygon);
            result.push_back(std::move(child));
        }
    }
    performance.count("candidate_pairs", statistics.candidate_pairs);
    performance.count("spatial_candidates", statistics.spatial_candidates);
    performance.count("clip_calls", statistics.clip_calls);
    performance.count("separated_candidates", statistics.separated_candidates);
    performance.count("inherited_regions", statistics.inherited_regions);
    performance.count("inherited_facets", statistics.inherited_facets);
    performance.count("nonempty_results", statistics.nonempty_results);
    performance.count("output_regions", result.size());
    performance.stage("region_mapping.cleanup");
    return result;
}

std::vector<TriangleSelector::TriangleSplittingData> GLGizmoTracery::collect_volume_annotations(const ModelVolume& volume)
{
    return {volume.mmu_segmentation_facets.get_data(), volume.supported_facets.get_data(), volume.seam_facets.get_data(),
            volume.fuzzy_skin_facets.get_data(), volume.exterior_facets.get_data()};
}

void GLGizmoTracery::replace_tracery_mesh(ModelVolume& volume, TraceryPartitionResult& partition)
{
    volume.set_mesh(std::move(partition.mesh));
    FacetsAnnotation* annotations[] = {&volume.mmu_segmentation_facets, &volume.supported_facets, &volume.seam_facets,
                                       &volume.fuzzy_skin_facets, &volume.exterior_facets};
    for (size_t channel = 0; channel < 5; ++channel) {
        TriangleSelector selector(volume.mesh());
        selector.deserialize(partition.annotations[channel], false);
        annotations[channel]->set(selector);
    }
    volume.calculate_convex_hull();
    volume.invalidate_convex_hull_2d();
    volume.set_new_unique_id();
    volume.get_object()->invalidate_bounding_box();
}

GLGizmoTracery::PreparedTraceryVolume GLGizmoTracery::prepare_volume_tracery_partition(ModelVolume&                     volume,
                                                                                       size_t                           volume_index,
                                                                                       const EnforcerBlockerStateMap*   mapping,
                                                                                       const std::vector<unsigned int>* recipes) const
{
    TraceryPerformanceScope performance("partition_and_remap");
    performance.count("volume_index", volume_index);
    performance.count("input_facets", volume.mesh().its.indices.size());
    performance.count("history_selections", m_tracery_selections.size());
    performance.stage("partition.collect_current_annotations");
    PreparedTraceryVolume prepared;
    prepared.volume = &volume;
    prepared.index  = volume_index;
    auto current    = collect_volume_annotations(volume);
    performance.stage("partition.collect_partition_annotations");
    auto annotations            = collect_partition_annotations(volume, volume_index);
    annotations[0]              = current[0];
    auto initial                = annotations;
    initial[6]                  = initial[7]; // capture the committed mask, before this stroke
    auto       document         = volume.tracery_edit_data.empty() ? TraceryDocument::capture(volume.mesh(), initial) :
                                                                     TraceryDocument::decode(volume.tracery_edit_data);
    const auto previous_sources = document.source_facets();
    performance.stage("partition.build_regions");
    auto regions = build_voronoi_2d_data(volume_index, {}, recipes != nullptr);
    performance.count("region_source_facets", regions.size());
    performance.stage("partition.remap_material_ids");
    if (mapping) {
        {
            TraceryPerformanceScope::Sample sample(performance, "material_mapping.partition_paint");
            annotations[0] = tracery_remap_paint(volume.mesh(), annotations[0], *mapping);
        }
        {
            TraceryPerformanceScope::Sample sample(performance, "material_mapping.base_paint");
            annotations[5] = tracery_remap_paint(volume.mesh(), annotations[5], *mapping);
        }
        document.remap_materials(*mapping);
    }
    if (recipes) {
        for (auto& facet : regions)
            for (auto& region : facet.regions)
                region.color = EnforcerBlockerType(recipes->at(region.fragment_id));
    }
    // Source eligibility was enforced before generation. Target paint, including
    // opaque black, must not overwrite either generated or through-projected regions.
    performance.stage("partition.geometry");
    const TraceryEdit edit = m_active_stroke_erases ?
                                 TraceryEdit::Erase :
                                 (!m_incremental_generation && !regions.empty() ? TraceryEdit::Regenerate : TraceryEdit::Paint);
    const bool vector_surface = document.base_mesh().its.vertices == volume.mesh().its.vertices &&
                                document.base_mesh().its.indices == volume.mesh().its.indices;
    performance.count("vector_surface", vector_surface ? 1 : 0);
    if (!vector_surface)
        throw std::runtime_error("Legacy stained-glass mesh editing is disabled. Open an unmodified model to use vector stained glass.");
    prepared.partition = document.apply_vector_edit(volume.mesh(), annotations, regions, edit);
    performance.count("base_facets", document.base_mesh().its.indices.size());
    performance.count("output_facets", prepared.partition.mesh.indices.size());
    performance.stage("partition.save_editing_data");
    if (document.is_vector() && volume_index < m_fill_reference_selectors.size())
        document.set_selection_reference(m_fill_reference_selectors[volume_index]->serialize());
    prepared.document = document.encode();
    performance.stage("partition.build_mapping_index");
    TraceryRegionMapper mapper(document.base_mesh().its, prepared.partition);
    auto                remap_view = [&](const std::vector<GeneratedVoronoiRegion>& view) {
        auto base_view = view;
        for (auto& region : base_view)
            region.source_facet_id = previous_sources.at(region.source_facet_id);
        return remap_partition_regions(base_view, mapper);
    };
    performance.stage("partition.remap_generated_regions");
    if (volume_index < m_generated_voronoi_regions.size())
        prepared.generated = remap_view(m_generated_voronoi_regions[volume_index]);
    performance.stage("partition.remap_history_regions");
    for (const TracerySelection& selection : m_tracery_selections)
        prepared.selections.push_back(volume_index < selection.regions.size() ? remap_view(selection.regions[volume_index]) :
                                                                                std::vector<GeneratedVoronoiRegion>{});
    if (performance.enabled())
        performance.count("mapping_index_estimated_bytes", mapper.estimated_bytes());
    performance.stage("partition.assign_region_materials");
    if (recipes)
        for (auto& region : prepared.generated)
            region.mixed_filament_id = recipes->at(region.fragment_id);
    if (mapping)
        for (auto& selection : prepared.selections)
            for (auto& region : selection)
                region.mixed_filament_id = unsigned(mapping->at(region.mixed_filament_id));
    performance.stage("partition.cleanup");
    return prepared;
}

void GLGizmoTracery::commit_tracery_mesh_update(PreparedTraceryVolume&& prepared)
{
    ModelVolume&   volume = *prepared.volume;
    const size_t   index  = prepared.index;
    const ObjectID old_id = volume.id();
    if (!TraceryDocument::decode_cached(prepared.document)->is_vector())
        replace_tracery_mesh(volume, prepared.partition);
    m_mesh_restore_states.erase(old_id);
    volume.tracery_edit_data = std::move(prepared.document);
    if (m_edit_document_hashes.size() <= index)
        m_edit_document_hashes.resize(index + 1, 0);
    m_edit_document_hashes[index] = std::hash<std::string>{}(volume.tracery_edit_data);
    auto store                    = [index](auto& destination, auto&& value) {
        if (destination.size() <= index)
            destination.resize(index + 1);
        destination[index] = std::move(value);
    };
    store(m_stained_glass_base_paint_data, std::move(prepared.partition.annotations[5]));
    store(m_mask_data, std::move(prepared.partition.annotations[6]));
    store(m_previous_mask_data, std::move(prepared.partition.annotations[7]));
    store(m_generated_voronoi_regions, std::move(prepared.generated));
    for (size_t i = 0; i < m_tracery_selections.size(); ++i)
        store(m_tracery_selections[i].regions, std::move(prepared.selections[i]));
}

bool GLGizmoTracery::bake_exact_tracery_partitions(ModelObject& object)
{
    // Prepare ALL volumes and editor geometry before replacing any source mesh.
    std::vector<PreparedTraceryVolume> updates;
    try {
        size_t index = 0;
        for (ModelVolume* volume : object.volumes) {
            if (!volume->is_model_part())
                continue;
            const bool has_regions = index < m_generated_voronoi_regions.size() && !m_generated_voronoi_regions[index].empty();
            if (has_regions || (!m_incremental_generation && index < m_stained_glass_base_paint_data.size()))
                updates.push_back(prepare_volume_tracery_partition(*volume, index));
            ++index;
        }
    } catch (const std::exception& error) {
        wxMessageBox(from_u8(error.what()), _L("Stained glass"), wxOK | wxICON_ERROR);
        return false;
    }
    for (auto& update : updates)
        commit_tracery_mesh_update(std::move(update));
    if (!updates.empty()) {
        update_from_model_object(false);
        invalidate_painting_raycast();
        m_seed_fill_last_mesh_id = -1;
    }
    return true;
}

void GLGizmoTracery::notify_mmu_paint_changed(const ModelObject& object) const
{
    TraceryPerformanceScope performance("notify_vector_paint");
    const ModelObjectPtrs& objects = wxGetApp().model().objects;
    const size_t object_index = std::find(objects.begin(), objects.end(), &object) - objects.begin();
    performance.stage("notify.object_info");
    wxGetApp().obj_list()->update_info_items(object_index);
    performance.stage("notify.instance_usage");
    wxGetApp().plater()->get_partplate_list().notify_instance_update(object_index, 0);
    performance.stage("notify.schedule_slice");
    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    performance.stage("notify.filament_usage");
    wxGetApp().plater()->notify_filament_usage_changed();
}

void GLGizmoTracery::bake_fragments_to_mmu_painting(bool prepared)
{
    TraceryPerformanceScope performance("commit_effect");
    performance.stage("effect.prepare");
    Model& direct_model = wxGetApp().plater()->model();
    ModelObject* direct_object = find_masked_model_object(direct_model);
    if (direct_object == nullptr)
        return;
    if (!prepared && !bake_exact_tracery_partitions(*direct_object))
        return;
    performance.stage("effect.notify");
    notify_mmu_paint_changed(*direct_object);
    performance.stage("effect.save_selection_history");
    bool has_generated_regions = false;
    for (const auto& regions : m_generated_voronoi_regions)
        has_generated_regions = has_generated_regions || !regions.empty();
    if (has_generated_regions && m_pending_selection_fragment_count > 0) {
        TracerySelection selection;
        selection.mode = m_pending_selection_mode;
        selection.regions.resize(m_generated_voronoi_regions.size());
        for (size_t volume = 0; volume < m_generated_voronoi_regions.size(); ++volume)
            for (const GeneratedVoronoiRegion& region : m_generated_voronoi_regions[volume])
                if (region.fragment_id < m_pending_selection_fragment_count)
                    selection.regions[volume].emplace_back(region);
        selection.fragment_colors.assign(m_fragment_colors.begin(),
            m_fragment_colors.begin() + std::min(m_pending_selection_fragment_count, m_fragment_colors.size()));
        selection.mixed_filament_ids.assign(m_fragment_mixed_filament_ids.begin(),
            m_fragment_mixed_filament_ids.begin() + std::min(m_pending_selection_fragment_count, m_fragment_mixed_filament_ids.size()));
        m_tracery_selections.emplace_back(std::move(selection));
    }
    performance.stage("effect.clear_temporary_regions");
    m_generated_voronoi_regions.clear();
    m_transition_recipe_targets.clear();
    m_generated_fragments.clear();
    performance.stage("effect.refresh_scene");
    bool vector_only = true;
    for (const auto* volume : direct_object->volumes)
        if (volume->is_model_part() && !volume->tracery_edit_data.empty() &&
            !TraceryDocument::decode_cached(volume->tracery_edit_data)->is_vector())
            vector_only = false;
    performance.count("geometry_refresh_skipped", vector_only ? 1 : 0);
    if (vector_only) {
        // Vector edits change materials, not geometry. The notification above
        // schedules slicing and updates usage; redraw reads the new document.
        m_parent.set_as_dirty();
        return;
    }
    const auto& objects = wxGetApp().model().objects;
    const auto found = std::find(objects.begin(), objects.end(), direct_object);
    if (found != objects.end())
        wxGetApp().plater()->changed_mesh(int(found - objects.begin()));
}

TriangleSelector::Tracery2DData GLGizmoTracery::build_voronoi_2d_data(
    size_t volume_index, const std::function<void(const std::string&)>& diagnostic, bool material_budget) const
{
    TriangleSelector::Tracery2DData data;
    if (volume_index >= m_generated_voronoi_regions.size()) {
        if (diagnostic)
            diagnostic("projection_skipped\tvolume=" + std::to_string(volume_index) + "\treason=no_generated_volume");
        return data;
    }
    Model& model = wxGetApp().plater()->model();
    const ModelObject* object = find_masked_model_object(model);
    if (object == nullptr) {
        if (diagnostic)
            diagnostic("projection_skipped\tvolume=" + std::to_string(volume_index) + "\treason=masked_object_not_found");
        return data;
    }
    const ModelVolume* volume = nullptr;
    size_t current_index = 0;
    for (const ModelVolume* candidate : object->volumes)
        if (candidate->is_model_part() && current_index++ == volume_index) {
            volume = candidate;
            break;
        }
    if (volume == nullptr) {
        if (diagnostic)
            diagnostic("projection_skipped\tvolume=" + std::to_string(volume_index) + "\treason=model_volume_not_found");
        return data;
    }
    const indexed_triangle_set& mesh = volume->mesh().its;
    struct ProjectionStatistics {
        size_t input = 0;
        size_t output = 0;
        size_t skipped_fragment_id = 0;
        size_t skipped_polygon = 0;
        size_t skipped_filament = 0;
        size_t skipped_plane = 0;
        size_t non_finite = 0;
        size_t degenerate = 0;
        double output_area_2d = 0.0;
    };
    std::map<int, ProjectionStatistics> statistics_by_facet;
    const auto polygon_area_3d = [](const std::vector<Vec3f>& polygon) {
        if (polygon.size() < 3)
            return 0.0;
        const Vec3d origin = polygon.front().cast<double>();
        double area = 0.0;
        for (size_t index = 1; index + 1 < polygon.size(); ++index)
            area += 0.5 * (polygon[index].cast<double>() - origin).cross(
                polygon[index + 1].cast<double>() - origin).norm();
        return area;
    };
    const auto polygon_area_2d = [](const std::vector<Vec2d>& polygon) {
        if (polygon.size() < 3)
            return 0.0;
        double twice_area = 0.0;
        for (size_t index = 0; index < polygon.size(); ++index)
            twice_area += polygon[index].x() * polygon[(index + 1) % polygon.size()].y() -
                          polygon[index].y() * polygon[(index + 1) % polygon.size()].x();
        return std::abs(twice_area) * 0.5;
    };
    std::map<int, size_t> facet_entries;
    for (const GeneratedVoronoiRegion& generated : m_generated_voronoi_regions[volume_index]) {
        ProjectionStatistics& statistics = statistics_by_facet[generated.source_facet_id];
        ++statistics.input;
        const double area_3d = polygon_area_3d(generated.local_polygon);
        if (generated.mixed_filament_id == 0 && generated.fragment_id >=
            (material_budget ? m_fragment_colors.size() : m_fragment_mixed_filament_ids.size())) {
            ++statistics.skipped_fragment_id;
            if (diagnostic)
                diagnostic("projection_skipped\tvolume=" + std::to_string(volume_index) +
                           "\tfragment=" + std::to_string(generated.fragment_id) +
                           "\tfacet=" + std::to_string(generated.source_facet_id) +
                           "\treason=fragment_id_out_of_range");
            continue;
        }
        if (generated.local_polygon.size() < 3) {
            ++statistics.skipped_polygon;
            if (diagnostic)
                diagnostic("projection_skipped\tvolume=" + std::to_string(volume_index) +
                           "\tfragment=" + std::to_string(generated.fragment_id) +
                           "\tfacet=" + std::to_string(generated.source_facet_id) +
                           "\treason=polygon_has_fewer_than_3_vertices");
            continue;
        }
        // Before recipes are assigned, only the geometry is needed. A temporary
        // valid ID passes the common projection checks; the budget helper clears
        // it on its private partition and never publishes it to the model.
        const unsigned int filament = material_budget ? 1u : (generated.mixed_filament_id != 0 ? generated.mixed_filament_id :
            m_fragment_mixed_filament_ids[generated.fragment_id]);
        if (filament == 0 || filament > unsigned(EnforcerBlockerType::ExtruderMax)) {
            ++statistics.skipped_filament;
            if (diagnostic)
                diagnostic("projection_skipped\tvolume=" + std::to_string(volume_index) +
                           "\tfragment=" + std::to_string(generated.fragment_id) +
                           "\tfacet=" + std::to_string(generated.source_facet_id) +
                           "\tfilament=" + std::to_string(filament) + "\treason=invalid_filament");
            continue;
        }
        auto [entry, inserted] = facet_entries.emplace(generated.source_facet_id, data.size());
        if (inserted) {
            auto plane = make_tracery_facet_plane(mesh, generated.source_facet_id);
            if (!plane) {
                ++statistics.skipped_plane;
                if (diagnostic)
                    diagnostic("projection_skipped\tvolume=" + std::to_string(volume_index) +
                               "\tfragment=" + std::to_string(generated.fragment_id) +
                               "\tfacet=" + std::to_string(generated.source_facet_id) +
                               "\tfilament=" + std::to_string(filament) + "\treason=invalid_source_facet_plane");
                facet_entries.erase(entry);
                continue;
            }
            data.emplace_back(std::move(*plane));
        }
        if (const auto found = facet_entries.find(generated.source_facet_id); found != facet_entries.end()) {
            TriangleSelector::TraceryFacet2DData& facet = data[found->second];
            TriangleSelector::TraceryRegion2D region;
            region.color = EnforcerBlockerType(filament);
            region.fragment_id = generated.fragment_id;
            for (const Vec3f& vertex : generated.local_polygon) {
                const Vec3d delta = vertex.cast<double>() - facet.origin;
                region.polygon.emplace_back(delta.dot(facet.axis_u), delta.dot(facet.axis_v));
            }
            update_tracery_region_bounds(region);
            const double area_2d = polygon_area_2d(region.polygon);
            bool finite = true;
            for (const Vec2d& vertex : region.polygon)
                finite = finite && std::isfinite(vertex.x()) && std::isfinite(vertex.y());
            if (!finite)
                ++statistics.non_finite;
            if (area_3d <= 1e-12 || area_2d <= 1e-12)
                ++statistics.degenerate;
            ++statistics.output;
            statistics.output_area_2d += area_2d;
            if (diagnostic) {
                std::ostringstream detail;
                detail << std::setprecision(12)
                       << "projection_output\tvolume=" << volume_index
                       << "\tfragment=" << generated.fragment_id
                       << "\tfacet=" << generated.source_facet_id
                       << "\tfilament=" << filament
                       << "\tvertices=" << region.polygon.size()
                       << "\tarea_3d=" << area_3d
                       << "\tarea_2d=" << area_2d
                       << "\tfinite=" << finite;
                diagnostic(detail.str());
            }
            facet.regions.emplace_back(std::move(region));
        }
    }
    if (diagnostic)
        for (const auto& [source_facet_id, statistics] : statistics_by_facet) {
            std::ostringstream detail;
            detail << std::setprecision(12)
                   << "projection_facet_summary\tvolume=" << volume_index
                   << "\tfacet=" << source_facet_id
                   << "\tinput=" << statistics.input
                   << "\toutput=" << statistics.output
                   << "\tskipped_fragment_id=" << statistics.skipped_fragment_id
                   << "\tskipped_polygon=" << statistics.skipped_polygon
                   << "\tskipped_filament=" << statistics.skipped_filament
                   << "\tskipped_plane=" << statistics.skipped_plane
                   << "\tnon_finite=" << statistics.non_finite
                   << "\tdegenerate=" << statistics.degenerate
                   << "\toutput_area_2d=" << statistics.output_area_2d;
            diagnostic(detail.str());
        }
    data.erase(std::remove_if(data.begin(), data.end(), [](const auto& facet) { return facet.regions.empty(); }), data.end());
    return data;
}

TriangleSelector::Tracery2DData GLGizmoTracery::build_tracery_2d_data(size_t volume_index) const
{
    TriangleSelector::Tracery2DData result;
    std::ofstream diagnostic;
#ifndef NDEBUG
    if (m_display_diagnostics_active) {
        const std::filesystem::path path = m_display_diagnostics_dir /
            ("tracery_2d_volume_" + std::to_string(volume_index) + ".tsv");
        const bool new_file = !std::filesystem::exists(path);
        diagnostic.open(path, std::ios::app | std::ios::binary);
        if (diagnostic && new_file)
            diagnostic << "record\tvolume\tfragment\tsource_facet\tcolor\tdetail\n";
    }
#endif
    const auto trace = [&diagnostic, volume_index](const std::string& record, size_t fragment, int source_facet,
                                                    int color, const std::string& detail) {
        if (diagnostic)
            diagnostic << record << '\t' << volume_index << '\t' << fragment << '\t' << source_facet << '\t'
                       << color << "\t" << detail << '\n';
    };
    if (volume_index >= m_generated_fragments.size())
        return result;

    const Model& model = wxGetApp().plater()->model();
    const ModelObject* object = nullptr;
    for (const ModelObject* candidate : model.objects)
        if (candidate->id() == m_mask_object_id) {
            object = candidate;
            break;
        }
    if (object == nullptr && m_mask_object_index >= 0 && size_t(m_mask_object_index) < model.objects.size())
        object = model.objects[size_t(m_mask_object_index)];
    if (object == nullptr)
        return result;

    const ModelVolume* target_volume = nullptr;
    size_t current_volume_index = 0;
    for (const ModelVolume* volume : object->volumes) {
        if (!volume->is_model_part())
            continue;
        if (current_volume_index++ == volume_index) {
            target_volume = volume;
            break;
        }
    }
    if (target_volume == nullptr)
        return result;

    const indexed_triangle_set& mesh = target_volume->mesh().its;
    std::map<int, size_t> facet_entries;
    const auto ensure_facet = [&result, &facet_entries, &mesh, &trace](int source_facet_id) -> TriangleSelector::TraceryFacet2DData* {
        if (source_facet_id < 0 || size_t(source_facet_id) >= mesh.indices.size())
            return nullptr;
        if (const auto found = facet_entries.find(source_facet_id); found != facet_entries.end())
            return &result[found->second];
        const stl_triangle_vertex_indices& face = mesh.indices[size_t(source_facet_id)];
        if (face[0] < 0 || face[1] < 0 || face[2] < 0 ||
            size_t(face[0]) >= mesh.vertices.size() || size_t(face[1]) >= mesh.vertices.size() ||
            size_t(face[2]) >= mesh.vertices.size())
            return nullptr;
        const Vec3d origin = mesh.vertices[size_t(face[0])].cast<double>();
        Vec3d axis_u = mesh.vertices[size_t(face[1])].cast<double>() - origin;
        const Vec3d edge_v = mesh.vertices[size_t(face[2])].cast<double>() - origin;
        Vec3d normal = axis_u.cross(edge_v);
        if (axis_u.squaredNorm() <= 1e-18 || normal.squaredNorm() <= 1e-18)
            return nullptr;
        axis_u.normalize();
        normal.normalize();
        Vec3d axis_v = normal.cross(axis_u);
        axis_v.normalize();
        const size_t entry_index = result.size();
        result.emplace_back(TriangleSelector::TraceryFacet2DData{ source_facet_id, origin, axis_u, axis_v, {} });
        facet_entries.emplace(source_facet_id, entry_index);
        std::ostringstream plane;
        plane << std::setprecision(12)
              << "origin=" << origin.x() << ',' << origin.y() << ',' << origin.z()
              << ";axis_u=" << axis_u.x() << ',' << axis_u.y() << ',' << axis_u.z()
              << ";axis_v=" << axis_v.x() << ',' << axis_v.y() << ',' << axis_v.z()
              << ";source_vertices="
              << mesh.vertices[size_t(face[0])].x() << ',' << mesh.vertices[size_t(face[0])].y() << ',' << mesh.vertices[size_t(face[0])].z() << ';'
              << mesh.vertices[size_t(face[1])].x() << ',' << mesh.vertices[size_t(face[1])].y() << ',' << mesh.vertices[size_t(face[1])].z() << ';'
              << mesh.vertices[size_t(face[2])].x() << ',' << mesh.vertices[size_t(face[2])].y() << ',' << mesh.vertices[size_t(face[2])].z();
        trace("facet_plane", 0, source_facet_id, 0, plane.str());
        return &result.back();
    };

    for (const GeneratedFragment& fragment : m_generated_fragments[volume_index]) {
        if (fragment.fragment_id >= m_fragment_mixed_filament_ids.size() ||
            fragment.source_facet_ids.size() != fragment.surface.indices.size()) {
            trace("fragment_skipped", fragment.fragment_id, -1, 0,
                  "faces=" + std::to_string(fragment.surface.indices.size()) +
                  ",source_ids=" + std::to_string(fragment.source_facet_ids.size()));
            continue;
        }
        const unsigned int filament_id = m_fragment_mixed_filament_ids[fragment.fragment_id];
        if (filament_id == 0 || filament_id > unsigned(EnforcerBlockerType::ExtruderMax)) {
            trace("fragment_skipped", fragment.fragment_id, -1, int(filament_id), "invalid_filament");
            continue;
        }
        trace("fragment_input", fragment.fragment_id, -1, int(filament_id),
              "vertices=" + std::to_string(fragment.surface.vertices.size()) +
              ",faces=" + std::to_string(fragment.surface.indices.size()));
        struct ProjectionStatistics {
            size_t input_faces = 0;
            size_t invalid_vertices = 0;
            size_t invalid_source_facet = 0;
            size_t degenerate_3d = 0;
            size_t noncoplanar = 0;
            size_t degenerate_2d = 0;
            size_t output_regions = 0;
            size_t sample_count = 0;
            double output_area_2d = 0.0;
            double maximum_plane_distance = 0.0;
        };
        std::map<int, ProjectionStatistics> projection_statistics;
        for (size_t face_index = 0; face_index < fragment.surface.indices.size(); ++face_index) {
            const stl_triangle_vertex_indices& face = fragment.surface.indices[face_index];
            const int source_facet_id = fragment.source_facet_ids[face_index];
            ProjectionStatistics* statistics = nullptr;
            if (diagnostic) {
                statistics = &projection_statistics[source_facet_id];
                ++statistics->input_faces;
            }
            if (face[0] < 0 || face[1] < 0 || face[2] < 0 ||
                size_t(face[0]) >= fragment.surface.vertices.size() ||
                size_t(face[1]) >= fragment.surface.vertices.size() ||
                size_t(face[2]) >= fragment.surface.vertices.size()) {
                if (statistics != nullptr)
                    ++statistics->invalid_vertices;
                trace("face_skipped", fragment.fragment_id, source_facet_id, int(filament_id), "invalid_vertex_index");
                continue;
            }
            TriangleSelector::TraceryFacet2DData* facet = ensure_facet(source_facet_id);
            if (facet == nullptr) {
                if (statistics != nullptr)
                    ++statistics->invalid_source_facet;
                trace("face_skipped", fragment.fragment_id, source_facet_id, int(filament_id),
                      "invalid_source_facet");
                continue;
            }
            const std::array<Vec3d, 3> vertices_3d = {
                fragment.surface.vertices[size_t(face[0])].cast<double>(),
                fragment.surface.vertices[size_t(face[1])].cast<double>(),
                fragment.surface.vertices[size_t(face[2])].cast<double>()
            };
            const double area_3d = 0.5 * (vertices_3d[1] - vertices_3d[0]).cross(vertices_3d[2] - vertices_3d[0]).norm();
            const Vec3d plane_normal = facet->axis_u.cross(facet->axis_v);
            std::array<double, 3> plane_distances;
            for (size_t corner = 0; corner < plane_distances.size(); ++corner)
                plane_distances[corner] = (vertices_3d[corner] - facet->origin).dot(plane_normal);
            const double maximum_plane_distance = std::max({ std::abs(plane_distances[0]), std::abs(plane_distances[1]),
                                                              std::abs(plane_distances[2]) });
            if (statistics != nullptr) {
                statistics->maximum_plane_distance = std::max(statistics->maximum_plane_distance, maximum_plane_distance);
                if (maximum_plane_distance > 1e-5)
                    ++statistics->noncoplanar;
            }
            TriangleSelector::TraceryRegion2D region;
            region.color = EnforcerBlockerType(filament_id);
            region.fragment_id = fragment.fragment_id;
            for (size_t corner = 0; corner < 3; ++corner) {
                const Vec3d delta = vertices_3d[corner] - facet->origin;
                region.polygon.emplace_back(delta.dot(facet->axis_u), delta.dot(facet->axis_v));
            }
            update_tracery_region_bounds(region);
            const Vec2d first_edge = region.polygon[1] - region.polygon[0];
            const Vec2d second_edge = region.polygon[2] - region.polygon[0];
            const double twice_area_2d = first_edge.x() * second_edge.y() - first_edge.y() * second_edge.x();
            if (area_3d > 1e-12 && std::abs(twice_area_2d) > 1e-12) {
                facet->regions.emplace_back(std::move(region));
                const TriangleSelector::TraceryRegion2D& written = facet->regions.back();
                if (statistics != nullptr) {
                    ++statistics->output_regions;
                    statistics->output_area_2d += std::abs(twice_area_2d) * 0.5;
                }
                std::ostringstream detail;
                detail << std::setprecision(12)
                       << written.polygon[0].x() << ',' << written.polygon[0].y() << ';'
                       << written.polygon[1].x() << ',' << written.polygon[1].y() << ';'
                       << written.polygon[2].x() << ',' << written.polygon[2].y();
                trace("region_output", fragment.fragment_id, facet->source_facet_id, int(filament_id), detail.str());
            } else {
                if (statistics != nullptr) {
                    if (area_3d <= 1e-12)
                        ++statistics->degenerate_3d;
                    else
                        ++statistics->degenerate_2d;
                    if (statistics->sample_count++ < 12) {
                        std::ostringstream detail;
                        detail << std::setprecision(12)
                               << "face=" << face_index << ";area_3d=" << area_3d
                               << ";area_2d=" << std::abs(twice_area_2d) * 0.5
                               << ";plane_distances=" << plane_distances[0] << ',' << plane_distances[1] << ',' << plane_distances[2]
                               << ";vertices_3d="
                               << vertices_3d[0].x() << ',' << vertices_3d[0].y() << ',' << vertices_3d[0].z() << '|'
                               << vertices_3d[1].x() << ',' << vertices_3d[1].y() << ',' << vertices_3d[1].z() << '|'
                               << vertices_3d[2].x() << ',' << vertices_3d[2].y() << ',' << vertices_3d[2].z()
                               << ";vertices_2d="
                               << region.polygon[0].x() << ',' << region.polygon[0].y() << '|'
                               << region.polygon[1].x() << ',' << region.polygon[1].y() << '|'
                               << region.polygon[2].x() << ',' << region.polygon[2].y();
                        trace("degenerate_projection_sample", fragment.fragment_id, facet->source_facet_id,
                              int(filament_id), detail.str());
                    }
                }
                trace("face_skipped", fragment.fragment_id, facet->source_facet_id, int(filament_id),
                      area_3d <= 1e-12 ? "degenerate_3d" : "degenerate_2d");
            }
        }
        if (diagnostic) {
            for (const auto& [source_facet_id, statistics] : projection_statistics) {
                trace("projection_summary", fragment.fragment_id, source_facet_id, int(filament_id),
                      "input=" + std::to_string(statistics.input_faces) +
                      ",invalid_vertices=" + std::to_string(statistics.invalid_vertices) +
                      ",invalid_source=" + std::to_string(statistics.invalid_source_facet) +
                      ",degenerate_3d=" + std::to_string(statistics.degenerate_3d) +
                      ",noncoplanar=" + std::to_string(statistics.noncoplanar) +
                      ",degenerate_2d=" + std::to_string(statistics.degenerate_2d) +
                      ",output_regions=" + std::to_string(statistics.output_regions) +
                      ",output_area_2d=" + std::to_string(statistics.output_area_2d) +
                      ",max_plane_distance=" + std::to_string(statistics.maximum_plane_distance));
            }
        }
    }
    for (const TriangleSelector::TraceryFacet2DData& facet : result)
        trace("facet_summary", 0, facet.source_facet_id, 0,
              "regions=" + std::to_string(facet.regions.size()));
    result.erase(std::remove_if(result.begin(), result.end(),
                                [](const TriangleSelector::TraceryFacet2DData& facet) { return facet.regions.empty(); }),
                 result.end());
    return result;
}

void GLGizmoTracery::update_from_model_object(bool)
{
    TraceryPerformanceScope performance("selector_refresh");
    performance.stage("selector.check_inputs");
    const ModelObject* model_object = m_c->selection_info()->model_object();
    if (model_object == nullptr)
        return;

    std::vector<ColorRGBA> extruder_colors = ::get_extruders_colors();
    if (extruder_colors.empty())
        extruder_colors.push_back(GLVolume::NEUTRAL_COLOR);
    std::optional<SelectorRefreshKey> refresh_key;
    if (m_commit_refresh_active) {
        std::vector<SelectorVolumeKey> volumes;
        for (const auto* volume : model_object->volumes) {
            if (!volume->is_model_part())
                continue;
            volumes.emplace_back(volume->id(), volume->mesh_ptr(), volume->extruder_id(),
                std::array<uint64_t, 5>{volume->mmu_segmentation_facets.timestamp(), volume->supported_facets.timestamp(),
                    volume->seam_facets.timestamp(), volume->fuzzy_skin_facets.timestamp(), volume->exterior_facets.timestamp()},
                volume->tracery_edit_data);
        }
        refresh_key.emplace(model_object, model_object->id(), std::move(volumes), extruder_colors);
        if (m_commit_selector_key == refresh_key && m_triangle_selectors.size() == std::get<2>(*refresh_key).size() &&
            m_fill_reference_selectors.size() == m_triangle_selectors.size()) {
            performance.count("cache_hit", 1);
            performance.stage("selector.refresh_policy");
            refresh_source_selection_policy(true);
            return;
        }
    }
    performance.count("cache_hit", 0);
    performance.stage("selector.rebuild");

    int current_object_index = -1;
    if (const Model* model = m_parent.get_model()) {
        for (size_t index = 0; index < model->objects.size(); ++index) {
            if (model->objects[index] == model_object) {
                current_object_index = int(index);
                break;
            }
        }
    }
    std::vector<size_t> document_hashes;
    for (const auto* volume : model_object->volumes)
        if (volume->is_model_part())
            document_hashes.push_back(volume->tracery_edit_data.empty() ? 0 : std::hash<std::string>{}(volume->tracery_edit_data));
    const bool document_changed = document_hashes != m_edit_document_hashes;
    m_edit_document_hashes      = std::move(document_hashes);
    // Sidebar/material updates may rebuild ModelObject instances and assign a
    // new ObjectID without changing the selected project object. Preserve its
    // in-memory mask in that case; a different project object still clears it.
    const bool restore_mask = !document_changed &&
                              (model_object->id() == m_mask_object_id ||
                               (!m_mask_data.empty() && current_object_index >= 0 && current_object_index == m_mask_object_index));
    if (restore_mask) {
        m_mask_object_id    = model_object->id();
        m_mask_object_index = current_object_index;
    }
    if (!restore_mask) {
        m_mask_data.clear();
        m_previous_mask_data.clear();
        m_stained_glass_base_paint_data.clear();
        m_generated_fragments.clear();
        m_generated_voronoi_regions.clear();
        m_tracery_selections.clear();
        m_incremental_generation = false;
        m_mask_object_index      = -1;
    }
    m_triangle_selectors.clear();
    m_fill_reference_selectors.clear();
    m_source_selection_policy.clear();
    // Rebuild every entry, including parts without a document. Resizing to a
    // later document index would otherwise create empty, falsely valid slots.
    m_stained_glass_base_paint_data.clear();
    for (const ModelVolume* volume : model_object->volumes) {
        if (!volume->is_model_part())
            continue;
        const size_t selector_idx = m_triangle_selectors.size();
        std::shared_ptr<const TraceryDocument> base_document;
        // Persistent data is authoritative after undo/redo and material updates;
        // a cached mask may describe a different derived mesh with the same ID.
        if (!volume->tracery_edit_data.empty()) {
            try {
                const auto document = TraceryDocument::decode_cached(volume->tracery_edit_data);
                if (document->matches(volume->mesh())) {
                    m_mask_data.resize(selector_idx + 1);
                    m_mask_data[selector_idx]                     = document->last_annotations()[6];
                    base_document = document;
                    m_mask_object_id                              = model_object->id();
                    m_mask_object_index                           = current_object_index;
                }
            } catch (const std::exception& error) {
                BOOST_LOG_TRIVIAL(error) << "Cannot restore stained-glass editing data: " << error.what();
            }
        }
        tracery_append_base_paint(m_stained_glass_base_paint_data, volume->mmu_segmentation_facets.get_data(),
            base_document ? &base_document->last_annotations()[5] : nullptr);
        const int              color_idx = std::clamp(volume->extruder_id() - 1, 0, int(extruder_colors.size()) - 1);
        std::vector<ColorRGBA> ebt_colors{extruder_colors[color_idx]};
        ebt_colors.insert(ebt_colors.end(), extruder_colors.begin(), extruder_colors.end());
        auto       fill_reference = std::make_unique<TriangleSelectorPatch>(volume->mesh(), ebt_colors, 0.2f);
        fill_reference->preserve_subdivision(true);
        const auto max_ebt        = EnforcerBlockerType(std::min(extruder_colors.size(), size_t(EnforcerBlockerType::ExtruderMax)));
        if (selector_idx < m_stained_glass_base_paint_data.size())
            fill_reference->deserialize(m_stained_glass_base_paint_data[selector_idx], false, max_ebt);
        else
            fill_reference->deserialize(volume->mmu_segmentation_facets.get_data(), false, max_ebt);
        if (!volume->tracery_edit_data.empty()) {
            try {
                const auto document = TraceryDocument::decode_cached(volume->tracery_edit_data);
                if (document->is_vector() && !document->selection_reference().triangles_to_split.empty())
                    fill_reference->deserialize(document->selection_reference(), true, max_ebt);
            } catch (const std::exception& error) {
                BOOST_LOG_TRIVIAL(error) << "Cannot restore stained-glass selection: " << error.what();
            }
        }
        // Keep the normal material selector and its white seed-fill contour,
        // but hide only its coloured paint patches while editing stained glass.
        fill_reference->set_paint_fill_visible(false);
        fill_reference->request_update_render_data();
        m_fill_reference_selectors.emplace_back(std::move(fill_reference));

        // Keep the effect layer on exactly the material selector's leaf tree.
        // Existing documents may contain an older, independently refined
        // mask; sample it once while migrating to the shared topology.
        m_triangle_selectors.emplace_back(std::make_unique<TriangleSelectorGUI>(volume->mesh(), 0.2f));
        TriangleSelectorGUI& effect = *m_triangle_selectors.back();
        effect.deserialize(m_fill_reference_selectors.back()->serialize(), false);
        if (selector_idx < m_mask_data.size()) {
            TriangleSelector previous_effect(volume->mesh());
            previous_effect.deserialize(m_mask_data[selector_idx], false);
            effect.set_leaf_states(
                [&previous_effect](const Vec3f& point, int source_facet) { return previous_effect.state_at(point, source_facet); });
        } else {
            effect.set_leaf_states({});
        }
        m_triangle_selectors.back()->request_update_render_data();
    }
    // The model document owns the vector effect. Reopening only restores the
    // selection tools; surface rendering reads the same document independently.
    refresh_source_selection_policy(true);
    if (m_commit_refresh_active)
        m_commit_selector_key = std::move(refresh_key);
}

void GLGizmoTracery::on_shutdown()
{
    // Completed edits are already stored on the model volume. The shared
    // surface renderer and slicer continue to read them after the gizmo closes.
}

void GLGizmoTracery::on_load(cereal::BinaryInputArchive& ar)
{
    GLGizmoPainterBase::on_load(ar);
    ar(m_mask_object_id, m_mask_data, m_stained_glass_base_paint_data, m_fragment_density, m_refraction_strength,
       m_mesh_restore_states, m_regeneration_version);
    m_density_input = m_fragment_density;
    m_refraction_input = m_refraction_strength;
    m_regeneration_requested = false;
    m_generated_fragments.clear();
    m_generated_voronoi_regions.clear();
    m_tracery_selections.clear();
    m_previous_mask_data.clear();
    m_incremental_generation = false;
}

void GLGizmoTracery::on_save(cereal::BinaryOutputArchive& ar) const
{
    ar(m_mask_object_id, m_mask_data, m_stained_glass_base_paint_data, m_fragment_density, m_refraction_strength,
       m_mesh_restore_states, m_regeneration_version);
}

PainterGizmoType GLGizmoTracery::get_painter_type() const
{
    return PainterGizmoType::TRACERY;
}

wxString GLGizmoTracery::handle_snapshot_action_name(bool shift_down, Button) const
{
    return shift_down ? _L("Erase stained glass area") : _L("Paint stained glass area");
}

} // namespace Slic3r::GUI
