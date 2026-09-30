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

wxColour lighter_tracery_recipe_target(const wxColour& color)
{
    // Moving each channel toward white preserves the target hue while making
    // the printable glass recipe lighter and less saturated. This is not a
    // display-only adjustment: the matcher receives this target colour.
    constexpr double white_bias = 0.18;
    const auto lighten = [white_bias](unsigned char channel) {
        return int(std::lround(double(channel) * (1.0 - white_bias) + 255.0 * white_bias));
    };
    return wxColour(lighten(color.Red()), lighten(color.Green()), lighten(color.Blue()));
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
    const float shifted_hue = hue + (tracery_noise(fragment_id + 43) - 0.5f) * 0.16f;
    const float shifted_saturation = std::clamp(saturation + (tracery_noise(fragment_id + 67) - 0.5f) * 0.26f, 0.0f, 1.0f);
    const float shifted_light = std::clamp(light + (tracery_noise(fragment_id + 89) - 0.5f) * 0.32f, 0.05f, 0.95f);
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
            // White keeps the physical glass brighter; mixing all three CMY
            // ingredients at once tends to produce a muddy complementary mix.
            constexpr int minimum_white_percent = 15;
            const int chromatic_count = int(weights[0] > 0) + int(weights[1] > 0) + int(weights[2] > 0);
            if (weights[3] < minimum_white_percent || chromatic_count > 2)
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
    // The through mode is an interface placeholder until its projection rules
    // are defined. Do not silently apply ordinary surface painting in it.
    if (m_paint_mode == PaintMode::THROUGH)
        return true;
    return GLGizmoPainterBase::gizmo_event(action, mouse_position, shift_down, alt_down, control_down);
}

std::string GLGizmoTracery::on_get_name() const
{
    return _u8L("Stained glass");
}

void GLGizmoTracery::render_painter_gizmo()
{
    // The canvas renders the stained-glass replacement before the ordinary
    // model pass. Only the editing aids belong in this later painter pass.
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));
    m_c->object_clipper()->render_cut();
    m_c->instances_hider()->render_cut();
    if (m_paint_mode == PaintMode::SURFACE)
        render_cursor();
    glsafe(::glDisable(GL_BLEND));
}

