#ifndef slic3r_TraceryDocument_hpp_
#define slic3r_TraceryDocument_hpp_
#include "TraceryPartition.hpp"
#include "Polygon.hpp"
#include <map>
#include <memory>

namespace Slic3r {
class ModelVolume;
enum class TraceryEdit { Paint, Erase, Regenerate };

// Authoritative regions live in immutable base-facet coordinates. Neither the
// generated mesh nor its triangle IDs are ever promoted into the base.
class TraceryDocument
{
public:
    using Annotation = TriangleSelector::TriangleSplittingData;
    using Regions    = std::map<int, std::map<unsigned, Polygons>>;
    static TraceryDocument                 capture(const TriangleMesh& mesh, const std::vector<Annotation>& annotations);
    TraceryPartitionResult                 rebuild(const TriangleMesh&                    current,
                                                   const std::vector<Annotation>&         annotations,
                                                   const TriangleSelector::Tracery2DData& paint,
                                                   TraceryEdit                            edit);
    static std::unique_ptr<TraceryDocument> from_volume(const ModelVolume& volume);
    // Immutable snapshots for readers; editable documents remain independent copies.
    static std::shared_ptr<const TraceryDocument> decode_cached(const std::string& data);
    static std::shared_ptr<const TraceryDocument> from_volume_snapshot(const ModelVolume& volume);
    // Keep a refresh operation's working set alive even when it exceeds the idle
    // cache budget. Scopes are thread-local, nestable and release on exceptions.
    class ReadCacheScope
    {
    public:
        ReadCacheScope();
        ~ReadCacheScope();
        ReadCacheScope(const ReadCacheScope&) = delete;
        ReadCacheScope& operator=(const ReadCacheScope&) = delete;
    private:
        struct State;
        std::unique_ptr<State> m_state;
        friend class TraceryDocument;
    };
    const Annotation& selection_reference() const { return m_selection_reference; }
    void set_selection_reference(Annotation reference) { m_selection_reference = std::move(reference); }
    bool is_vector() const { return m_vector; }
    TraceryPartitionResult apply_vector_edit(const TriangleMesh& current, const std::vector<Annotation>& annotations,
                                            const TriangleSelector::Tracery2DData& paint, TraceryEdit edit);
    void clear_vector_glass(const TriangleMesh& current, const std::vector<Annotation>& annotations);
    TriangleSelector::Tracery2DData surface_regions() const;
    std::map<unsigned, indexed_triangle_set> material_surfaces() const;
    std::vector<unsigned> material_ids() const;
    void apply_ordinary_paint(const TriangleSelector::Tracery2DData& paint);
    const TriangleMesh&                    base_mesh() const { return m_base; }
    const std::vector<int>&                source_facets() const { return m_sources; }
    const std::vector<Annotation>&         last_annotations() const { return m_last; }
    bool                                   matches(const TriangleMesh& mesh) const;
    std::string                            encode() const;
    std::string                            encode_replay(const std::vector<Annotation>&         annotations,
                                                         const TriangleSelector::Tracery2DData& paint,
                                                         TraceryEdit                            edit) const;
    static TraceryPartitionResult          replay(const std::string& data);
    static TraceryDocument                 decode(const std::string& data);
    static TriangleSelector::Tracery2DData erased_regions(const TriangleMesh& mesh, const Annotation& before, const Annotation& after);
    void                                   remap_materials(const EnforcerBlockerStateMap& mapping);

private:
    std::vector<Regions> updated_channels(const TriangleMesh& current, const std::vector<Annotation>& annotations,
                                          const TriangleSelector::Tracery2DData& paint, TraceryEdit edit) const;
    Annotation m_selection_reference;
    bool m_vector = false;
    TriangleMesh            m_base;
    indexed_triangle_set    m_derived;
    std::vector<int>        m_sources;
    std::vector<Regions>    m_channels;
    std::vector<Annotation> m_last;
};
} // namespace Slic3r
#endif
