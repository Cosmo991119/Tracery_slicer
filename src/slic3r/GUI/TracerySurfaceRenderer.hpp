#ifndef slic3r_TracerySurfaceRenderer_hpp_
#define slic3r_TracerySurfaceRenderer_hpp_
#include "libslic3r/Point.hpp"
#include "libslic3r/Color.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {
class ModelVolume;
class TraceryDocument;
namespace GUI {
struct Camera;
// GPU data is derived from the document. Only original model triangles are drawn.
class TracerySurfaceRenderer
{
public:
    TracerySurfaceRenderer() = default;
    TracerySurfaceRenderer(const TracerySurfaceRenderer&) = delete;
    TracerySurfaceRenderer& operator=(const TracerySurfaceRenderer&) = delete;
    ~TracerySurfaceRenderer();
    bool render(const ModelVolume& volume, const std::vector<ColorRGBA>& colors, const Transform3d& world,
                const Camera& camera, const std::array<float, 4>& clipping, const std::array<float, 2>& z_range, float alpha = 1.f);
private:
    bool update(const ModelVolume& volume, const std::vector<ColorRGBA>& colors);
    void upload(const TraceryDocument& doc, const std::vector<ColorRGBA>& colors, unsigned base);
    void draw(unsigned program);
    unsigned m_buffer = 0, m_texture = 0;
    int m_count = 0, m_width = 1, m_height = 1;
    uint64_t m_timestamp = 0;
    int m_base = 0;
    std::string m_document;
    std::vector<ColorRGBA> m_colors;
};
}
}
#endif
