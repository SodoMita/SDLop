#version 450

layout(location = 0) in vec3 v_color;
layout(location = 1) in vec2 v_corner;

layout(push_constant) uniform SceneParams {
    vec2 resolution;
    float time;
    float pad;
} params;

layout(location = 0) out vec4 out_color;

void main()
{
    float d = length(v_corner);
    if (d > 1.0) {
        discard;
    }

    /* Deliberately expensive per-fragment turbulence. */
    vec2 z = v_corner * 2.5 + vec2(params.time * 0.21, -params.time * 0.17);
    float acc = 0.0;
    for (int i = 0; i < 18; ++i) {
        z = vec2(z.x + sin(z.y * 1.7 + float(i) * 0.37),
                 z.y + cos(z.x * 1.3 - float(i) * 0.29));
        acc += length(z) * 0.008;
    }
    float pulse = 0.55 + 0.45 * sin(acc * 7.0 + params.time * 2.0);
    float glow = pow(1.0 - d, 2.2);

    out_color = vec4(v_color * glow * pulse, glow * 0.65);
}
