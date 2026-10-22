#version 140
uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;
uniform mat4 volume_world_matrix;
uniform vec4 clipping_plane;
uniform vec2 z_range;
in vec3 v_position;
in vec2 v_uv;
in vec2 v_range;
out vec2 surface_uv;
out vec2 region_range;
out vec3 view_position;
out vec3 clipping_distances;
void main()
{
    vec4 p = vec4(v_position, 1.0);
    vec4 world = volume_world_matrix * p;
    view_position = (view_model_matrix * p).xyz;
    gl_Position = projection_matrix * view_model_matrix * p;
    surface_uv = v_uv;
    region_range = v_range;
    clipping_distances = vec3(dot(world, clipping_plane), world.z - z_range.x, z_range.y - world.z);
}
