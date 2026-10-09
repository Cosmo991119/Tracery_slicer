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

#include "libslic3r/AABBMesh.hpp"
#include "libslic3r/KDTreeIndirect.hpp"
#include "libslic3r/Utils.hpp"

#include <GL/glew.h>
#include <wx/image.h>
#include <wx/stdpaths.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
#include <set>
#include <stdexcept>
#include <utility>

namespace {

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

// Diagnostic PLY uses one coloured vertex per triangle so neighbouring
// fragments remain independently inspectable in a mesh viewer.
template<class Enumerate>
void write_tracery_diagnostic_ply(const std::filesystem::path& path, size_t faces, Enumerate enumerate)
{
    std::ofstream output(path, std::ios::binary);
    if (!output)
        throw std::runtime_error("Cannot create stained-glass diagnostic PLY");
    output << "ply\nformat ascii 1.0\n"
           << "element vertex " << faces * 3 << "\n"
           << "property float x\nproperty float y\nproperty float z\n"
           << "property uchar red\nproperty uchar green\nproperty uchar blue\n"
           << "element face " << faces << "\n"
           << "property list uchar int vertex_indices\nend_header\n";
    output << std::setprecision(9);
    size_t written_vertices = 0;
    enumerate([&](const Slic3r::Vec3f& position, const Slic3r::ColorRGBA& color) {
        const auto channel = [](float value) {
            return int(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
        };
        output << position.x() << ' ' << position.y() << ' ' << position.z() << ' '
               << channel(color.r()) << ' ' << channel(color.g()) << ' ' << channel(color.b()) << '\n';
        ++written_vertices;
    });
    if (written_vertices != faces * 3)
        throw std::runtime_error("Stained-glass diagnostic PLY face count mismatch");
     for (size_t face = 0; face < faces; ++face)
        output << "3 " << face * 3 << ' ' << face * 3 + 1 << ' ' << face * 3 + 2 << '\n';
    if (!output)
        throw std::runtime_error("Cannot finish stained-glass diagnostic PLY");
}

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

bool is_legal_tracery_recipe(const std::array<int, 4>& weights);

Slic3r::GUI::MixedColorMatchRecipeResult build_tracery_cmyw_recipe(
    const std::vector<std::string>& physical_colours, const wxColour& target, bool glass_recipe)
{
    Slic3r::GUI::MixedColorMatchRecipeResult best;
    if (!target.IsOk() || physical_colours.size() != 4)
        return best;

    std::vector<wxColour> palette;
    palette.reserve(4);
    for (const std::string& value : physical_colours) {
        const wxColour colour(value);
        if (!colour.IsOk())
            return best;
        palette.emplace_back(colour);
    }

    const auto encode_weights = [](const std::array<int, 4>& weights) {
        return std::to_string(weights[0]) + "/" + std::to_string(weights[1]) + "/" +
            std::to_string(weights[2]) + "/" + std::to_string(weights[3]);
    };
    const auto consider = [&best, &palette, &target, &encode_weights, glass_recipe](const std::array<int, 4>& weights) {
        if (glass_recipe) {
            if (!is_legal_tracery_recipe(weights))
                return;
        }
        std::vector<double> mix_weights;
        mix_weights.reserve(weights.size());
        for (int weight : weights)
            mix_weights.emplace_back(double(weight));
        const wxColour preview = Slic3r::GUI::blend_multi_filament_mixer(palette, mix_weights);
        const double delta_e = Slic3r::GUI::color_delta_e00(target, preview);
        if (best.valid && delta_e >= best.delta_e)
            return;

        best.valid = true;
        best.component_a = 1;
        best.component_b = 2;
        best.mix_b_percent = weights[0] + weights[1] > 0 ?
            int(std::lround(100.0 * double(weights[1]) / double(weights[0] + weights[1]))) : 0;
        best.gradient_component_ids = Slic3r::MixedFilamentManager::encode_gradient_component_ids({ 1, 2, 3, 4 });
        best.gradient_component_weights = encode_weights(weights);
        best.preview_color = preview;
        best.delta_e = delta_e;
    };

    // CMYW is a fixed four-channel palette, so this has only one component
    // combination. A 5% grid is cheap; refining the best cell at 1% preserves
    // responsive application even for a full stained-glass palette.
    constexpr int coarse_step = 5;
    std::array<int, 4> best_weights = { 0, 0, 0, 100 };
    for (int c = 0; c <= 100; c += coarse_step)
        for (int m = 0; m <= 100 - c; m += coarse_step)
            for (int y = 0; y <= 100 - c - m; y += coarse_step) {
                const std::array<int, 4> weights = { c, m, y, 100 - c - m - y };
                const double previous = best.delta_e;
                consider(weights);
                if (best.valid && best.delta_e != previous)
                    best_weights = weights;
            }

    for (int c = std::max(0, best_weights[0] - 4); c <= std::min(100, best_weights[0] + 4); ++c)
        for (int m = std::max(0, best_weights[1] - 4); m <= std::min(100 - c, best_weights[1] + 4); ++m)
            for (int y = std::max(0, best_weights[2] - 4); y <= std::min(100 - c - m, best_weights[2] + 4); ++y)
                consider({ c, m, y, 100 - c - m - y });

    return best;
}

struct TraceryEditorRecipe {
    std::array<int, 4> weights = { 0, 0, 0, 0 };
    wxColour            preview;
};

bool is_legal_tracery_recipe(const std::array<int, 4>& weights)
{
    int active_count = 0;
    int chromatic_count = 0;
    int total = 0;
    for (size_t index = 0; index < weights.size(); ++index) {
        total += weights[index];
        if (weights[index] == 0)
            continue;
        if (weights[index] < 15)
            return false;
        ++active_count;
        if (index < 3)
            ++chromatic_count;
    }
    if (total != 100 || active_count < 1 || active_count > 3 || chromatic_count > 2)
        return false;
    // A three-ingredient glass recipe must be one white ingredient plus two
    // CMY ingredients. This rejects both CMY-only and four-colour mud.
    return active_count != 3 || (weights[3] > 0 && chromatic_count == 2);
}

std::vector<TraceryEditorRecipe> build_tracery_editor_palette()
{
    const std::array<wxColour, 4> cmyw = {
        wxColour("#08ABFB"), wxColour("#D93B90"), wxColour("#F9ED3D"), wxColour("#FFFFFF")
    };
    std::vector<wxColour> colours(cmyw.begin(), cmyw.end());
    std::vector<TraceryEditorRecipe> palette;
    // Five-percent increments are sufficient for a visual palette, keep the
    // editor responsive, and leave 328 legal recipes before colour grouping.
    constexpr int step = 5;
    for (int c = 0; c <= 100; c += step)
        for (int m = 0; m <= 100 - c; m += step)
            for (int y = 0; y <= 100 - c - m; y += step) {
                const std::array<int, 4> weights = { c, m, y, 100 - c - m - y };
                if (!is_legal_tracery_recipe(weights))
                    continue;
                std::vector<double> mix_weights;
                mix_weights.reserve(weights.size());
                for (const int weight : weights)
                    mix_weights.emplace_back(weight);
                palette.push_back({ weights, Slic3r::GUI::blend_multi_filament_mixer(colours, mix_weights) });
            }
    return palette;
}

double tracery_recipe_balance_penalty(const std::array<int, 4>& weights)
{
    int minimum = 101;
    int maximum = 0;
    for (const int weight : weights)
        if (weight > 0) {
            minimum = std::min(minimum, weight);
            maximum = std::max(maximum, weight);
        }
    // This is deliberately a preference rather than another hard cutoff: a
    // dominant colour is sometimes required to preserve a saturated source.
    return minimum <= 0 ? 0.0 : 0.08 * double(maximum - minimum);
}

} // namespace

namespace Slic3r::GUI {

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
    return GLGizmoPainterBase::gizmo_event(action, mouse_position, shift_down, alt_down, control_down);
}

std::string GLGizmoTracery::on_get_name() const
{
    return _u8L("Stained glass");
}

void GLGizmoTracery::render_painter_gizmo()
{
    // The complete result is regular MMU paint. Do not draw a selection mask,
    // cursor, clip cap or instance hider over that same surface: any of these
    // coplanar editing aids can be mistaken for a stained-glass fragment and
    // can depth-fight with the committed colour triangles.
}

void GLGizmoTracery::start_display_diagnostics()
{
    try {
        m_display_diagnostics_dir = std::filesystem::path(wxStandardPaths::Get().GetDocumentsDir().ToStdWstring()) /
            "TraceryDiagnostics" /
            ("display-run-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(m_display_diagnostics_dir);
        std::ofstream readme(m_display_diagnostics_dir / "README.txt", std::ios::binary);
        readme << "Display diagnostics for stained glass. Normal rendering is not changed.\n"
               << "events.tsv records model/paint identity, fragment counts and rendering decisions.\n"
               << "*_mask.png is the stencil buffer immediately after the selected-surface pass.\n"
               << "*_glass.png is the framebuffer after glass, before the source model is drawn.\n"
               << "*_final.png is the framebuffer after the source model is drawn.\n"
               << "*_fallback_final.png is the normal-model fallback path (no stencil pass).\n"
               << "global_canvas_route records every active canvas's ordinary model route.\n"
               << "after_mix_canvas_*_opaque_final.png captures the ordinary view after leaving the editor.\n"
               << "An absent stage means that path was not reached before the phase changed.\n"
               << "Each image is captured only once per phase; no diagnostic geometry is injected.\n";
        if (!readme)
            throw std::runtime_error("Cannot create display diagnostic README");
        m_display_diagnostics_phase = "editing";
        m_display_diagnostics_last_canvas_state.clear();
        m_display_diagnostics_captured.clear();
        m_display_diagnostics_active = true;
        display_canvas_probe.directory = m_display_diagnostics_dir;
        display_canvas_probe.phase = "editing";
        display_canvas_probe.last_canvas_states.clear();
        display_canvas_probe.captured_frames.clear();
        display_canvas_probe.active = true;
        record_display_diagnostic_state("start");
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
    record_display_diagnostic_state("stop");
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

    m_imgui->text(_L("Stained glass"));
    m_imgui->text_colored(ImGuiWrapper::COL_WINDOW_BG, _L("Paint the model surface to define the stained-glass area."));
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
        ImGui::PushStyleColor(ImGuiCol_Button, selected ? ImVec4(0.f, 0.59f, 0.53f, 0.25f) : ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.f, 0.59f, 0.53f, 0.18f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.f, 0.59f, 0.53f, 0.30f));
        if (ImGui::Button(into_u8(std::wstring(1, mode_icons[i])).c_str()))
            m_paint_mode = i == 0 ? PaintMode::SURFACE : PaintMode::THROUGH;
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(mode_tips[i], ImGui::GetFontSize() * 20.0f);
    }
    ImGui::Separator();
    m_imgui->text(_L("Tool type"));

    const std::array<wchar_t, 5> tool_ids = {
        ImGui::CircleButtonIcon, ImGui::SphereButtonIcon, ImGui::TriangleButtonIcon, ImGui::FillButtonIcon, ImGui::GapFillIcon
    };
    const std::array<wchar_t, 5> display_icons = m_is_dark_mode ? std::array<wchar_t, 5>{
        ImGui::CircleButtonDarkIcon, ImGui::SphereButtonDarkIcon, ImGui::TriangleButtonDarkIcon, ImGui::FillButtonDarkIcon, ImGui::GapFillDarkIcon
    } : std::array<wchar_t, 5>{
        ImGui::CircleButtonIcon, ImGui::SphereButtonIcon, ImGui::TriangleButtonIcon, ImGui::FillButtonIcon, ImGui::GapFillIcon
    };
    const std::array<wxString, 5> tool_tips = {
        _L("Surface brush"), _L("Volume brush"), _L("Face paint"), _L("Fill connected area"), _L("Eraser")
    };
    for (size_t i = 0; i < tool_ids.size(); ++i) {
        if (i != 0)
            ImGui::SameLine();
        const std::wstring label(1, display_icons[i]);
        const bool selected = m_current_tool == tool_ids[i];
        ImGui::PushStyleColor(ImGuiCol_Button, selected ? ImVec4(0.f, 0.59f, 0.53f, 0.25f) : ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.f, 0.59f, 0.53f, 0.18f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.f, 0.59f, 0.53f, 0.30f));
        if (ImGui::Button(into_u8(label).c_str())) {
            m_current_tool = tool_ids[i];
            m_eraser_mode = tool_ids[i] == ImGui::GapFillIcon;
        }
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(tool_tips[i], ImGui::GetFontSize() * 20.0f);
    }

