#version 450

/* Instanced quads. Binding 0 is a static corner buffer, binding 1 is the
 * storage buffer written by the compute pass. */

layout(location = 0) in vec2 in_corner;
layout(location = 1) in vec2 in_pos;
layout(location = 2) in vec3 in_color;
layout(location = 3) in float in_size;

layout(push_constant) uniform SceneParams {
    vec2 resolution;
    float time;
    float pad;
} params;

layout(location = 0) out vec3 v_color;
layout(location = 1) out vec2 v_corner;

void main()
{
    float angle = params.time * 0.35 + in_pos.x * 1.7 + in_pos.y * 0.9;
    float c = cos(angle);
    float s = sin(angle);
    vec2 rotated = vec2(in_corner.x * c - in_corner.y * s,
                        in_corner.x * s + in_corner.y * c);

    vec2 p = in_pos + rotated * in_size;
    p.x *= params.resolution.y / max(params.resolution.x, 1.0);

    gl_Position = vec4(p, 0.0, 1.0);
    v_color = in_color;
    v_corner = in_corner;
}
