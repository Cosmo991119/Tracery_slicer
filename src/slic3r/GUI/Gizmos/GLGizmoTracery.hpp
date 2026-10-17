#ifndef slic3r_GLGizmoTracery_hpp_
#define slic3r_GLGizmoTracery_hpp_

#include "GLGizmoPainterBase.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TraceryPartition.hpp"
#include "libslic3r/TraceryRestore.hpp"
#include "libslic3r/TraceryPlateMaterials.hpp"

#include "slic3r/GUI/I18N.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
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
#ifndef NDEBUG
    void record_display_diagnostic_canvas(int object_index, bool picking_enabled, bool stencil_available,
                                          int matching_volumes, bool editing);
    void capture_display_diagnostic_frame(const char* stage, bool stencil_buffer);
    // Process-wide probe: ordinary view may render through a different canvas
    // than the one that owns this editor instance.
    static bool global_display_diagnostics_active();
    static void record_global_display_diagnostic_canvas(const void* canvas, const std::string& detail);
    static void capture_global_display_diagnostic_frame(const void* canvas, const char* stage);
#endif
    std::string get_tooltip() const override { return _u8L("Stained glass"); }

protected:
    void        on_render_input_window(float x, float y, float bottom_limit) override;
    std::string on_get_name() const override;
    wxString    handle_snapshot_action_name(bool shift_down, Button button_down) const override;

    std::string get_gizmo_entering_text() const override { return _u8L("Entering Stained glass"); }
    std::string get_gizmo_leaving_text() const override { return _u8L("Leaving Stained glass"); }
    std::string get_action_snapshot_name() const override { return _u8L("Stained glass editing"); }

    EnforcerBlockerType get_left_button_state_type() const override { return m_eraser_mode ? EnforcerBlockerType::NONE : EnforcerBlockerType::ENFORCER; }
    EnforcerBlockerType get_right_button_state_type() const override { return EnforcerBlockerType::NONE; }
    EnforcerBlockerType get_paint_state(bool left_button, bool erase) const override;
    TriangleSelectorGUI* get_edit_selector(size_t mesh_idx) override;
    const TriangleSelectorGUI* get_edit_selector(size_t mesh_idx) const override;
    size_t get_edit_selector_count() const override;
    void apply_seed_fill_to_edit_selector(size_t mesh_idx, EnforcerBlockerType new_state) override;
    ColorRGBA get_cursor_sphere_left_button_color() const override { return { 0.0f, 0.59f, 0.53f, 0.25f }; }
    ColorRGBA get_cursor_sphere_right_button_color() const override { return { 0.0f, 0.59f, 0.53f, 0.15f }; }