    if (m_eraser_mode || m_current_tool == ImGui::CircleButtonIcon) {
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
        m_tool_type = ToolType::SMART_FILL;
    }

    if (m_tool_type == ToolType::BRUSH && m_cursor_type != TriangleSelector::CursorType::POINTER) {
        ImGui::AlignTextToFramePadding();
        m_imgui->text(_L("Brush size"));
        ImGui::SameLine();
        ImGui::PushItemWidth(m_imgui->scaled(8.f));
        m_imgui->bbl_slider_float_style("##tracery_brush_size", &m_cursor_radius, CursorRadiusMin, CursorRadiusMax, "%.2f", 1.0f, true);
    }

    ImGui::Separator();
    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Fragment density"));
    ImGui::SameLine();
    ImGui::PushItemWidth(m_imgui->scaled(8.f));
    m_imgui->bbl_slider_float_style("##tracery_density", &m_fragment_density, 0.f, 1.f, "%.0f%%", 1.0f, true);
    const bool density_edit_finished = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Refraction strength"));
    ImGui::SameLine();
    ImGui::PushItemWidth(m_imgui->scaled(8.f));
    m_imgui->bbl_slider_float_style("##tracery_refraction", &m_refraction_strength, 0.f, 1.f, "%.0f%%", 1.0f, true);
    const bool refraction_edit_finished = ImGui::IsItemDeactivatedAfterEdit();
    if (density_edit_finished || refraction_edit_finished) {
        generate_paint_plan();
        apply_stained_glass_colors();
        m_parent.set_as_dirty();
    }

