#include "TracerySurfaceRenderer.hpp"
#include "libslic3r/TraceryDocument.hpp"
#include "libslic3r/TraceryPerformance.hpp"
#include "libslic3r/Model.hpp"
#include "GUI_App.hpp"
#include "GUI.hpp"
#include "GLShader.hpp"
#include "Camera.hpp"
#include <GL/glew.h>
#include <algorithm>
#include <stdexcept>
#include <cmath>
#include <limits>
#include <boost/log/trivial.hpp>

namespace Slic3r::GUI {
namespace {
struct SurfaceVertex { float x, y, z, u, v, begin, end; };
using Texel = std::array<float, 4>;
void append_region(std::vector<Texel>& data, const TriangleSelector::TraceryRegion2D& region,
                   const std::vector<ColorRGBA>& colors, unsigned base)
{
    const unsigned id = unsigned(region.color) == 0 ? base : unsigned(region.color);
    const auto color = adjust_color_for_rendering(colors.at(std::min(size_t(id - 1), colors.size() - 1)));
    data.push_back({float(region.polygon.size()), color.r(), color.g(), color.b()});
    for (const auto& p : region.polygon) data.push_back({float(p.x()), float(p.y()), 0, 0});
}
float append_facet_index(std::vector<Texel>& data, const TriangleSelector::TraceryFacet2DData& facet,
                         const std::vector<ColorRGBA>& colors, unsigned base)
{
    Vec2d low = Vec2d::Constant(std::numeric_limits<double>::max());
    Vec2d high = -low;
    std::vector<std::pair<Vec2d, Vec2d>> bounds;
    for (const auto& region : facet.regions) {
        Vec2d a = Vec2d::Constant(std::numeric_limits<double>::max()), b = -a;
        for (const auto& p : region.polygon) { a = a.cwiseMin(p); b = b.cwiseMax(p); }
        bounds.emplace_back(a, b);
        low = low.cwiseMin(a); high = high.cwiseMax(b);
    }
    const int cells = std::clamp(int(std::ceil(std::sqrt(double(facet.regions.size()) / 4.))), 1, 32);
    const size_t header = data.size();
    data.push_back({float(low.x()), float(low.y()), float(high.x()), float(high.y())});
    data.push_back({float(cells), float(header + 2), 0, 0});
    data.resize(data.size() + size_t(cells * cells), Texel{});
    std::vector<float> offsets;
    for (const auto& region : facet.regions) {
        offsets.push_back(float(data.size()));
        append_region(data, region, colors, base);
    }
    const Vec2d step = (high - low) / double(cells);
    for (int y = 0; y < cells; ++y) for (int x = 0; x < cells; ++x) {
        const Vec2d a = low + step.cwiseProduct(Vec2d(x, y)), b = a + step;
        const float begin = float(data.size());
        for (size_t i = 0; i < bounds.size(); ++i)
            if ((bounds[i].first.array() <= b.array()).all() && (bounds[i].second.array() >= a.array()).all())
                data.push_back({offsets[i], 0, 0, 0});
        data[header + 2 + size_t(y * cells + x)] = {begin, float(data.size()), 0, 0};
    }
    return float(header);
}

}
TracerySurfaceRenderer::~TracerySurfaceRenderer()
{
    if (m_buffer) glDeleteBuffers(1, &m_buffer);
    if (m_texture) glDeleteTextures(1, &m_texture);
}

bool TracerySurfaceRenderer::update(const ModelVolume& volume, const std::vector<ColorRGBA>& colors)
{
    if (volume.tracery_edit_data.empty() || colors.empty()) return false;
    if (m_document == volume.tracery_edit_data && m_timestamp == volume.mmu_segmentation_facets.timestamp() &&
        m_colors == colors && m_base == volume.extruder_id()) return m_count > 0;
    TraceryPerformanceScope performance("vector_renderer_update");
    performance.stage("renderer.decode_document");
    try {
        auto doc = TraceryDocument::from_volume_snapshot(volume);
        if (!doc) return false;
        performance.stage("renderer.upload");
        upload(*doc, colors, unsigned(std::max(1, volume.extruder_id())));
    } catch (const std::exception& error) {
        performance.mark_failed();
        m_count = 0;
        BOOST_LOG_TRIVIAL(error) << "Cannot render vector stained glass: " << error.what();
    }
    m_document = volume.tracery_edit_data;
    m_timestamp = volume.mmu_segmentation_facets.timestamp();
    m_colors = colors;
    m_base = volume.extruder_id();
    return m_count > 0;
}

void TracerySurfaceRenderer::upload(const TraceryDocument& doc, const std::vector<ColorRGBA>& colors, unsigned base)
{
    TraceryPerformanceScope performance("vector_renderer_upload");
    performance.stage("renderer.surface_regions");
    const auto regions = doc.surface_regions();
    performance.count("surface_facets", regions.size());
    performance.stage("renderer.build_index");
    const auto& mesh = doc.base_mesh().its;
    std::vector<SurfaceVertex> vertices;
    std::vector<Texel> data;
    for (const auto& facet : regions) {
        const float begin = append_facet_index(data, facet, colors, base);
        const float end = begin;
        for (int index : mesh.indices.at(facet.source_facet_id)) {
            const Vec3f& p = mesh.vertices[index];
            const Vec3d d = p.cast<double>() - facet.origin;
            vertices.push_back({p.x(), p.y(), p.z(), float(d.dot(facet.axis_u) / facet.axis_u.squaredNorm()),
                               float(d.dot(facet.axis_v) / facet.axis_v.squaredNorm()), begin, end});
        }
    }
    performance.count("vertices", vertices.size());
    performance.count("texels", data.size());
    performance.stage("renderer.gpu_upload");
    GLint maximum = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum);
    if (maximum <= 0) throw std::runtime_error("No OpenGL texture capacity");
    m_width = std::min(maximum, 4096);
    m_height = std::max(1, int((data.size() + m_width - 1) / m_width));
    if (m_height > maximum || data.size() >= (1u << 24))
        throw std::runtime_error("Vector surface exceeds GPU texture capacity");
    data.resize(size_t(m_width) * m_height, Texel{});
    GLint old_buffer = 0, old_texture = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_buffer);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
    if (!m_buffer) glGenBuffers(1, &m_buffer);
    if (!m_texture) glGenTextures(1, &m_texture);
    glBindBuffer(GL_ARRAY_BUFFER, m_buffer);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(SurfaceVertex), vertices.data(), GL_STATIC_DRAW);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, m_width, m_height, 0, GL_RGBA, GL_FLOAT, data.data());
    glBindTexture(GL_TEXTURE_2D, old_texture);
    glBindBuffer(GL_ARRAY_BUFFER, old_buffer);
    m_count = int(vertices.size());
}

