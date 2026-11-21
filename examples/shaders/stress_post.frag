#version 450

layout(set = 0, binding = 0) uniform sampler2D scene_texture;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform SceneParams {
    vec2 resolution;
    float time;
    float pad;
} params;

void main()
{
    vec2 texel = 1.0 / params.resolution;

    /* 3x3 tent blur over the particle scene. */
    vec3 sum = vec3(0.0);
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 offset = vec2(float(x), float(y)) * texel * 1.4;
            sum += texture(scene_texture, v_uv + offset).rgb;
        }
    }
    vec3 color = sum * (1.0 / 9.0);

    vec2 q = v_uv * 2.0 - 1.0;
    q.x *= params.resolution.x / max(params.resolution.y, 1.0);
    float radius = length(q);
    float plasma = sin(q.x * 3.1 + params.time) * cos(q.y * 2.7 - params.time * 0.8);
    float vignette = smoothstep(1.65, 0.25, radius);
    float scanline = 0.94 + 0.06 * sin(v_uv.y * params.resolution.y * 3.14159265);

    color += vec3(0.018, 0.035, 0.060) * (0.6 + 0.4 * plasma);
    color *= vignette * scanline;

    out_color = vec4(color, 1.0);
}