bool GLGizmoTracery::has_colored_overlay() const
{
    return std::any_of(m_overlay_volumes.cbegin(), m_overlay_volumes.cend(), [](const OverlayVolume& volume) {
        return !volume.fragments.empty();
    });
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
               << "events.tsv records model/overlay identity, fragment counts and rendering decisions.\n"
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
    size_t masks = 0;
    for (const OverlayVolume& volume : m_overlay_volumes) {
        fragments += volume.fragments.size();
        masks += volume.mask_model != nullptr ? 1 : 0;
    }
    size_t saved_patches = 0;
    std::string object_ids;
    if (model != nullptr) {
        for (size_t index = 0; index < model->objects.size(); ++index) {
            const ModelObject* object = model->objects[index];
            if (!object_ids.empty()) object_ids += ',';
            object_ids += std::to_string(object->id().id);
            if (int(index) == m_mask_object_index)
                for (const ModelVolume* volume : object->volumes)
                    saved_patches += volume->tracery_facets.patches.size();
        }
    }
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    out << milliseconds << '\t' << m_display_diagnostics_phase << '\t' << event
        << "\tdetail=" << detail
        << "\tediting=" << (m_state == On)
        << "\tmask_id=" << m_mask_object_id.id
        << "\tmask_index=" << m_mask_object_index
        << "\toverlay_id=" << m_overlay_object_id.id
        << "\tobject_ids=" << object_ids
        << "\tlookup=" << overlay_object_index()
        << "\tmask_entries=" << m_mask_data.size()
        << "\tlive_selectors=" << m_triangle_selectors.size()
        << "\toverlay_volumes=" << m_overlay_volumes.size()
        << "\tfragments=" << fragments
        << "\tmask_models=" << masks
        << "\tsaved_patches=" << saved_patches
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

int GLGizmoTracery::overlay_object_index() const
{
    if (!has_colored_overlay())
        return -1;

    const Model* model = m_parent.get_model();
    if (model == nullptr)
        return -1;
    for (size_t index = 0; index < model->objects.size(); ++index)
        if (model->objects[index]->id() == m_overlay_object_id)
            return int(index);
    return -1;
}

void GLGizmoTracery::render_colored_overlay()
{
    render_overlay_geometry(false);
}

void GLGizmoTracery::render_surface_mask()
{
    render_overlay_geometry(true);
}

void GLGizmoTracery::render_overlay_geometry(bool mask_only)
{
    const int object_idx = overlay_object_index();
    if (object_idx < 0)
        return;
    const ModelObject* model_object = m_parent.get_model()->objects[size_t(object_idx)];

    auto* shader = wxGetApp().get_shader("gouraud");
    if (shader == nullptr)
        return;

    const GLboolean blend_enabled = ::glIsEnabled(GL_BLEND);
    const GLboolean depth_test_enabled = ::glIsEnabled(GL_DEPTH_TEST);
    const GLboolean cull_face_enabled = ::glIsEnabled(GL_CULL_FACE);
    const GLboolean polygon_offset_enabled = ::glIsEnabled(GL_POLYGON_OFFSET_FILL);
    GLboolean depth_write_enabled = GL_TRUE;
    GLint depth_function = GL_LESS;
    GLfloat polygon_offset_factor = 0.0f;
    GLfloat polygon_offset_units = 0.0f;
    glsafe(::glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_write_enabled));
    glsafe(::glGetIntegerv(GL_DEPTH_FUNC, &depth_function));
    glsafe(::glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &polygon_offset_factor));
    glsafe(::glGetFloatv(GL_POLYGON_OFFSET_UNITS, &polygon_offset_units));

    shader->start_using();
    ScopeGuard restore_overlay_state([=]() {
        shader->stop_using();
        glsafe(::glDepthMask(depth_write_enabled));
        glsafe(::glDepthFunc(static_cast<GLenum>(depth_function)));
        glsafe(::glPolygonOffset(polygon_offset_factor, polygon_offset_units));
        glsafe(blend_enabled == GL_TRUE ? ::glEnable(GL_BLEND) : ::glDisable(GL_BLEND));
        glsafe(depth_test_enabled == GL_TRUE ? ::glEnable(GL_DEPTH_TEST) : ::glDisable(GL_DEPTH_TEST));
        glsafe(cull_face_enabled == GL_TRUE ? ::glEnable(GL_CULL_FACE) : ::glDisable(GL_CULL_FACE));
        glsafe(polygon_offset_enabled == GL_TRUE ? ::glEnable(GL_POLYGON_OFFSET_FILL) : ::glDisable(GL_POLYGON_OFFSET_FILL));
    });
    const Camera& camera = wxGetApp().plater()->get_camera();
    // The mask is made from the painted selection, not from the tessellated
    // shards. It therefore remains stable when mixed-filament colours refresh
    // the original model's independent MMU display mesh.
    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDepthFunc(GL_LEQUAL));
    glsafe(::glDepthMask(mask_only ? GL_FALSE : GL_TRUE));
    glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
    const float depth_bias = mask_only ? -4.0f : (m_diagnostic_strong_depth_bias ? -16.0f : -8.0f);
    glsafe(::glPolygonOffset(depth_bias, depth_bias));
    glsafe(::glDisable(GL_CULL_FACE));

    const GLVolumeCollection& displayed_volumes = m_parent.get_volumes();
    shader->set_uniform("z_range", displayed_volumes.get_z_range());
    shader->set_uniform("clipping_plane", displayed_volumes.get_clipping_plane());
    shader->set_uniform("use_color_clip_plane", false);
    shader->set_uniform("slope.actived", false);
    shader->set_uniform("print_volume.type", -1);
#if ENABLE_ENVIRONMENT_MAP
    shader->set_uniform("use_environment_tex", false);