private:
    bool m_convert_plate_on_open = false;
    void commit_material_selection();
    void refresh_source_selection_policy(bool filter_masks = false);
    std::vector<std::set<EnforcerBlockerType>> m_source_selection_policy;
    bool on_init() override;
    void update_model_object() override;
    void update_from_model_object(bool first_update) override;
    void on_opening() override
    {
        m_convert_plate_on_open = true;
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
    void erase_all_stained_glass();
    void generate_paint_plan();
    struct GenerationInput;
    struct ColorGroup;
    struct FragmentSeed;
    struct VoronoiColorRegion;
    struct GeneratedVoronoiRegion;
    bool collect_generation_input(GenerationInput& input);
    void collect_volume_generation_input(GenerationInput& input, size_t mesh_idx, size_t volume_idx,
                                         const std::vector<ColorRGBA>& extruder_colors,
                                         const ColorRGBA& source_color) const;
    bool build_color_regions_from_input_faces(const GenerationInput& input,
                                              std::vector<ColorGroup>& color_groups,
                                              std::vector<VoronoiColorRegion>& regions) const;
    void allocate_region_fragment_budgets(std::vector<VoronoiColorRegion>& regions,
                                          float maximum_fragment_diameter) const;
    std::vector<Vec3f> sample_region_seed_candidates(const GenerationInput& input,
                                                      const VoronoiColorRegion& region) const;
    void generate_region_seeds(const GenerationInput& input, const std::vector<VoronoiColorRegion>& regions,
                               std::vector<FragmentSeed>& seeds) const;
    void generate_voronoi_regions(const GenerationInput& input, const std::vector<VoronoiColorRegion>& regions,
                                  const std::vector<FragmentSeed>& seeds,
                                  std::vector<std::vector<GeneratedVoronoiRegion>>& output) const;
    void build_fragment_topology(const std::vector<std::vector<GeneratedVoronoiRegion>>& regions,
                                 size_t fragment_count, std::vector<std::set<size_t>>& neighbours) const;
    void assign_voronoi_fragment_colors(const std::vector<ColorGroup>& color_groups,
                                        const std::vector<FragmentSeed>& seeds,
                                        const std::vector<std::set<size_t>>& neighbours);
    void publish_generated_regions(std::vector<std::vector<GeneratedVoronoiRegion>>&& regions);
    struct WorkTriangle;
    void build_work_surface(const GenerationInput& input, float boundary_sample_edge,
                            std::vector<WorkTriangle>& work_triangles) const;
    struct ColorRegionIntegrity;
    struct EdgeKey {
        size_t mesh_idx;
        std::array<int64_t, 6> points;
        bool operator<(const EdgeKey& other) const {
            return mesh_idx != other.mesh_idx ? mesh_idx < other.mesh_idx : points < other.points;
        }
    };
    ColorRegionIntegrity build_color_regions(std::vector<WorkTriangle>& work_triangles,
                                             std::vector<ColorGroup>& color_groups,
                                             std::vector<std::vector<size_t>>& group_triangles,
                                             std::vector<double>& group_areas,
                                             std::vector<size_t>& work_parents,
                                             std::map<EdgeKey, size_t>& work_edge_owners,
                                             std::map<size_t, std::array<int64_t, 4>>& work_anchors,
                                             std::map<size_t, size_t>& work_group_indices,
                                             bool verify_integrity) const;
    ColorRegionIntegrity inspect_color_regions(const std::vector<WorkTriangle>& work_triangles,
                                               const std::vector<ColorGroup>& color_groups,
                                               const std::vector<std::vector<size_t>>& group_triangles,
                                               const std::vector<double>& group_areas) const;
    struct CpuGeometry;
    struct FragmentGenerationIntegrity;
    bool generate_fragments(const std::vector<WorkTriangle>& work_triangles,
                            const std::vector<ColorGroup>& color_groups,
                            const std::vector<std::vector<size_t>>& group_triangles,
                            const std::vector<double>& group_areas,
                            float target_edge, float maximum_fragment_diameter,
                            std::map<EdgeKey, size_t>& work_edge_owners,
                            size_t& fragment_count, std::vector<FragmentSeed>& seeds,
                            std::vector<std::vector<CpuGeometry>>& geometry,
                            std::vector<size_t>& fragment_for_triangle,
                            std::vector<std::set<size_t>>& fragment_neighbours,
                            std::vector<float>& fragment_boundary_distance,
                            std::vector<size_t>& fragment_boundary_neighbor,
                            bool verify_integrity, FragmentGenerationIntegrity* builder_integrity);
    FragmentGenerationIntegrity inspect_fragment_generation(
        const std::vector<WorkTriangle>& work_triangles, const std::vector<ColorGroup>& color_groups,
        const std::vector<std::vector<size_t>>& group_triangles, const std::vector<double>& group_areas,
        float target_edge, size_t fragment_count, const std::vector<FragmentSeed>& seeds,
        const std::vector<std::vector<CpuGeometry>>& geometry,
        const std::vector<size_t>& fragment_for_triangle,
        const std::vector<std::set<size_t>>& fragment_neighbours,
        const std::vector<float>& fragment_boundary_distance,
        const std::vector<size_t>& fragment_boundary_neighbor) const;
    struct FragmentColorAssignmentIntegrity;
    bool assign_fragment_colors(const std::vector<ColorGroup>& color_groups,
                                const std::vector<FragmentSeed>& seeds,
                                const std::vector<std::set<size_t>>& fragment_neighbours,
                                const std::vector<float>& fragment_boundary_distance,
                                const std::vector<size_t>& fragment_boundary_neighbor,
                                size_t fragment_count, float target_edge, float refraction_strength,
                                std::vector<ColorRGBA>& fragment_colors, bool verify_integrity,
                                FragmentColorAssignmentIntegrity* builder_integrity);
    FragmentColorAssignmentIntegrity inspect_fragment_color_assignment(
        const std::vector<ColorGroup>& color_groups, const std::vector<FragmentSeed>& seeds,
        const std::vector<std::set<size_t>>& fragment_neighbours,
        const std::vector<float>& fragment_boundary_distance,
        const std::vector<size_t>& fragment_boundary_neighbor,
        size_t fragment_count, float target_edge, float refraction_strength,
        const std::vector<ColorRGBA>& fragment_colors) const;
    struct PublishGeneratedFragmentsIntegrity;
    void publish_generated_fragments(std::vector<std::vector<CpuGeometry>>& geometry,
                                     const std::vector<Transform3d>& volume_transforms,
                                     size_t fragment_count,
                                     bool verify_integrity,
                                     PublishGeneratedFragmentsIntegrity* builder_integrity);
    PublishGeneratedFragmentsIntegrity inspect_published_fragments(size_t fragment_count) const;
    void apply_stained_glass_colors();
    struct PreparedTraceryVolume;
    struct StainedGlassApplyPlan {
        enum class Mode {
            ConvertPlate,
            RestoreBasePaint,
            BakeFragments
        };

        Mode                      mode = Mode::RestoreBasePaint;
        std::vector<unsigned int> fragment_mixed_filament_ids;
        std::string               mixed_filament_definitions;
        std::vector<std::string> colours;
        std::vector<unsigned int> new_physical_ids;
        MixedFilamentManager manager;
        DynamicPrintConfig global_material_updates;
        std::map<int, DynamicPrintConfig> plate_material_updates;
        std::unique_ptr<Model> model;
        size_t selected_object_index = 0;
        size_t approximate_colour_count = 0;
        EnforcerBlockerStateMap conversion = tracery_identity_material_map();
        std::vector<PreparedTraceryVolume> partitions;
        std::vector<ColorRGBA> preview_colours;
    };
    std::optional<StainedGlassApplyPlan> prepare_stained_glass_apply(bool palette_only = false);
    void                                 generate_through_fragments();
    void                                 generate_through_voronoi_regions();
    void                                 apply_incremental_boundary_transitions();
    void                                 commit_stained_glass_apply(StainedGlassApplyPlan&& plan);
    void bake_fragments_to_mmu_painting(bool prepared = false);
    TriangleSelector::Tracery2DData build_voronoi_2d_data(
        size_t volume_index, const std::function<void(const std::string&)>& diagnostic, bool material_budget = false) const;
    TriangleSelector::Tracery2DData build_tracery_2d_data(size_t volume_index) const;
    ModelObject* find_masked_model_object(Model& model) const;
    std::vector<TriangleSelector::TriangleSplittingData> collect_partition_annotations(const ModelVolume& volume, size_t index) const;
    std::vector<GeneratedVoronoiRegion> remap_partition_regions(
        const std::vector<GeneratedVoronoiRegion>& regions, const TraceryPartitionResult& partition) const;
    PreparedTraceryVolume prepare_volume_tracery_partition(ModelVolume& volume, size_t index,
        const EnforcerBlockerStateMap* mapping = nullptr, const std::vector<unsigned int>* recipes = nullptr) const;
    void commit_tracery_mesh_update(PreparedTraceryVolume&& prepared);
    static std::vector<TriangleSelector::TriangleSplittingData> collect_volume_annotations(const ModelVolume& volume);
    static void replace_tracery_mesh(ModelVolume& volume, TraceryPartitionResult& partition);
    bool restore_original_tracery_meshes(ModelObject& object);
    void clear_stained_glass_editor();
    bool bake_exact_tracery_partitions(ModelObject& object);
    void notify_mmu_paint_changed(const ModelObject& object) const;
#ifndef NDEBUG
    void start_display_diagnostics();
    void stop_display_diagnostics();
    void record_display_diagnostic_state(const char* event, const std::string& detail = {});
#endif

    // CPU-only geometry used to classify surface points while baking the
    // generated colours into normal MMU paint. It is never rendered.
    struct GeneratedFragment {
        size_t                   fragment_id = 0;
        indexed_triangle_set     surface;
        // Parallel to surface.indices: identifies the original model facet
        // from which each generated triangle was derived.
        std::vector<int>         source_facet_ids;
    };

    enum class PaintMode { SURFACE, THROUGH };

    // The direct Voronoi path preserves a clipped convex polygon until it is
    // intersected with the selector leaves during the final MMU bake.
    struct GeneratedVoronoiRegion {
        size_t              fragment_id = 0;
        int                 source_facet_id = -1;
        std::vector<Vec3f>  local_polygon;
        // Set once a CMYW recipe has been resolved. It lets a dirty region
        // from an older selection be baked without remapping its local ID.
        unsigned int        mixed_filament_id = 0;
        bool                is_back_projection = false;
    };

    struct PreparedTraceryVolume {
        ModelVolume* volume = nullptr;
        size_t index = 0;
        TraceryPartitionResult partition;
        TraceryRestoreState restore_state;
        std::vector<GeneratedVoronoiRegion> generated;
        std::vector<std::vector<GeneratedVoronoiRegion>> selections;
    };

    // A completed stroke owns its mode, generated front/back geometry and the
    // CMYW slots it received. Geometry is retained for later transition work;
    // ordinary incremental baking deliberately leaves previous strokes intact.
    struct TracerySelection {
        PaintMode                                     mode = PaintMode::SURFACE;
        std::vector<std::vector<GeneratedVoronoiRegion>> regions;
        std::vector<ColorRGBA>                         fragment_colors;
        std::vector<unsigned int>                     mixed_filament_ids;
    };

    // Prepares one projected back-side fragment. Its implementation will
    // retain front_fragment.fragment_id and fill source_facet_ids with the
    // facets hit on the target back surface.
    GeneratedFragment generate_back_fact_tracery_date(const GeneratedFragment& front_fragment,
                                                       const TriangleMesh& target_mesh,
                                                       size_t volume_index) const;

    PaintMode m_paint_mode = PaintMode::SURFACE;
    PaintMode m_pending_selection_mode = PaintMode::SURFACE;
    wchar_t m_current_tool = 0;
    bool    m_eraser_mode = false;
    bool    m_detect_geometry_edge = true;
    bool    m_active_stroke_erases = false;
    bool    m_selection_modified = false;
    bool    m_regeneration_requested = false;
    bool    m_palette_update_requested = false;
#ifndef NDEBUG
    bool    m_display_diagnostics_active = false;
#endif
    bool    m_generation_integrity_valid = true;
#ifndef NDEBUG
    std::filesystem::path m_display_diagnostics_dir;
    std::string m_display_diagnostics_phase;
    std::string m_display_diagnostics_last_canvas_state;
    std::set<std::string> m_display_diagnostics_captured;
#endif
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
    // Selector data before the current completed stroke. It identifies the
    // added leaves, so a later stroke does not regenerate earlier selections.
    std::vector<TriangleSelector::TriangleSplittingData> m_previous_mask_data;
    // The ordinary multi-material paint as it existed before the first
    // stained-glass stroke. Every commit starts from this snapshot, then
    // applies the complete current stained-glass mask. This avoids retaining
    // leaf states generated by an earlier regeneration.
    std::vector<TriangleSelector::TriangleSplittingData> m_stained_glass_base_paint_data;
    // Session/undo snapshots follow the volume ID across each exact remesh.
    std::map<ObjectID, TraceryRestoreState> m_mesh_restore_states;
    // The normal MMU selector is the source of truth for face and bucket
    // selection. m_triangle_selectors stores only the stained-glass effect
    // flag, on the exact same leaf topology.
    std::vector<std::unique_ptr<TriangleSelectorPatch>> m_fill_reference_selectors;
    std::vector<std::vector<GeneratedFragment>> m_generated_fragments;
    std::vector<std::vector<GeneratedVoronoiRegion>> m_generated_voronoi_regions;
    std::vector<TracerySelection> m_tracery_selections;
    bool m_incremental_generation = false;
    size_t m_pending_selection_fragment_count = 0;
    // Maps transient palette entries added for old boundary fragments back to
    // their persistent selection-local recipe slots.
    std::vector<std::pair<size_t, size_t>> m_transition_recipe_targets;
    // Target colours are converted to normal virtual-filament paint states.
    std::vector<ColorRGBA> m_fragment_colors;
    std::vector<unsigned int> m_fragment_mixed_filament_ids;
    std::vector<uint64_t> m_stained_mixed_stable_ids;
    int      m_mask_object_index = -1;
};

} // namespace Slic3r::GUI

#endif // slic3r_GLGizmoTracery_hpp_