    ImGui::Separator();
    if (m_imgui->button(_L("Regenerate fragments"))) {
        m_regeneration_requested = true;
        ++m_regeneration_version;
        generate_paint_plan();
        apply_stained_glass_colors();
        m_parent.set_as_dirty();
    }
    ImGui::Separator();
    if (m_imgui->button("Export geometry diagnostics")) {
        m_export_next_generation = true;
        capture_mask_and_generate();
    }
    if (m_imgui->button(m_display_diagnostics_active ? "Stop display diagnostics" : "Start display diagnostics")) {
        if (m_display_diagnostics_active)
            stop_display_diagnostics();
        else
            start_display_diagnostics();
    }
    if (ImGui::Checkbox("Diagnostic: stronger depth bias", &m_diagnostic_strong_depth_bias))
        m_parent.set_as_dirty();
    ImGui::Separator();
    if (m_imgui->button(_L("Erase all painting"))) {
        m_selection_modified = true;
        for (const auto& selector : m_triangle_selectors) {
            selector->reset();
            selector->request_update_render_data(true);
        }
        capture_mask_and_generate();
        apply_stained_glass_colors();
        m_parent.set_as_dirty();
    }
    ImGui::SameLine();
    m_imgui->text_colored(ImGuiWrapper::COL_WINDOW_BG,
        m_eraser_mode ? _L("Left mouse button: erase") : _L("Right mouse button: erase"));

    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();
}

void GLGizmoTracery::update_model_object()
{
    m_selection_modified = true;
    capture_mask_and_generate();
    // This gizmo is a destructive, generated paint brush. Commit on every
    // completed stroke so the standard colour-painting renderer owns the
    // visible result even while this editor remains open.
    apply_stained_glass_colors();
    m_parent.set_as_dirty();
}

void GLGizmoTracery::capture_mask_and_generate()
{
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
    m_mask_data.clear();
    m_mask_data.reserve(m_triangle_selectors.size());
    for (const std::unique_ptr<TriangleSelectorGUI>& selector : m_triangle_selectors)
        m_mask_data.emplace_back(selector->serialize());
    generate_paint_plan();
}