void TracerySurfaceRenderer::draw(unsigned program)
{
    GLint previous_buffer = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previous_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, m_buffer);
    const char* names[] = {"v_position", "v_uv", "v_range"};
    const int sizes[] = {3, 2, 2};
    const size_t offsets[] = {0, 3 * sizeof(float), 5 * sizeof(float)};
    GLint locations[3];
    for (size_t i = 0; i < 3; ++i) {
        locations[i] = glGetAttribLocation(program, names[i]);
        if (locations[i] < 0) continue;
        glEnableVertexAttribArray(locations[i]);
        glVertexAttribPointer(locations[i], sizes[i], GL_FLOAT, GL_FALSE, sizeof(SurfaceVertex), reinterpret_cast<void*>(offsets[i]));
    }
    glDrawArrays(GL_TRIANGLES, 0, m_count);
    for (GLint location : locations) if (location >= 0) glDisableVertexAttribArray(location);
    glBindBuffer(GL_ARRAY_BUFFER, previous_buffer);
}

bool TracerySurfaceRenderer::render(const ModelVolume& volume, const std::vector<ColorRGBA>& colors, const Transform3d& world,
                                    const Camera& camera, const std::array<float, 4>& clipping,
                                    const std::array<float, 2>& z_range, float alpha)
{
    auto* shader = wxGetApp().get_shader("tracery_surface");
    if (!shader || !update(volume, colors)) return false;
    auto* previous = wxGetApp().get_current_shader();
    GLint active = 0, texture = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    shader->start_using();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * world);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("volume_world_matrix", world);
    shader->set_uniform("clipping_plane", clipping);
    shader->set_uniform("z_range", z_range);
    shader->set_uniform("region_texture", 0);
    shader->set_uniform("texture_size", Vec2f(float(m_width), float(m_height)));
    shader->set_uniform("surface_alpha", alpha);
    draw(shader->get_id());
    shader->stop_using();
    if (previous) previous->start_using();
    glBindTexture(GL_TEXTURE_2D, texture);
    glActiveTexture(active);
    return true;
}
}