#endif

    // The object model is updated after a drag finishes, but the canvas
    // volume carries the live transformation while a move/rotate/scale is in
    // progress. Map every model part to the overlay mesh created for it, then
    // render through that live GLVolume transformation.
    std::vector<int> overlay_mesh_for_volume(model_object->volumes.size(), -1);
    for (size_t volume_idx = 0, mesh_idx = 0; volume_idx < model_object->volumes.size(); ++volume_idx) {
        if (!model_object->volumes[volume_idx]->is_model_part())
            continue;
        if (mesh_idx >= m_overlay_volumes.size())
            break;
        overlay_mesh_for_volume[volume_idx] = int(mesh_idx++);
    }

    for (const GLVolume* display_volume : m_parent.get_volumes().volumes) {
        if (!display_volume->is_active || display_volume->composite_id.object_id != object_idx)
            continue;
        const int volume_idx = display_volume->composite_id.volume_id;
        if (volume_idx < 0 || size_t(volume_idx) >= overlay_mesh_for_volume.size())
            continue;
        const int overlay_mesh_idx = overlay_mesh_for_volume[size_t(volume_idx)];
        if (overlay_mesh_idx < 0)
            continue;

        const Transform3d trafo_matrix = display_volume->world_matrix();
        shader->set_uniform("view_model_matrix", camera.get_view_matrix() * trafo_matrix);
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        shader->set_uniform("volume_world_matrix", trafo_matrix);
        const Matrix3d view_normal_matrix = camera.get_view_matrix().matrix().block(0, 0, 3, 3) *
            trafo_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
        shader->set_uniform("view_normal_matrix", view_normal_matrix);

        const OverlayVolume& overlay_volume = m_overlay_volumes[size_t(overlay_mesh_idx)];
        if (mask_only) {
            if (overlay_volume.mask_model != nullptr)
                overlay_volume.mask_model->render();
        } else {
            for (const OverlayFragment& fragment : overlay_volume.fragments)
                fragment.model->render();
        }
    }

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
        _L("Normal: paint the visible surface"), _L("Through: painting will be available later")
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
        rebuild_colored_overlay();
        m_parent.set_as_dirty();
    }

    ImGui::Separator();
    if (m_imgui->button(_L("Regenerate fragments"))) {
        m_regeneration_requested = true;
        ++m_regeneration_version;
        rebuild_colored_overlay();
        m_parent.set_as_dirty();
    }
    if (!m_fragment_colors.empty() && m_imgui->button(_L("Reapply glass colours on exit")))
        m_palette_update_requested = true;
    if (m_palette_update_requested)
        m_imgui->text(_L("Glass colours will update when editing closes."));
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
    m_mask_data.clear();
    m_mask_data.reserve(m_triangle_selectors.size());
    for (const std::unique_ptr<TriangleSelectorGUI>& selector : m_triangle_selectors)
        m_mask_data.emplace_back(selector->serialize());
    rebuild_colored_overlay();
}