void GLGizmoTracery::generate_paint_plan()
{
    record_display_diagnostic_state("generation_begin");
    ScopeGuard log_generation_end([this]() { record_display_diagnostic_state("generation_end"); });
    const bool export_diagnostics = std::exchange(m_export_next_generation, false);
    m_generated_fragments.clear();
    m_fragment_colors.clear();
    m_fragment_mixed_filament_ids.clear();
    const ModelObject* model_object = m_c != nullptr && m_c->selection_info() != nullptr ?
        m_c->selection_info()->model_object() : nullptr;
    if (model_object == nullptr || model_object->id() != m_mask_object_id) {
        if (export_diagnostics)
            wxMessageBox(_L("Select a model and paint a stained-glass area before exporting diagnostics."),
                         _L("Stained glass"), wxOK | wxICON_INFORMATION);
        return;
    }

    m_generated_fragments.resize(m_triangle_selectors.size());

    struct SurfaceTriangle {
        size_t mesh_idx;
        Vec3f  a;
        Vec3f  b;
        Vec3f  c;
        Vec3f  world_a;
        Vec3f  world_b;
        Vec3f  world_c;
        Vec3f  normal;
        ColorRGBA source_color;
    };
    struct WorkTriangle {
        size_t mesh_idx;
        Vec3f  a;
        Vec3f  b;
        Vec3f  c;
        Vec3f  world_a;
        Vec3f  world_b;
        Vec3f  world_c;
        Vec3f  normal;
        ColorRGBA source_color;
        size_t color_group;
        float area;
    };
    struct ColorSource {
        ColorRGBA color;
        indexed_triangle_set facets;
        std::unique_ptr<AABBMesh> aabb;
    };
    struct ColorGroup {
        size_t component_id;
        ColorRGBA color;
    };

    const Model* canvas_model = m_parent.get_model();
    int object_idx = -1;
    if (canvas_model != nullptr) {
        for (size_t idx = 0; idx < canvas_model->objects.size(); ++idx) {
            if (canvas_model->objects[idx] == model_object) {
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
    std::vector<ColorGroup> color_groups;
    std::vector<std::vector<std::unique_ptr<ColorSource>>> color_sources(m_triangle_selectors.size());
    std::vector<SurfaceTriangle> surface_triangles;
    std::vector<Transform3d> volume_transforms;
    double surface_area = 0.0;
    size_t mesh_idx = 0;
    for (size_t volume_idx = 0; volume_idx < model_object->volumes.size(); ++volume_idx) {
        const ModelVolume* volume = model_object->volumes[volume_idx];
        if (!volume->is_model_part())
            continue;
        if (mesh_idx >= m_triangle_selectors.size())
            break;

        const indexed_triangle_set facets = m_triangle_selectors[mesh_idx]->get_facets_strict(EnforcerBlockerType::ENFORCER);
        const Transform3d& volume_transform = volume->get_matrix();
        volume_transforms.emplace_back(volume_transform);
        const ColorRGBA source_color = volume_source_color(int(volume_idx));

        // Multi-material painting stores its colours per surface facet, not
        // per ModelVolume. Mirror the same facet sets used by the normal
        // model renderer and use an AABB lookup below to sample the actual
        // source colour at each selected surface position.
        std::vector<indexed_triangle_set> facets_per_color;
        if (mesh_idx < m_stained_glass_base_paint_data.size()) {
            TriangleSelector base_selector(volume->mesh());
            base_selector.deserialize(m_stained_glass_base_paint_data[mesh_idx], false);
            base_selector.get_facets(facets_per_color);
        } else if (!volume->mmu_segmentation_facets.empty()) {
            volume->mmu_segmentation_facets.get_facets(*volume, facets_per_color);
        }
        if (!facets_per_color.empty()) {
            for (size_t color_idx = 0; color_idx < facets_per_color.size(); ++color_idx) {
                if (facets_per_color[color_idx].indices.empty())
                    continue;
                ColorRGBA painted_color = source_color;
                if (color_idx == 0) {
                    const int extruder_id = volume->extruder_id();
                    if (extruder_id > 0 && size_t(extruder_id) <= extruder_colors.size())
                        painted_color = adjust_color_for_rendering(extruder_colors[size_t(extruder_id - 1)]);
                } else if (color_idx <= extruder_colors.size()) {
                    painted_color = adjust_color_for_rendering(extruder_colors[color_idx - 1]);
                }
                painted_color.a(1.0f);
                //save model color source data
                auto source = std::make_unique<ColorSource>();
                source->color = painted_color;
                source->facets = std::move(facets_per_color[color_idx]);
                source->aabb = std::make_unique<AABBMesh>(source->facets);
                color_sources[mesh_idx].emplace_back(std::move(source));
            }
        }
        for (const stl_triangle_vertex_indices& face : facets.indices) {
            const Vec3f a = facets.vertices[face[0]];
            const Vec3f b = facets.vertices[face[1]];
            const Vec3f c = facets.vertices[face[2]];
            Vec3f normal = (b - a).cross(c - a);
            if (normal.squaredNorm() > 0.0f)
                normal.normalize();
            else
                normal = Vec3f::UnitZ();
            const Vec3f world_a = (volume_transform * a.cast<double>()).cast<float>();
            const Vec3f world_b = (volume_transform * b.cast<double>()).cast<float>();
            const Vec3f world_c = (volume_transform * c.cast<double>()).cast<float>();
            surface_area += 0.5 * double((world_b - world_a).cross(world_c - world_a).norm());
            surface_triangles.push_back({ mesh_idx, a, b, c, world_a, world_b, world_c, normal, source_color });
        }
        ++mesh_idx;
    }
    if (surface_triangles.empty()) {
        if (export_diagnostics)
            wxMessageBox(_L("Paint a stained-glass area before exporting diagnostics."), _L("Stained glass"), wxOK | wxICON_INFORMATION);
        return;
    }

    // Edge keys are used later to rebuild fine same-colour regions and to
    // find real borders between different original colours.
    struct EdgeKey {
        size_t mesh_idx;
        std::array<int64_t, 6> points;
        bool operator<(const EdgeKey& other) const {
            return mesh_idx != other.mesh_idx ? mesh_idx < other.mesh_idx : points < other.points;
        }
    };
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
    const auto sampled_source_color = [&color_sources](size_t mesh_idx, const Vec3f& local_position, const ColorRGBA& fallback) {
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
    // Fragment dimensions are expressed in model units (millimetres), never
    // as a fraction of the painted area. Larger selections gain more shards,
    // not larger circular cells.
    const float target_edge = 6.5f - 4.0f * m_fragment_density;
    const float maximum_fragment_diameter = target_edge * 1.35f;
    // Fragment ownership is evaluated on a substantially finer temporary
    // mesh than the fragment size. This lets the resulting boundary follow a
    // smooth curve while leaving the source model itself untouched.
    // Curves are tessellated on a common fine mesh.  This is deliberately
    // finer than the shard spacing: visual smoothness must not depend on the
    // original model's triangle size.
    float boundary_sample_edge = std::max(0.012f, target_edge * 0.05f);
    // The temporary surface must stay bounded even for a large painted model.
    // Without this cap, two density changes can each create millions of work
    // triangles and block the UI while their maps and adjacency lists rebuild.
    constexpr size_t max_work_triangles = 120000;
    const auto estimated_work_triangles = [&surface_triangles](float sample_edge) {
        double result = 0.0;
        for (const SurfaceTriangle& source : surface_triangles) {
            const float longest = std::max({ (source.world_b - source.world_a).norm(),
                (source.world_c - source.world_b).norm(), (source.world_a - source.world_c).norm() });
            const size_t subdivisions = size_t(std::clamp(int(std::ceil(longest / sample_edge)), 1, 96));
            result += double(subdivisions) * double(subdivisions);
        }
        return result;
    };
    for (int pass = 0; pass < 3; ++pass) {
        const double estimate = estimated_work_triangles(boundary_sample_edge);
        if (estimate <= max_work_triangles)
            break;
        boundary_sample_edge *= float(std::sqrt(estimate / double(max_work_triangles)));
    }
    std::vector<WorkTriangle> work_triangles;
    work_triangles.reserve(surface_triangles.size() * 24);
    for (const SurfaceTriangle& source : surface_triangles) {
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
                work_triangles.push_back({ source.mesh_idx, a, b, c, world_a, world_b, world_c, source.normal, color, 0,
                    0.5f * (world_b - world_a).cross(world_c - world_a).norm() });
                if (j < subdivisions - i - 1) {
                    const Vec3f d = point(source.a, source.b, source.c, i + 1, j + 1);
                    const Vec3f world_d = point(source.world_a, source.world_b, source.world_c, i + 1, j + 1);
                    const ColorRGBA color = sampled_source_color(source.mesh_idx, (b + d + c) / 3.0f, source.source_color);
                    work_triangles.push_back({ source.mesh_idx, b, d, c, world_b, world_d, world_c, source.normal, color, 0,
                        0.5f * (world_d - world_b).cross(world_c - world_b).norm() });
                }
            }
        }
    }

    // The original mesh is often very coarse. Rebuild the regions from the
    // fine surface samples instead: two samples belong together only when
    // they share an edge *and* have the same original displayed colour. This
    // keeps separate red islands independent and gives every colour region
    // its own fragment layout.
    color_groups.clear();
    std::vector<size_t> work_parents(work_triangles.size());
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
    std::map<EdgeKey, size_t> work_edge_owners;
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

    std::map<size_t, std::array<int64_t, 4>> work_anchors;
    for (size_t index = 0; index < work_triangles.size(); ++index) {
        const std::array<int64_t, 3> point = quantize_point(work_triangles[index].a);
        const std::array<int64_t, 4> anchor = { int64_t(work_triangles[index].mesh_idx), point[0], point[1], point[2] };
        const size_t root = find_work_root(index);
        const auto current = work_anchors.find(root);
        if (current == work_anchors.end() || anchor < current->second)
            work_anchors[root] = anchor;
    }
    std::map<size_t, size_t> work_group_indices;
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

    // A fragment is confined to one original-colour region. A red part can
    // therefore never be recoloured blue merely because another seed is near.
    std::vector<std::vector<size_t>> group_triangles(color_groups.size());
    std::vector<double> group_areas(color_groups.size(), 0.0);
    for (size_t index = 0; index < work_triangles.size(); ++index) {
        const WorkTriangle& triangle = work_triangles[index];
        group_triangles[triangle.color_group].emplace_back(index);
        group_areas[triangle.color_group] += triangle.area;
    }

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
    size_t fragment_count = 0;
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

    struct FragmentSeed {
        Vec3f position;
        size_t color_group;
        size_t stable_id;
    };
    std::vector<FragmentSeed> seeds;
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
        if (export_diagnostics)
            wxMessageBox(_L("No stained-glass fragments were generated for the painted area."),
                         _L("Stained glass"), wxOK | wxICON_INFORMATION);
        return;
    }

    struct CpuGeometry {
        indexed_triangle_set surface;

        bool empty() const { return surface.indices.empty(); }
        void add_vertex(const Vec3f& vertex) { surface.vertices.emplace_back(vertex); }
        void add_triangle(unsigned int first, unsigned int second, unsigned int third)
        {
            surface.indices.emplace_back(stl_triangle_vertex_indices{int(first), int(second), int(third)});
        }
    };
    std::vector<std::vector<CpuGeometry>> geometry(m_triangle_selectors.size(), std::vector<CpuGeometry>(fragment_count));

    const auto warped_position = [target_edge](const Vec3f& point) {
        // One continuous field is shared by every seed. Unlike per-seed
        // noise, it cannot tear a boundary into saw teeth. Voronoi borders
        // in this gently warped field become the required interlocking arcs.
        const float frequency = 1.35f / std::max(target_edge, 0.001f);
        const float amount = target_edge * 0.24f;
        return point + Vec3f(
            amount * std::sin((point.y() + 0.31f * point.z()) * frequency),
            amount * std::sin((point.z() + 0.27f * point.x()) * frequency + 1.7f),
            amount * std::sin((point.x() + 0.23f * point.y()) * frequency + 3.1f));
    };
    std::vector<Vec3f> warped_seeds;
    warped_seeds.reserve(seeds.size());
    for (const FragmentSeed& seed : seeds)
        warped_seeds.emplace_back(warped_position(seed.position));

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

    std::vector<size_t> fragment_for_triangle(work_triangles.size(), fragment_count);
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
                data.add_triangle(first, first + index, first + index + 1);
        }
    }

    struct BoundarySample {
        size_t first_triangle;
        size_t second_triangle;
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
    std::vector<std::set<size_t>> fragment_neighbours(fragment_count);
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

    std::vector<float> fragment_boundary_distance(fragment_count, std::numeric_limits<float>::max());
    std::vector<size_t> fragment_boundary_neighbor(fragment_count, no_group);
    for (size_t triangle = 0; triangle < work_triangles.size(); ++triangle) {
        const size_t fragment = fragment_for_triangle[triangle];
        if (fragment == fragment_count || boundary_distance[triangle] >= fragment_boundary_distance[fragment])
            continue;
        fragment_boundary_distance[fragment] = boundary_distance[triangle];
        fragment_boundary_neighbor[fragment] = boundary_neighbor[triangle];
    }

    std::vector<ColorRGBA> base_colors;
    base_colors.reserve(fragment_count);
    for (const FragmentSeed& seed : seeds)
        base_colors.emplace_back(color_groups[seed.color_group].color);
    const std::vector<TraceryEditorRecipe> editor_palette = build_tracery_editor_palette();
    if (editor_palette.empty())
        return;
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
        const float boundary_mix = proximity * (0.16f + 0.42f * m_refraction_strength);
        desired_colors[fragment] = glass_color(base_colors[fragment], boundary_color, boundary_mix, seeds[fragment].stable_id);
    }

    struct CandidateRecipe { size_t palette_index; double score; };
    std::vector<std::vector<CandidateRecipe>> fragment_candidates(fragment_count);
    for (size_t fragment = 0; fragment < fragment_count; ++fragment) {
        const wxColour target = tracery_wx_color(desired_colors[fragment]);
        auto& candidates = fragment_candidates[fragment];
        candidates.reserve(editor_palette.size());
        for (size_t recipe = 0; recipe < editor_palette.size(); ++recipe) {
            const double score = color_delta_e00(target, editor_palette[recipe].preview) +
                tracery_recipe_balance_penalty(editor_palette[recipe].weights);
            candidates.push_back({ recipe, score });
        }
        std::sort(candidates.begin(), candidates.end(), [](const CandidateRecipe& first, const CandidateRecipe& second) {
            return first.score < second.score;
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

    m_fragment_colors.resize(fragment_count);
    for (size_t fragment = 0; fragment < fragment_count; ++fragment) {
        const TraceryEditorRecipe& recipe = editor_palette[selected_recipe[fragment]];
        const wxColour& preview = recipe.preview;
        const ColorRGBA color(float(preview.Red()) / 255.0f, float(preview.Green()) / 255.0f,
            float(preview.Blue()) / 255.0f, 1.0f);
        m_fragment_colors[fragment] = color;
    }

    std::filesystem::path diagnostic_dir;
    std::ofstream diagnostic_stats;
    std::string diagnostic_error;
    bool diagnostic_ready = false;
    if (export_diagnostics) {
        try {
            diagnostic_dir = std::filesystem::path(wxStandardPaths::Get().GetDocumentsDir().ToStdWstring()) /
                "TraceryDiagnostics" /
                ("run-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(diagnostic_dir);

            write_tracery_diagnostic_ply(diagnostic_dir / "01_source_colours.ply", work_triangles.size(),
                [&](const auto& emit) {
                    for (const WorkTriangle& triangle : work_triangles) {
                        emit(triangle.world_a, triangle.source_color);
                        emit(triangle.world_b, triangle.source_color);
                        emit(triangle.world_c, triangle.source_color);
                    }
                });

            size_t clipped_faces = 0;
            for (const auto& per_volume : geometry)
                for (const CpuGeometry& data : per_volume)
                    clipped_faces += data.surface.indices.size();
            write_tracery_diagnostic_ply(diagnostic_dir / "02_clipped_geometry.ply", clipped_faces,
                [&](const auto& emit) {
                    for (size_t volume = 0; volume < geometry.size(); ++volume)
                        for (size_t fragment = 0; fragment < geometry[volume].size(); ++fragment) {
                            const CpuGeometry& data = geometry[volume][fragment];
                            for (const stl_triangle_vertex_indices& face : data.surface.indices)
                                for (size_t corner = 0; corner < 3; ++corner) {
                                    Vec3f local = Vec3f::Zero();
                                    if (face[corner] < data.surface.vertices.size())
                                        local = data.surface.vertices[face[corner]];
                                    emit((volume_transforms[volume] * local.cast<double>()).cast<float>(),
                                         m_fragment_colors[fragment]);
                                }
                        }
                });

            diagnostic_stats.open(diagnostic_dir / "mesh_comparison.csv", std::ios::binary);
            if (!diagnostic_stats)
                throw std::runtime_error("Cannot create stained-glass diagnostic report");
            diagnostic_stats << "volume,fragment,vertices_before,faces_before,vertices_after,faces_after,invalid_indices,tiny_faces,positions_equal,indices_equal\n";
            std::ofstream readme(diagnostic_dir / "README.txt", std::ios::binary);
            if (!readme)
                throw std::runtime_error("Cannot create stained-glass diagnostic README");
            readme << "01_source_colours.ply: selected work triangles, coloured by their sampled original colour.\n"
                   << "02_clipped_geometry.ply: coloured fragments just before conversion to indexed_triangle_set.\n"
                   << "03_exported_surface.ply: the exact indexed surface stored for slicing after conversion.\n"
                   << "mesh_comparison.csv: per-volume/per-fragment copy integrity and tiny-face counts.\n"
                   << "PLY coordinates are in object space, after the model-volume transform.\n";
            diagnostic_ready = true;
        } catch (const std::exception& error) {
            diagnostic_error = error.what();
        }
    }

    for (size_t fragment = 0; fragment < fragment_count; ++fragment) {
        for (size_t volume = 0; volume < geometry.size(); ++volume) {
            CpuGeometry& data = geometry[volume][fragment];
            if (data.empty())
                continue;
            indexed_triangle_set surface = std::move(data.surface);
            if (diagnostic_ready) {
                size_t tiny_faces = 0;
                for (size_t face = 0; face < surface.indices.size(); ++face) {
                    if (surface.indices[face][0] < surface.vertices.size() &&
                        surface.indices[face][1] < surface.vertices.size() &&
                        surface.indices[face][2] < surface.vertices.size()) {
                        const Vec3f& a = surface.vertices[surface.indices[face][0]];
                        const Vec3f& b = surface.vertices[surface.indices[face][1]];
                        const Vec3f& c = surface.vertices[surface.indices[face][2]];
                        if ((b - a).cross(c - a).squaredNorm() < 1e-12f)
                            ++tiny_faces;
                    }
                }
                diagnostic_stats << volume << ',' << fragment << ',' << surface.vertices.size() << ',' << surface.indices.size()
                                 << ',' << surface.vertices.size() << ',' << surface.indices.size() << ",0," << tiny_faces << ",1,1\n";
            }
            m_generated_fragments[volume].emplace_back(GeneratedFragment{ fragment, std::move(surface) });
        }
    }

    if (diagnostic_ready) {
        try {
            size_t exported_faces = 0;
            for (const auto& volume : m_generated_fragments)
                for (const GeneratedFragment& fragment : volume)
                    exported_faces += fragment.surface.indices.size();
            write_tracery_diagnostic_ply(diagnostic_dir / "03_exported_surface.ply", exported_faces,
                [&](const auto& emit) {
                    for (size_t volume = 0; volume < m_generated_fragments.size(); ++volume)
                        for (const GeneratedFragment& fragment : m_generated_fragments[volume])
                            for (const stl_triangle_vertex_indices& face : fragment.surface.indices)
                                for (size_t corner = 0; corner < 3; ++corner) {
                                    Vec3f local = Vec3f::Zero();
                                    if (face[corner] < fragment.surface.vertices.size())
                                        local = fragment.surface.vertices[face[corner]];
                                    emit((volume_transforms[volume] * local.cast<double>()).cast<float>(),
                                         m_fragment_colors[fragment.fragment_id]);
                                }
                });
            diagnostic_stats.close();
            wxMessageBox(_L("Stained-glass diagnostics saved to:\n") + wxString::FromUTF8(diagnostic_dir.u8string().c_str()),
                         _L("Stained glass"), wxOK | wxICON_INFORMATION);
        } catch (const std::exception& error) {
            diagnostic_error = error.what();
        }
    }
    if (!diagnostic_error.empty()) {
        wxMessageBox(_L("Could not export stained-glass diagnostics: ") + wxString::FromUTF8(diagnostic_error.c_str()),
                     _L("Stained glass"), wxOK | wxICON_WARNING);
    }
}

void GLGizmoTracery::apply_stained_glass_colors()
{
    m_display_diagnostics_phase = "mix_update";
    set_display_canvas_probe_phase(m_display_diagnostics_phase);
    record_display_diagnostic_state("mix_begin");
    ScopeGuard log_mix_end([this]() {
        record_display_diagnostic_state("mix_end");
        m_display_diagnostics_phase = "after_mix";
        m_display_diagnostics_last_canvas_state.clear();
        set_display_canvas_probe_phase(m_display_diagnostics_phase);
    });

    auto plan = prepare_stained_glass_apply();
    if (!plan)
        return;

    if (m_paint_mode == PaintMode::THROUGH)
        generate_through_fragments();

    commit_stained_glass_apply(std::move(*plan));
}

std::optional<GLGizmoTracery::StainedGlassApplyPlan> GLGizmoTracery::prepare_stained_glass_apply()
{
    if (m_fragment_colors.empty()) {
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
    auto& mixed_manager = bundle->mixed_filaments;

    const std::array<wxColour, 4> cmyw = {
        wxColour("#08ABFB"), wxColour("#D93B90"), wxColour("#F9ED3D"), wxColour("#FFFFFF")
    };
    const auto find_physical_colour = [](const std::vector<std::string>& colours, const wxColour& wanted) {
        for (size_t index = 0; index < colours.size(); ++index) {
            const wxColour actual(colours[index]);
            if (actual.IsOk() && actual.Red() == wanted.Red() && actual.Green() == wanted.Green() && actual.Blue() == wanted.Blue())
                return int(index);
        }
        return -1;
    };

    std::vector<wxColour> missing_base_colours;
    for (const wxColour& colour : cmyw)
        if (find_physical_colour(colour_option->values, colour) < 0)
            missing_base_colours.emplace_back(colour);
    std::vector<std::string> projected_colours = colour_option->values;
    for (const wxColour &colour : missing_base_colours)
        projected_colours.emplace_back(tracery_hex(colour));
    MixedFilamentManager projected_manager = mixed_manager;
    projected_manager.auto_generate(projected_colours);
    if (projected_colours.size() > MAXIMUM_EXTRUDER_NUMBER ||
        projected_manager.total_filaments(projected_colours.size()) > MAXIMUM_FILAMENT_NUMBER) {
        wxMessageBox(_L("There are not enough slots for the original colours and CMYW ingredients."),
            _L("Stained glass"), wxOK | wxICON_WARNING);
        return std::nullopt;
    }

    // Add all missing base colours in one Sidebar update. Adding a physical
    // filament rebuilds the color option, so no pointer into the old option is
    // used after this call.
    wxGetApp().sidebar().add_custom_filaments(missing_base_colours);
    record_display_diagnostic_state("after_add_custom_filaments");
    colour_option = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
    if (colour_option == nullptr)
        return std::nullopt;
    std::array<unsigned int, 4> cmyw_ids = {};
    std::vector<std::string> cmyw_colours;
    cmyw_colours.reserve(cmyw.size());
    for (size_t index = 0; index < cmyw.size(); ++index) {
        const int physical_index = find_physical_colour(colour_option->values, cmyw[index]);
        if (physical_index < 0)
            return std::nullopt;
        cmyw_ids[index] = unsigned(physical_index + 1);
        cmyw_colours.emplace_back(colour_option->values[size_t(physical_index)]);
    }
    // The actual CMYW recipe slots may be a mix of existing and newly added
    // filaments. On U1, make all four use Full Spectrum after their IDs are known.
    wxGetApp().sidebar().set_stained_glass_base_filament_preset(
        std::vector<unsigned int>(cmyw_ids.begin(), cmyw_ids.end()));

    const size_t available_mixed = MAXIMUM_FILAMENT_NUMBER - mixed_manager.total_filaments(colour_option->values.size());
    // A palette entry may resolve to an existing CMYW recipe, so do not cap
    // the candidates by free slots before recipe de-duplication.
    constexpr size_t palette_budget = 24;

    // Keep the established publishing path: it owns the virtual-filament
    // budget, creates valid recipe rows and maps their returned IDs to model
    // facets. The editor only supplies its constrained preview colours.
    struct PaletteColour {
        wxColour colour;
        size_t usage = 0;
    };
    std::vector<PaletteColour> candidates;
    constexpr double merge_delta_e = 7.0;
    for (const ColorRGBA& source : m_fragment_colors) {
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

    std::vector<size_t> fragment_palette(m_fragment_colors.size(), 0);
    for (size_t fragment = 0; fragment < m_fragment_colors.size(); ++fragment) {
        const wxColour colour = tracery_wx_color(m_fragment_colors[fragment]);
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
    const std::vector<wxColour> target_colours = palette_preview_colours;

    constexpr size_t no_new_row = size_t(-1);
    std::vector<MixedFilamentBatchEntry> new_rows;
    std::vector<size_t> target_row_indices(target_colours.size(), no_new_row);
    std::map<std::string, size_t> recipe_rows;
    std::map<std::string, unsigned int> existing_recipe_ids;
    // This is a live paint brush, not an export operation: later strokes must
    // refer to an equivalent existing virtual filament instead of consuming a
    // new slot for the same CMYW recipe.
    unsigned int virtual_id = unsigned(colour_option->values.size());
    for (const MixedFilament &row : mixed_manager.mixed_filaments()) {
        if (row.deleted || !row.enabled)
            continue;
        ++virtual_id;
        const std::string fingerprint = std::to_string(row.component_a) + ":" + std::to_string(row.component_b) + ":" +
            std::to_string(row.mix_b_percent) + ":" + row.gradient_component_ids + ":" + row.gradient_component_weights;
        existing_recipe_ids.emplace(fingerprint, virtual_id);
    }
    for (size_t index = 0; index < target_colours.size(); ++index) {
        MixedColorMatchRecipeResult recipe = build_tracery_cmyw_recipe(cmyw_colours, target_colours[index], true);
        if (!recipe.valid)
            continue;
        palette_preview_colours[index] = recipe.preview_color;

        const auto physical_id = [&cmyw_ids](unsigned int local_id) {
            return local_id >= 1 && local_id <= cmyw_ids.size() ? cmyw_ids[local_id - 1] : 0u;
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
        const std::string fingerprint = std::to_string(recipe.component_a) + ":" + std::to_string(recipe.component_b) + ":" +
            std::to_string(recipe.mix_b_percent) + ":" + recipe.gradient_component_ids + ":" + recipe.gradient_component_weights;
        if (const auto existing = existing_recipe_ids.find(fingerprint); existing != existing_recipe_ids.end()) {
            palette_filament_ids[index] = existing->second;
            continue;
        }
        if (const auto existing = recipe_rows.find(fingerprint); existing != recipe_rows.end()) {
            target_row_indices[index] = existing->second;
            continue;
        }
        MixedFilamentBatchEntry entry;
        entry.component_a = recipe.component_a;
        entry.component_b = recipe.component_b;
        entry.mix_b_percent = recipe.mix_b_percent;
        entry.manual_pattern = recipe.manual_pattern;
        entry.gradient_component_ids = recipe.gradient_component_ids;
        entry.gradient_component_weights = recipe.gradient_component_weights;
        entry.distribution_mode = recipe.gradient_component_ids.empty() ? int(MixedFilament::Simple) : int(MixedFilament::LayerCycle);
        entry.display_color = tracery_hex(recipe.preview_color);
        target_row_indices[index] = new_rows.size();
        recipe_rows.emplace(fingerprint, new_rows.size());
        new_rows.emplace_back(std::move(entry));
    }

    bool has_unresolved_recipe = false;
    for (size_t index = 0; index < target_row_indices.size(); ++index) {
        if (palette_filament_ids[index] == 0 && target_row_indices[index] == no_new_row) {
            has_unresolved_recipe = true;
            break;
        }
    }
    if (new_rows.size() > available_mixed || has_unresolved_recipe) {
        wxMessageBox(_L("Unable to make CMYW recipes for every model and stained-glass colour."),
            _L("Stained glass"), wxOK | wxICON_WARNING);
        return std::nullopt;
    }
    std::vector<unsigned int> assigned_ids;
    mixed_manager.add_batch_custom_filaments(new_rows, colour_option->values, &assigned_ids);
    for (size_t index = 0; index < candidates.size(); ++index) {
        const size_t row = target_row_indices[index];
        if (palette_filament_ids[index] == 0 && row != no_new_row && row < assigned_ids.size())
            palette_filament_ids[index] = assigned_ids[row];
    }
    if (std::any_of(palette_filament_ids.begin(), palette_filament_ids.end(), [](unsigned int id) { return id == 0; })) {
        wxMessageBox(_L("Unable to assign a CMYW recipe to every stained-glass colour."),
            _L("Stained glass"), wxOK | wxICON_WARNING);
        return std::nullopt;
    }
    StainedGlassApplyPlan plan;
    plan.mode = StainedGlassApplyPlan::Mode::BakeFragments;
    plan.fragment_mixed_filament_ids.resize(fragment_palette.size(), 0);
    for (size_t fragment = 0; fragment < fragment_palette.size(); ++fragment) {
        plan.fragment_mixed_filament_ids[fragment] = palette_filament_ids[fragment_palette[fragment]];
    }

    plan.mixed_filament_definitions = mixed_manager.serialize_custom_entries();
    return plan;
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

    size_t volume_index = 0;
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
}

void GLGizmoTracery::commit_stained_glass_apply(StainedGlassApplyPlan&& plan)
{
    if (plan.mode == StainedGlassApplyPlan::Mode::BakeFragments) {
        m_fragment_mixed_filament_ids = std::move(plan.fragment_mixed_filament_ids);
        if (PresetBundle* bundle = wxGetApp().preset_bundle) {
            if (auto* definitions = bundle->project_config.option<ConfigOptionString>("mixed_filament_definitions"))
                definitions->value = std::move(plan.mixed_filament_definitions);
        }
    }

    bake_fragments_to_mmu_painting();
    wxGetApp().sidebar().update_color_mix_panel();
    wxGetApp().plater()->post_slice_state_change_update();
    m_parent.set_as_dirty();
}

void GLGizmoTracery::bake_fragments_to_mmu_painting()
{
    Model &model = wxGetApp().plater()->model();
    ModelObject *object = nullptr;
    for (ModelObject *candidate : model.objects)
        if (candidate->id() == m_mask_object_id) { object = candidate; break; }
    if (object == nullptr && m_mask_object_index >= 0 && size_t(m_mask_object_index) < model.objects.size())
        object = model.objects[size_t(m_mask_object_index)];
    if (object == nullptr)
        return;

    bool updated = false;
    size_t volume_index = 0;
    for (ModelVolume *volume : object->volumes) {
        if (!volume->is_model_part())
            continue;
        struct GeneratedPaint {
            unsigned int filament_id;
            std::unique_ptr<AABBMesh> surface;
        };
        std::vector<GeneratedPaint> generated;
        BoundingBoxf3 region;
        if (volume_index < m_generated_fragments.size()) {
            for (const GeneratedFragment &fragment : m_generated_fragments[volume_index]) {
                if (fragment.fragment_id >= m_fragment_mixed_filament_ids.size())
                    continue;
                const unsigned int filament = m_fragment_mixed_filament_ids[fragment.fragment_id];
                if (filament == 0 || filament > unsigned(EnforcerBlockerType::ExtruderMax) || fragment.surface.indices.empty())
                    continue;
                for (const Vec3f &vertex : fragment.surface.vertices) {
                    const Vec3d point = vertex.cast<double>();
                    if (!region.defined) {
                        region.min = point;
                        region.max = point;
                        region.defined = true;
                    } else {
                        region.min = region.min.cwiseMin(point);
                        region.max = region.max.cwiseMax(point);
                    }
                }
                generated.push_back({ filament, std::make_unique<AABBMesh>(fragment.surface) });
            }
        }
        const bool has_base_paint = volume_index < m_stained_glass_base_paint_data.size();
        if (has_base_paint || !generated.empty()) {
            TriangleSelector selector(volume->mesh());
            if (has_base_paint)
                selector.deserialize(m_stained_glass_base_paint_data[volume_index], false);
            else
                selector.deserialize(volume->mmu_segmentation_facets.get_data(), false);
            if (!generated.empty()) {
                const float max_leaf_edge = std::clamp((6.5f - 4.0f * m_fragment_density) * 0.08f, 0.18f, 0.55f);
                selector.paint_generated(region, max_leaf_edge, [&generated](const Vec3f &point) {
                    constexpr double surface_epsilon_sq = 1e-8;
                    double nearest = std::numeric_limits<double>::max();
                    unsigned int filament = 0;
                    for (const GeneratedPaint &paint : generated) {
                        const double distance = paint.surface->squared_distance(point.cast<double>());
                        if (distance < nearest) {
                            nearest = distance;
                            filament = paint.filament_id;
                        }
                    }
                    return nearest <= surface_epsilon_sq ? EnforcerBlockerType(filament) : EnforcerBlockerType::NONE;
                });
            }
            updated |= volume->mmu_segmentation_facets.set(selector);
        }
        ++volume_index;
    }
    if (updated) {
        const ModelObjectPtrs &objects = wxGetApp().model().objects;
        const size_t object_index = std::find(objects.begin(), objects.end(), object) - objects.begin();
        wxGetApp().obj_list()->update_info_items(object_index);
        wxGetApp().plater()->get_partplate_list().notify_instance_update(object_index, 0);
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
        wxGetApp().plater()->notify_filament_usage_changed();
    }
    // Generated surfaces are CPU-only classifiers; release them immediately
    // after their colours have been committed to regular MMU paint.
    m_generated_fragments.clear();
}

void GLGizmoTracery::update_from_model_object(bool)
{
    record_display_diagnostic_state("editor_reopen_begin");
    const ModelObject* model_object = m_c->selection_info()->model_object();
    if (model_object == nullptr)
        return;

    int current_object_index = -1;
    if (const Model* model = m_parent.get_model()) {
        for (size_t index = 0; index < model->objects.size(); ++index) {
            if (model->objects[index] == model_object) {
                current_object_index = int(index);
                break;
            }
        }
    }
    // Sidebar/material updates may rebuild ModelObject instances and assign a
    // new ObjectID without changing the selected project object. Preserve its
    // in-memory mask in that case; a different project object still clears it.
    const bool restore_mask = model_object->id() == m_mask_object_id ||
        (!m_mask_data.empty() && current_object_index >= 0 && current_object_index == m_mask_object_index);
    if (restore_mask) {
        m_mask_object_id = model_object->id();
        m_mask_object_index = current_object_index;
    }
    if (!restore_mask) {
        m_mask_data.clear();
        m_stained_glass_base_paint_data.clear();
        m_generated_fragments.clear();
        m_mask_object_index = -1;
    }
    m_triangle_selectors.clear();
    const std::vector<ColorRGBA> colors = {
        GLVolume::NEUTRAL_COLOR,
        { 0.0f, 0.59f, 0.53f, 0.55f },
        { 0.0f, 0.59f, 0.53f, 0.0f }
    };
    for (const ModelVolume* volume : model_object->volumes) {
        if (!volume->is_model_part())
            continue;
        m_triangle_selectors.emplace_back(std::make_unique<TriangleSelectorPatch>(volume->mesh(), colors));
        const size_t selector_idx = m_triangle_selectors.size() - 1;
        if (restore_mask && selector_idx < m_mask_data.size())
            m_triangle_selectors.back()->deserialize(m_mask_data[selector_idx], false);
        m_triangle_selectors.back()->request_update_render_data();
    }
    // The finished effect is ordinary MMU paint. Reopening restores only the
    // editing mask; it never recreates a renderable fragment layer.
    record_display_diagnostic_state("editor_reopen_end",
        std::string("restore_mask=") + (restore_mask ? "1" : "0"));
    m_display_diagnostics_phase = "reentered";
    m_display_diagnostics_last_canvas_state.clear();
    set_display_canvas_probe_phase(m_display_diagnostics_phase);
}

void GLGizmoTracery::on_shutdown()
{
    record_display_diagnostic_state("shutdown_begin");
    // Every completed edit has already baked into normal colour painting.
    // Closing this gizmo must not create a second, delayed render/slice layer.
    m_display_diagnostics_phase = "after_exit";
    m_display_diagnostics_last_canvas_state.clear();
    set_display_canvas_probe_phase(m_display_diagnostics_phase);
    record_display_diagnostic_state("shutdown_baked");
}

void GLGizmoTracery::on_load(cereal::BinaryInputArchive& ar)
{
    record_display_diagnostic_state("load_begin");
    GLGizmoPainterBase::on_load(ar);
    ar(m_mask_object_id, m_mask_data, m_stained_glass_base_paint_data, m_fragment_density, m_refraction_strength);
    m_generated_fragments.clear();
    record_display_diagnostic_state("load_end");
}

void GLGizmoTracery::on_save(cereal::BinaryOutputArchive& ar) const
{
    ar(m_mask_object_id, m_mask_data, m_stained_glass_base_paint_data, m_fragment_density, m_refraction_strength);
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
