#ifndef slic3r_GLGizmoTracery_hpp_
#define slic3r_GLGizmoTracery_hpp_

#include "GLGizmoPainterBase.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TraceryPartition.hpp"
#include "libslic3r/TraceryRestore.hpp"
#include "libslic3r/TraceryVectorSelection.hpp"
#include "libslic3r/TraceryVectorFill.hpp"
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
#include <tuple>
#include <vector>

namespace Slic3r { class TraceryRegionMapper; }

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
    void                       apply_cursor_to_edit_selector(size_t                                    mesh_idx,
                                                             int                                       facet,
                                                             std::unique_ptr<TriangleSelector::Cursor> cursor,
                                                             EnforcerBlockerType                       state,
                                                             const Transform3d&                        transform,
                                                             bool                                      split,
                                                             float                                     angle) override;
    void apply_seed_fill_to_edit_selector(size_t mesh_idx, EnforcerBlockerType new_state) override;
    bool select_vector_fill(size_t mesh_idx, const Vec3f& hit, int facet,
                            const TriangleSelector::ClippingPlane& clip, bool apply) override;
    void clear_fill_preview(size_t mesh_idx) override;
    ColorRGBA get_cursor_sphere_left_button_color() const override { return { 0.0f, 0.59f, 0.53f, 0.25f }; }
    ColorRGBA get_cursor_sphere_right_button_color() const override { return { 0.0f, 0.59f, 0.53f, 0.15f }; }

private:
    void commit_material_selection();
    void                                  apply_vector_stroke(size_t mesh_idx, const TriangleSelector::Tracery2DData& footprint);
    std::vector<TraceryVectorSelection>   m_vector_masks, m_previous_vector_masks, m_pending_vector_masks;
    bool                                  m_vector_stroke_pending = false;
    void                                  render_vector_selection();
    std::vector<std::shared_ptr<GLModel>> m_vector_selection_preview;
    void                                  update_vector_selection_preview(size_t mesh_idx);

    void refresh_source_selection_policy(bool filter_masks = false);
    std::vector<std::set<EnforcerBlockerType>> m_source_selection_policy;
    bool on_init() override;
    void update_model_object() override;
    void update_from_model_object(bool first_update) override;
    void on_opening() override
    {
        m_selection_modified = false;
        m_regeneration_requested = false;
        m_opening_fragment_density = m_fragment_density;
        m_density_input            = m_fragment_density;
        m_paint_mode = PaintMode::SURFACE;
    }
    void on_shutdown() override;
    void on_load(cereal::BinaryInputArchive& ar) override;
    void on_save(cereal::BinaryOutputArchive& ar) const override;
    PainterGizmoType get_painter_type() const override;

    void capture_selection_mask();
    void regenerate_stained_glass(bool randomize);
    void save_selection_mask();
    void erase_all_stained_glass();
    void generate_paint_plan();
    struct GenerationInput;
    struct PreparedGeneration;
    struct GenerationCache;
    std::shared_ptr<GenerationCache>    m_generation_cache;
    std::shared_ptr<PreparedGeneration> prepare_generation_input(const ModelObject* object);
    ColorRGBA                           volume_source_color(const ModelObject* object, int volume_index) const;
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
    bool assign_fragment_colors(const std::vector<ColorGroup>& color_groups,
                                const std::vector<FragmentSeed>& seeds,
                                const std::vector<std::set<size_t>>& fragment_neighbours,
                                size_t fragment_count, std::vector<ColorRGBA>& fragment_colors);
    void apply_stained_glass_colors(const std::function<void()>& rollback_preparation = {});
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
    void                                 generate_through_voronoi_regions();
    void                                 apply_incremental_boundary_transitions();
    void                                 commit_stained_glass_apply(StainedGlassApplyPlan&& plan);
    void bake_fragments_to_mmu_painting(bool prepared = false);
    TriangleSelector::Tracery2DData                      build_voronoi_2d_data(size_t                                         volume_index,
                                                                               const std::function<void(const std::string&)>& diagnostic,
                                                                               bool                                           material_budget = false,
                                                                               bool                                           front_only = false) const;
    ModelObject* find_masked_model_object(Model& model) const;
    std::vector<TriangleSelector::TriangleSplittingData> collect_partition_annotations(const ModelVolume& volume, size_t index) const;
    std::vector<GeneratedVoronoiRegion> remap_partition_regions(
        const std::vector<GeneratedVoronoiRegion>& regions, TraceryRegionMapper& mapper) const;
    PreparedTraceryVolume prepare_volume_tracery_partition(ModelVolume& volume, size_t index,
        const EnforcerBlockerStateMap* mapping = nullptr, const std::vector<unsigned int>* recipes = nullptr) const;
    void commit_tracery_mesh_update(PreparedTraceryVolume&& prepared);
    static std::vector<TriangleSelector::TriangleSplittingData> collect_volume_annotations(const ModelVolume& volume);
    static void replace_tracery_mesh(ModelVolume& volume, TraceryPartitionResult& partition);
    bool restore_original_tracery_meshes(ModelObject& object, bool& mesh_changed);
    void clear_stained_glass_editor();
    bool bake_exact_tracery_partitions(ModelObject& object);
    void notify_mmu_paint_changed(const ModelObject& object) const;
