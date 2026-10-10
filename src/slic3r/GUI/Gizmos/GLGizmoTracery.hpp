#ifndef slic3r_GLGizmoTracery_hpp_
#define slic3r_GLGizmoTracery_hpp_

#include "GLGizmoPainterBase.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "slic3r/GUI/I18N.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Slic3r::GUI {

// Stained glass is an automatic multi-material paint brush. Its temporary
// fragments are baked into the regular per-facet painting before rendering or
// slicing, never published as a separate model layer.
class GLGizmoTracery final : public GLGizmoPainterBase
{
public:
    GLGizmoTracery(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);

    bool gizmo_event(SLAGizmoEventType action, const Vec2d& mouse_position, bool shift_down, bool alt_down, bool control_down) override;
    void render_painter_gizmo() override;
    void record_display_diagnostic_canvas(int object_index, bool picking_enabled, bool stencil_available,
                                          int matching_volumes, bool editing);
    void capture_display_diagnostic_frame(const char* stage, bool stencil_buffer);
    // Process-wide probe: ordinary view may render through a different canvas
    // than the one that owns this editor instance.
    static bool global_display_diagnostics_active();
    static void record_global_display_diagnostic_canvas(const void* canvas, const std::string& detail);
    static void capture_global_display_diagnostic_frame(const void* canvas, const char* stage);
    std::string get_tooltip() const override { return _u8L("Stained glass"); }

protected:
    // The stained-glass mask is input to the CPU baker only. Unlike ordinary
    // paint gizmos it must never be drawn over the committed MMU result.
    void        render_triangles(const Selection&) const override {}
    void        on_render_input_window(float x, float y, float bottom_limit) override;
    std::string on_get_name() const override;
    wxString    handle_snapshot_action_name(bool shift_down, Button button_down) const override;

    std::string get_gizmo_entering_text() const override { return _u8L("Entering Stained glass"); }
    std::string get_gizmo_leaving_text() const override { return _u8L("Leaving Stained glass"); }
    std::string get_action_snapshot_name() const override { return _u8L("Stained glass editing"); }

    EnforcerBlockerType get_left_button_state_type() const override { return m_eraser_mode ? EnforcerBlockerType::NONE : EnforcerBlockerType::ENFORCER; }
    EnforcerBlockerType get_right_button_state_type() const override { return EnforcerBlockerType::NONE; }
    ColorRGBA get_cursor_sphere_left_button_color() const override { return { 0.0f, 0.59f, 0.53f, 0.25f }; }
    ColorRGBA get_cursor_sphere_right_button_color() const override { return { 0.0f, 0.59f, 0.53f, 0.15f }; }

private:
    bool on_init() override;
    void update_model_object() override;
    void update_from_model_object(bool first_update) override;
    void on_opening() override
    {
        m_selection_modified = false;
        m_regeneration_requested = false;
        m_palette_update_requested = false;
        m_opening_fragment_density = m_fragment_density;
        m_opening_refraction_strength = m_refraction_strength;
        m_paint_mode = PaintMode::SURFACE;
    }
    void on_shutdown() override;
    void on_load(cereal::BinaryInputArchive& ar) override;
    void on_save(cereal::BinaryOutputArchive& ar) const override;
    PainterGizmoType get_painter_type() const override;

    void capture_mask_and_generate();
    void generate_paint_plan();
    struct GenerationInput;
    bool collect_generation_input(GenerationInput& input);
    struct WorkTriangle;
    void build_work_surface(const GenerationInput& input, float boundary_sample_edge,
                            std::vector<WorkTriangle>& work_triangles) const;
    void apply_stained_glass_colors();
    struct StainedGlassApplyPlan {
        enum class Mode {
            RestoreBasePaint,
            BakeFragments
        };

        Mode                      mode = Mode::RestoreBasePaint;
        std::vector<unsigned int> fragment_mixed_filament_ids;
        std::string               mixed_filament_definitions;
    };
    std::optional<StainedGlassApplyPlan> prepare_stained_glass_apply();
    void                                 generate_through_fragments();
    void                                 commit_stained_glass_apply(StainedGlassApplyPlan&& plan);
    void bake_fragments_to_mmu_painting();
    void start_display_diagnostics();
    void stop_display_diagnostics();
    void record_display_diagnostic_state(const char* event, const std::string& detail = {});

    // CPU-only geometry used to classify surface points while baking the
    // generated colours into normal MMU paint. It is never rendered.
    struct GeneratedFragment {
        size_t                   fragment_id = 0;
        indexed_triangle_set     surface;
    };

    enum class PaintMode { SURFACE, THROUGH };
    PaintMode m_paint_mode = PaintMode::SURFACE;
    wchar_t m_current_tool = 0;
    bool    m_eraser_mode = false;
    bool    m_selection_modified = false;
    bool    m_regeneration_requested = false;
    bool    m_palette_update_requested = false;
    bool    m_export_next_generation = false;
    bool    m_diagnostic_strong_depth_bias = false;
    bool    m_display_diagnostics_active = false;
    std::filesystem::path m_display_diagnostics_dir;
    std::string m_display_diagnostics_phase;
    std::string m_display_diagnostics_last_canvas_state;
    std::set<std::string> m_display_diagnostics_captured;
    float   m_fragment_density = 0.5f;
    float   m_refraction_strength = 0.5f;
    float   m_opening_fragment_density = 0.5f;
    float   m_opening_refraction_strength = 0.5f;
    size_t  m_regeneration_version = 0;
    // Keep the editable selection independently from the model. The base
    // painter clears its live selectors when the gizmo closes, while this
    // compact representation lets reopening restore the same painted area.
    ObjectID m_mask_object_id;
    std::vector<TriangleSelector::TriangleSplittingData> m_mask_data;
    // The ordinary multi-material paint as it existed before the first
    // stained-glass stroke. Every commit starts from this snapshot, then
    // applies the complete current stained-glass mask. This avoids retaining
    // leaf states generated by an earlier regeneration.
    std::vector<TriangleSelector::TriangleSplittingData> m_stained_glass_base_paint_data;
    std::vector<std::vector<GeneratedFragment>> m_generated_fragments;
    // Target colours are converted to normal virtual-filament paint states.
    std::vector<ColorRGBA> m_fragment_colors;
    std::vector<unsigned int> m_fragment_mixed_filament_ids;
    std::vector<uint64_t> m_stained_mixed_stable_ids;
    int      m_mask_object_index = -1;
};

} // namespace Slic3r::GUI

#endif // slic3r_GLGizmoTracery_hpp_