void GLGizmoTracery::rebuild_colored_overlay()
{
    record_display_diagnostic_state("rebuild_begin");
    ScopeGuard log_rebuild_end([this]() { record_display_diagnostic_state("rebuild_end"); });
    const bool export_diagnostics = std::exchange(m_export_next_generation, false);
    m_overlay_volumes.clear();
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

    m_overlay_object_id = model_object->id();
    m_overlay_volumes.resize(m_triangle_selectors.size());

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
        if (!facets.indices.empty()) {
            auto mask = std::make_unique<GLModel>();
            mask->init_from(facets);
            m_overlay_volumes[mesh_idx].mask_model = std::move(mask);
        }
        const Transform3d& volume_transform = volume->get_matrix();
        volume_transforms.emplace_back(volume_transform);
        const ColorRGBA source_color = volume_source_color(int(volume_idx));

        // Multi-material painting stores its colours per surface facet, not
        // per ModelVolume. Mirror the same facet sets used by the normal
        // model renderer and use an AABB lookup below to sample the actual
        // source colour at each selected surface position.
        if (!volume->mmu_segmentation_facets.empty()) {
            std::vector<indexed_triangle_set> facets_per_color;
            volume->mmu_segmentation_facets.get_facets(*volume, facets_per_color);
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

    std::vector<std::vector<GLModel::Geometry>> geometry(m_triangle_selectors.size(), std::vector<GLModel::Geometry>(fragment_count));
    for (std::vector<GLModel::Geometry>& per_volume : geometry)
        for (GLModel::Geometry& data : per_volume)
            data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3 };

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
            GLModel::Geometry& data = geometry[triangle.mesh_idx][candidate];
            const unsigned int first = unsigned(data.vertices_count());
            for (const ClipVertex& vertex : polygon)
                data.add_vertex(vertex.local, triangle.normal);
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
    m_fragment_colors.resize(fragment_count);
    m_overlay_volumes.resize(m_triangle_selectors.size());
    for (size_t fragment = 0; fragment < fragment_count; ++fragment) {
        ColorRGBA boundary_color = base_colors[fragment];
        const size_t neighbor = fragment_boundary_neighbor[fragment];
        if (neighbor != no_group)
            boundary_color = color_groups[neighbor].color;
        // The distance band is roughly one to two shard widths. Smoothstep
        // makes the change gradual without affecting either deep interior.
        const float boundary_band = target_edge * 1.7f;
        const float linear_proximity = fragment_boundary_distance[fragment] < boundary_band ?
            1.0f - fragment_boundary_distance[fragment] / boundary_band : 0.0f;
        const float proximity = linear_proximity * linear_proximity * (3.0f - 2.0f * linear_proximity);
        const float boundary_mix = proximity * (0.16f + 0.42f * m_refraction_strength);
        const ColorRGBA color = glass_color(base_colors[fragment], boundary_color, boundary_mix, seeds[fragment].stable_id);
        m_fragment_colors[fragment] = color;
        for (std::vector<GLModel::Geometry>& per_volume : geometry)
            per_volume[fragment].color = color;
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
                for (const GLModel::Geometry& data : per_volume)
                    clipped_faces += data.indices_count() / 3;
            write_tracery_diagnostic_ply(diagnostic_dir / "02_clipped_geometry.ply", clipped_faces,
                [&](const auto& emit) {
                    for (size_t volume = 0; volume < geometry.size(); ++volume)
                        for (const GLModel::Geometry& data : geometry[volume])
                            for (size_t index = 0; index + 2 < data.indices_count(); index += 3)
                                for (size_t corner = 0; corner < 3; ++corner) {
                                    const unsigned int vertex = data.indices[index + corner];
                                    Vec3f local = Vec3f::Zero();
                                    if (vertex < data.vertices_count())
                                        local = data.extract_position_3(vertex);
                                    emit((volume_transforms[volume] * local.cast<double>()).cast<float>(), data.color);
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
            GLModel::Geometry& data = geometry[volume][fragment];
            if (data.is_empty())
                continue;
            indexed_triangle_set surface = data.get_as_indexed_triangle_set();
            if (diagnostic_ready) {
                size_t invalid_indices = 0;
                size_t tiny_faces = 0;
                bool positions_equal = surface.vertices.size() == data.vertices_count();
                bool indices_equal = surface.indices.size() == data.indices_count() / 3;
                for (size_t vertex = 0; positions_equal && vertex < surface.vertices.size(); ++vertex)
                    positions_equal = (surface.vertices[vertex] - data.extract_position_3(vertex)).squaredNorm() == 0.0f;
                for (size_t face = 0; face < surface.indices.size(); ++face) {
                    const size_t first = face * 3;
                    for (size_t corner = 0; corner < 3; ++corner) {
                        const unsigned int index = data.indices[first + corner];
                        if (index >= data.vertices_count())
                            ++invalid_indices;
                        if (surface.indices[face][corner] != index)
                            indices_equal = false;
                    }
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
                diagnostic_stats << volume << ',' << fragment << ',' << data.vertices_count() << ',' << data.indices_count() / 3
                                 << ',' << surface.vertices.size() << ',' << surface.indices.size() << ',' << invalid_indices
                                 << ',' << tiny_faces << ',' << positions_equal << ',' << indices_equal << '\n';
            }
            auto overlay = std::make_unique<GLModel>();
            overlay->init_from(std::move(data));
            m_overlay_volumes[volume].fragments.emplace_back(OverlayFragment{ fragment, std::move(overlay), std::move(surface) });
        }
    }

    if (diagnostic_ready) {
        try {
            size_t exported_faces = 0;
            for (const OverlayVolume& volume : m_overlay_volumes)
                for (const OverlayFragment& fragment : volume.fragments)
                    exported_faces += fragment.surface.indices.size();
            write_tracery_diagnostic_ply(diagnostic_dir / "03_exported_surface.ply", exported_faces,
                [&](const auto& emit) {
                    for (size_t volume = 0; volume < m_overlay_volumes.size(); ++volume)
                        for (const OverlayFragment& fragment : m_overlay_volumes[volume].fragments)
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
    if (m_fragment_colors.empty()) {
        wxMessageBox(_L("Paint an area and generate stained-glass fragments first."), _L("Stained glass"), wxOK | wxICON_INFORMATION);
        return;
    }
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return;
    auto* colour_option = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
    if (colour_option == nullptr)
        return;
    // Only colours explicitly sprayed onto model facets need original-colour
    // recipes. A volume's ordinary base filament remains unchanged.
    const auto painted_source_ids = [] {
        std::set<unsigned int> ids;
        for (const ModelObject *object : wxGetApp().plater()->model().objects)
            for (const ModelVolume *volume : object->volumes) {
                const auto &states = volume->mmu_segmentation_facets.get_data().used_states;
                for (size_t id = 1; id < states.size(); ++id)
                    if (states[id]) ids.insert(unsigned(id));
            }
        return ids;
    };

    auto& mixed_manager = bundle->mixed_filaments;
    auto& mixed_rows = mixed_manager.mixed_filaments();
    const std::vector<MixedFilament> old_mixed_rows = mixed_rows;
    const std::vector<uint64_t> old_stained_ids = m_stained_mixed_stable_ids;
    std::set<uint64_t> retired_ids(m_stained_mixed_stable_ids.begin(), m_stained_mixed_stable_ids.end());
    std::set<uint64_t> protected_ids;
    // Recover previous effect rows after reopening a project, but never
    // retire a recipe still referenced by a different model object.
    const Model &scene = wxGetApp().plater()->model();
    size_t current_object_index = scene.objects.size();
    for (size_t index = 0; index < scene.objects.size(); ++index)
        if (scene.objects[index]->id() == m_mask_object_id) {
            current_object_index = index;
            break;
        }
    if (current_object_index == scene.objects.size() && m_mask_object_index >= 0 && size_t(m_mask_object_index) < scene.objects.size())
        current_object_index = size_t(m_mask_object_index);
    for (size_t object_index = 0; object_index < scene.objects.size(); ++object_index) {
        const ModelObject *object = scene.objects[object_index];
        const bool current = object_index == current_object_index;
        for (const ModelVolume *volume : object->volumes)
            for (const TraceryPatch &patch : volume->tracery_facets.patches)
                if (const MixedFilament *row = mixed_manager.mixed_filament_from_id(patch.filament_id, colour_option->values.size()))
                    (current ? retired_ids : protected_ids).insert(row->stable_id);
    }
    if (const auto *routes = bundle->project_config.option<ConfigOptionString>("stained_glass_color_routes")) {
        size_t begin = 0;
        while (begin < routes->value.size()) {
            const size_t end = routes->value.find(';', begin);
            const std::string pair = routes->value.substr(begin, end == std::string::npos ? end : end - begin);
            if (const size_t colon = pair.find(':'); colon != std::string::npos) {
                try { retired_ids.insert(std::stoull(pair.substr(colon + 1))); }
                catch (const std::exception &) {}
            }
            if (end == std::string::npos) break;
            begin = end + 1;
        }
    }
    for (MixedFilament& row : mixed_rows) {
        if (retired_ids.count(row.stable_id) && !protected_ids.count(row.stable_id)) {
            row.deleted = true;
            row.enabled = false;
        }
    }
    m_stained_mixed_stable_ids.clear();

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
        projected_manager.total_filaments(projected_colours.size()) + painted_source_ids().size() + 4 > MAXIMUM_FILAMENT_NUMBER) {
        mixed_rows = old_mixed_rows;
        m_stained_mixed_stable_ids = old_stained_ids;
        wxMessageBox(_L("There are not enough slots for the original colours, CMYW ingredients and stained-glass recipes."),
            _L("Stained glass"), wxOK | wxICON_WARNING);
        return;
    }

    // Retiring old recipes changes virtual IDs. Remap the model's existing
    // painting (including other objects' glass patches) before adding CMYW.
    bundle->update_mixed_filament_id_remap(old_mixed_rows, colour_option->values.size(), colour_option->values.size());
    wxGetApp().plater()->on_filaments_change(colour_option->values.size());
    record_display_diagnostic_state("after_filaments_change");
    colour_option = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
    if (colour_option == nullptr)
        return;

    // Add all missing base colours in one Sidebar update. Adding a physical
    // filament rebuilds the color option, so no pointer into the old option is
    // used after this call.
    wxGetApp().sidebar().add_custom_filaments(missing_base_colours);
    record_display_diagnostic_state("after_add_custom_filaments");
    colour_option = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
    if (colour_option == nullptr)
        return;
    std::array<unsigned int, 4> cmyw_ids = {};
    std::vector<std::string> cmyw_colours;
    cmyw_colours.reserve(cmyw.size());
    for (size_t index = 0; index < cmyw.size(); ++index) {
        const int physical_index = find_physical_colour(colour_option->values, cmyw[index]);
        if (physical_index < 0)
            return;
        cmyw_ids[index] = unsigned(physical_index + 1);
        cmyw_colours.emplace_back(colour_option->values[size_t(physical_index)]);
    }
    // The actual CMYW recipe slots may be a mix of existing and newly added
    // filaments. On U1, make all four use Full Spectrum after their IDs are known.
    wxGetApp().sidebar().set_stained_glass_base_filament_preset(
        std::vector<unsigned int>(cmyw_ids.begin(), cmyw_ids.end()));

    const std::set<unsigned int> used_source_ids = painted_source_ids();
    std::vector<unsigned int> physical_sources;
    struct VirtualSource { uint64_t stable_id; wxColour colour; };
    std::vector<VirtualSource> virtual_sources;
    for (unsigned int id : used_source_ids) {
        if (id <= colour_option->values.size()) {
            physical_sources.emplace_back(id);
            continue;
        }
        if (const MixedFilament *row = mixed_manager.mixed_filament_from_id(id, colour_option->values.size());
            row != nullptr && row->stable_id != 0) {
            const wxColour colour(wxString::FromUTF8(row->display_color));
            virtual_sources.push_back({ row->stable_id, colour.IsOk() ? colour : wxColour("#808080") });
        }
    }

    const size_t available_mixed = MAXIMUM_FILAMENT_NUMBER - mixed_manager.total_filaments(colour_option->values.size());
    const size_t source_budget = physical_sources.size() + virtual_sources.size();
    const size_t palette_budget = std::min<size_t>(24, available_mixed > source_budget ? available_mixed - source_budget : 0);
    if (palette_budget < 4) {
        wxMessageBox(_L("Not enough mixed-filament slots for the painted model colours and stained-glass colours."),
            _L("Stained glass"), wxOK | wxICON_WARNING);
        return;
    }

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
    std::vector<wxColour> target_colours = palette_preview_colours;
    for (unsigned int source : physical_sources) {
        const wxColour colour(wxString::FromUTF8(colour_option->values[source - 1]));
        target_colours.emplace_back(colour.IsOk() ? colour : wxColour("#808080"));
    }
    for (const VirtualSource &source : virtual_sources)
        target_colours.emplace_back(source.colour);
    std::vector<MixedFilamentBatchEntry> new_rows;
    std::vector<size_t> target_row_indices(target_colours.size(), size_t(-1));
    std::map<std::string, size_t> recipe_rows;
    for (size_t index = 0; index < target_colours.size(); ++index) {
        // Generic matching intentionally searches only pairs and triples to
        // keep an arbitrary filament palette responsive. Stained glass always
        // owns CMYW, so use its bounded four-channel matcher instead.
        const bool glass_recipe = index < candidates.size();
        const wxColour target = glass_recipe ? lighter_tracery_recipe_target(target_colours[index]) : target_colours[index];
        MixedColorMatchRecipeResult recipe = build_tracery_cmyw_recipe(cmyw_colours, target, glass_recipe);
        if (!recipe.valid)
            continue;
        if (index < candidates.size())
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
        const auto existing = recipe_rows.find(fingerprint);
        if (existing != recipe_rows.end()) {
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

    if (new_rows.size() > available_mixed ||
        std::any_of(target_row_indices.begin(), target_row_indices.end(), [](size_t row) { return row == size_t(-1); })) {
        wxMessageBox(_L("Unable to make CMYW recipes for every model and stained-glass colour."),
            _L("Stained glass"), wxOK | wxICON_WARNING);
        return;
    }
    std::vector<unsigned int> assigned_ids;
    mixed_manager.add_batch_custom_filaments(new_rows, colour_option->values, &assigned_ids);
    for (size_t index = 0; index < candidates.size(); ++index) {
        const size_t row = target_row_indices[index];
        if (row != size_t(-1) && row < assigned_ids.size())
            palette_filament_ids[index] = assigned_ids[row];
    }
    std::string source_routes;
    for (size_t source = 0; source < physical_sources.size(); ++source) {
        const size_t row_index = target_row_indices[candidates.size() + source];
        const unsigned int virtual_id = assigned_ids[row_index];
        const MixedFilament* row = mixed_manager.mixed_filament_from_id(virtual_id, colour_option->values.size());
        if (row == nullptr || row->stable_id == 0) {
            wxMessageBox(_L("Unable to assign a CMYW recipe to every painted model colour."),
                _L("Stained glass"), wxOK | wxICON_WARNING);
            return;
        }
        if (!source_routes.empty()) source_routes += ';';
        source_routes += std::to_string(physical_sources[source]) + ':' + std::to_string(row->stable_id);
    }
    for (size_t source = 0; source < virtual_sources.size(); ++source) {
        const size_t row_index = target_row_indices[candidates.size() + physical_sources.size() + source];
        const MixedFilament *row = mixed_manager.mixed_filament_from_id(assigned_ids[row_index], colour_option->values.size());
        if (row == nullptr || row->stable_id == 0)
            return;
        source_routes += ";v" + std::to_string(virtual_sources[source].stable_id) + ':' + std::to_string(row->stable_id);
    }
    m_fragment_mixed_filament_ids.resize(fragment_palette.size(), 0);
    for (size_t fragment = 0; fragment < fragment_palette.size(); ++fragment) {
        m_fragment_mixed_filament_ids[fragment] = palette_filament_ids[fragment_palette[fragment]];
        const wxColour& preview = palette_preview_colours[fragment_palette[fragment]];
        const ColorRGBA preview_colour(float(preview.Red()) / 255.0f, float(preview.Green()) / 255.0f,
            float(preview.Blue()) / 255.0f, 1.0f);
        for (OverlayVolume& volume : m_overlay_volumes)
            for (OverlayFragment& overlay : volume.fragments)
                if (overlay.fragment_id == fragment)
                    overlay.model->set_color(preview_colour);
    }

    for (unsigned int filament_id : assigned_ids) {
        if (const MixedFilament* row = mixed_manager.mixed_filament_from_id(filament_id, colour_option->values.size()))
            m_stained_mixed_stable_ids.emplace_back(row->stable_id);
    }
    if (auto* definitions = bundle->project_config.option<ConfigOptionString>("mixed_filament_definitions"))
        definitions->value = mixed_manager.serialize_custom_entries();
    if (auto* routes = bundle->project_config.option<ConfigOptionString>("stained_glass_color_routes"))
        routes->value = std::move(source_routes);
    publish_tracery_overlay();
    wxGetApp().sidebar().update_color_mix_panel();
    wxGetApp().plater()->post_slice_state_change_update();
    m_parent.set_as_dirty();
}

void GLGizmoTracery::publish_tracery_overlay()
{
    Model &model = wxGetApp().plater()->model();
    ModelObject *object = nullptr;
    for (ModelObject *candidate : model.objects)
        if (candidate->id() == m_mask_object_id) { object = candidate; break; }
    if (object == nullptr && m_mask_object_index >= 0 && size_t(m_mask_object_index) < model.objects.size())
        object = model.objects[size_t(m_mask_object_index)];
    if (object == nullptr)
        return;

    size_t volume_index = 0;
    for (ModelVolume *volume : object->volumes) {
        if (!volume->is_model_part())
            continue;
        std::map<unsigned int, indexed_triangle_set> by_filament;
        if (volume_index < m_overlay_volumes.size())
            for (const OverlayFragment &fragment : m_overlay_volumes[volume_index].fragments)
                if (fragment.fragment_id < m_fragment_mixed_filament_ids.size()) {
                    const unsigned int filament = m_fragment_mixed_filament_ids[fragment.fragment_id];
                    if (filament > 0)
                        its_merge(by_filament[filament], fragment.surface);
                }
        std::vector<TraceryPatch> patches;
        patches.reserve(by_filament.size());
        for (auto &[filament, surface] : by_filament)
            patches.push_back({filament, std::move(surface)});
        volume->tracery_facets.set(std::move(patches));
        ++volume_index;
    }
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
        m_overlay_volumes.clear();
        m_overlay_object_id = ObjectID();
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
    // Leaving the editor clears the paint selectors, not the finished glass
    // geometry. Reopening the same unchanged object only restores the tools;
    // it must not generate a different overlay for the same saved mask.
    const bool can_reuse_overlay = restore_mask && model_object->id() == m_overlay_object_id &&
        m_overlay_volumes.size() == m_triangle_selectors.size() && has_colored_overlay();
    if (restore_mask && !m_mask_data.empty() && !can_reuse_overlay)
        rebuild_colored_overlay();
    record_display_diagnostic_state("editor_reopen_end",
        std::string("restore_mask=") + (restore_mask ? "1" : "0") +
        ",can_reuse_overlay=" + (can_reuse_overlay ? "1" : "0"));
    m_display_diagnostics_phase = "reentered";
    m_display_diagnostics_last_canvas_state.clear();
    set_display_canvas_probe_phase(m_display_diagnostics_phase);
}

void GLGizmoTracery::on_shutdown()
{
    record_display_diagnostic_state("shutdown_begin");
    // Reopening the painter may restore a display-only overlay, but it does
    // not change the saved glass or its mixed-filament recipes. In particular,
    // do not retire and recreate those recipes on an unchanged editor exit.
    const bool content_modified = m_selection_modified || m_regeneration_requested ||
        m_fragment_density != m_opening_fragment_density ||
        m_refraction_strength != m_opening_refraction_strength;
    bool missing_published_glass = false;
    if (!content_modified && !m_fragment_colors.empty()) {
        const Model &model = wxGetApp().plater()->model();
        const ModelObject *object = nullptr;
        for (const ModelObject *candidate : model.objects)
            if (candidate->id() == m_mask_object_id) { object = candidate; break; }
        if (object == nullptr && m_mask_object_index >= 0 && size_t(m_mask_object_index) < model.objects.size())
            object = model.objects[size_t(m_mask_object_index)];
        missing_published_glass = object != nullptr &&
            std::none_of(object->volumes.begin(), object->volumes.end(), [](const ModelVolume *volume) {
                return !volume->tracery_facets.empty();
            });
    }
    if (!content_modified && !missing_published_glass && !m_palette_update_requested) {
        record_display_diagnostic_state("shutdown_unchanged");
        m_display_diagnostics_phase = "after_exit";
        m_display_diagnostics_last_canvas_state.clear();
        set_display_canvas_probe_phase(m_display_diagnostics_phase);
        return;
    }
    if (!content_modified && !missing_published_glass && m_palette_update_requested) {
        record_display_diagnostic_state("shutdown_reapply_palette");
        m_display_diagnostics_phase = "after_exit";
        m_display_diagnostics_last_canvas_state.clear();
        set_display_canvas_probe_phase(m_display_diagnostics_phase);
        wxGetApp().CallAfter([this] { apply_stained_glass_colors(); });
        return;
    }
    // Capture before GLGizmoPainterBase clears the live selectors. The colored
    // overlay remains available to the ordinary model renderer after closing.
    capture_mask_and_generate();
    record_display_diagnostic_state("shutdown_after_capture");
    m_display_diagnostics_phase = "after_exit";
    m_display_diagnostics_last_canvas_state.clear();
    set_display_canvas_probe_phase(m_display_diagnostics_phase);

    // Closing the toolbar, switching gizmos and clicking the explicit button
    // all mean that editing is finished.  Existing fragments must therefore
    // take the same deferred material-update path in every case.
    if (m_fragment_colors.empty()) {
        if (!m_selection_modified)
            return;
        publish_tracery_overlay();
        const Model &model = wxGetApp().plater()->model();
        const bool any_glass = std::any_of(model.objects.begin(), model.objects.end(), [](const ModelObject *object) {
            return std::any_of(object->volumes.begin(), object->volumes.end(), [](const ModelVolume *volume) {
                return !volume->tracery_facets.empty();
            });
        });
        if (!any_glass)
            if (auto *routes = wxGetApp().preset_bundle->project_config.option<ConfigOptionString>("stained_glass_color_routes"))
                routes->value.clear();
        wxGetApp().plater()->post_slice_state_change_update();
        return;
    }

    // Filament and mixed-colour changes may rebuild the model's display
    // objects. Running them only after this painter has exited prevents that
    // refresh from clearing the active painted selection.
    wxGetApp().CallAfter([this] { apply_stained_glass_colors(); });
}

void GLGizmoTracery::on_load(cereal::BinaryInputArchive& ar)
{
    record_display_diagnostic_state("load_begin");
    GLGizmoPainterBase::on_load(ar);
    ar(m_mask_object_id, m_mask_data, m_fragment_density, m_refraction_strength);
    m_overlay_volumes.clear();
    m_overlay_object_id = ObjectID();
    record_display_diagnostic_state("load_end");
}

void GLGizmoTracery::on_save(cereal::BinaryOutputArchive& ar) const
{
    ar(m_mask_object_id, m_mask_data, m_fragment_density, m_refraction_strength);
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