#ifndef NDEBUG
    void start_display_diagnostics();
    void stop_display_diagnostics();
    void record_display_diagnostic_state(const char* event, const std::string& detail = {});
#endif

    // Temporary triangles for the back-projection fallback; they do not replace
    // the model mesh and are converted back into vector regions.
    struct GeneratedFragment {
        size_t                   fragment_id = 0;
        indexed_triangle_set     surface;
        // Parallel to surface.indices: identifies the original model facet
        // from which each generated triangle was derived.
        std::vector<int>         source_facet_ids;
    };

    enum class PaintMode { SURFACE, THROUGH };

    // Clipped Voronoi polygons are stored against original source facets and
    // committed to the vector document for rendering and slicing.
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
        std::string document;
        std::vector<GeneratedVoronoiRegion> generated;
        std::vector<std::vector<GeneratedVoronoiRegion>> selections;
    };

    // A completed stroke owns its mode, generated front/back geometry and the
    // CMYW slots it received. Geometry is retained for later transition work;
    // ordinary incremental baking deliberately leaves previous strokes intact.
    struct TracerySelection
    {
        bool                                             full_through = false;
        PaintMode                                        mode         = PaintMode::SURFACE;
        std::vector<std::vector<GeneratedVoronoiRegion>> regions;
        std::vector<ColorRGBA>                           fragment_colors;
        std::vector<unsigned int>                        mixed_filament_ids;
    };

    // Prepares one projected back-side fragment. Its implementation will
    // retain front_fragment.fragment_id and fill source_facet_ids with the
    // facets hit on the target back surface.
    GeneratedFragment generate_back_fact_tracery_date(const GeneratedFragment& front_fragment,
                                                       const TriangleMesh& target_mesh,
                                                       size_t volume_index) const;

    PaintMode m_paint_mode = PaintMode::SURFACE;
    PaintMode m_pending_selection_mode = PaintMode::SURFACE;
    bool      m_full_through             = false;
    bool      m_pending_full_through     = false;
    wchar_t m_current_tool = 0;
    bool    m_eraser_mode = false;
    bool    m_detect_geometry_edge = true;
    bool    m_active_stroke_erases = false;
    bool    m_selection_modified = false;
    bool    m_regeneration_requested = false;
#ifndef NDEBUG
    bool    m_display_diagnostics_active = false;
#endif
#ifndef NDEBUG
    std::filesystem::path m_display_diagnostics_dir;
    std::string m_display_diagnostics_phase;
    std::string m_display_diagnostics_last_canvas_state;
    std::set<std::string> m_display_diagnostics_captured;
#endif
    float   m_fragment_density         = 0.5f;
    float   m_density_input            = 0.5f;
    float   m_opening_fragment_density = 0.5f;
    size_t  m_regeneration_version = 0;
    // Keep the editable selection independently from the model. The base
    // painter clears its live selectors when the gizmo closes, while this
    // compact representation lets reopening restore the same painted area.
    ObjectID m_mask_object_id;
    std::vector<TriangleSelector::TriangleSplittingData> m_mask_data;
    // Transient cache identity; deliberately not restored by undo snapshots.
    std::vector<size_t> m_edit_document_hashes;
    // Only reused during one synchronous commit. Pointer identity alone is not
    // enough: document bytes, paint timestamps, geometry and colours must match.
    using SelectorVolumeKey = std::tuple<ObjectID, std::shared_ptr<const TriangleMesh>, int,
                                         std::array<uint64_t, 5>, std::string>;
    using SelectorRefreshKey = std::tuple<const ModelObject*, ObjectID, std::vector<SelectorVolumeKey>, std::vector<ColorRGBA>>;
    bool m_commit_refresh_active = false;
    std::optional<SelectorRefreshKey> m_commit_selector_key;
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
    // Destroy before the immutable selectors they reference.
    std::vector<std::unique_ptr<TraceryVectorFill>> m_vector_fills;
    std::optional<std::tuple<size_t, size_t, bool>> m_last_vector_fill_stroke;
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
