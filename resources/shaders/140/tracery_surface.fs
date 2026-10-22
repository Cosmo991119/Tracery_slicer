#version 140
uniform sampler2D region_texture;
uniform vec2 texture_size;
uniform float surface_alpha;
in vec2 surface_uv;
in vec2 region_range;
in vec3 view_position;
in vec3 clipping_distances;
out vec4 out_color;
vec4 fetch_region(float index)
{
    return texture(region_texture, (vec2(mod(index, texture_size.x), floor(index / texture_size.x)) + 0.5) / texture_size);
}
float edge_distance_squared(vec2 p, vec2 a, vec2 b)
{
    vec2 edge = b - a;
    float t = clamp(dot(p - a, edge) / max(dot(edge, edge), 1e-20), 0.0, 1.0);
    vec2 d = p - a - t * edge;
    return dot(d, d);
}
vec3 region_color(vec2 p, vec3 fallback, bool nearest)
{
    float face = floor(region_range.x + 0.5);
    vec4 bounds = fetch_region(face);
    vec4 grid = fetch_region(face + 1.0);
    vec2 cell = clamp(floor((p - bounds.xy) / max(bounds.zw - bounds.xy, vec2(0.0000001)) * grid.x), vec2(0.0), vec2(grid.x - 1.0));
    vec4 range = fetch_region(grid.y + cell.y * grid.x + cell.x);
    float closest = 3.4e38;
    float entry = range.x;
    while (entry < range.y) {
        float offset = fetch_region(entry).x;
        vec4 header = fetch_region(offset);
        int count = int(header.x + 0.5);
        bool inside = false;
        float distance = 3.4e38;
        vec2 previous = fetch_region(offset + float(count)).xy;
        for (int i = 0; i < count; ++i) {
            vec2 current = fetch_region(offset + 1.0 + float(i)).xy;
            if ((current.y > p.y) != (previous.y > p.y)) {
                float x = (previous.x - current.x) * (p.y - current.y) / (previous.y - current.y) + current.x;
                if (p.x < x) inside = !inside;
            }
            if (nearest) distance = min(distance, edge_distance_squared(p, previous, current));
            previous = current;
        }
        if (inside) return header.yzw;
        if (nearest && distance < closest) {
            closest = distance;
            fallback = header.yzw;
        }
        entry += 1.0;
    }
    return fallback;
}
void main()
{
    if (any(lessThan(clipping_distances, vec3(0.0)))) discard;
    vec2 dx = dFdx(surface_uv) * 0.25;
    vec2 dy = dFdy(surface_uv) * 0.25;
    // Subpixel samples beyond this source facet must not borrow a different
    // region's color, which would draw seams along the original triangle edges.
    vec3 center = region_color(surface_uv, vec3(0.7), true);
    vec3 color = (region_color(surface_uv + dx + dy, center, false) + region_color(surface_uv + dx - dy, center, false) +
                  region_color(surface_uv - dx + dy, center, false) + region_color(surface_uv - dx - dy, center, false)) * 0.25;
    vec3 normal = normalize(cross(dFdx(view_position), dFdy(view_position)));
    float light = 0.45 + 0.55 * abs(dot(normal, normalize(vec3(-0.4, 0.6, 1.0))));
    out_color = vec4(color * light, surface_alpha);
}
