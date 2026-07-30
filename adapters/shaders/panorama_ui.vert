#version 450
layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;

layout(push_constant) uniform PushConstants
{
    mat4 u_proj;
    vec4 u_linear_ab;
    vec4 u_translate_opacity;
} pc;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

void main()
{
    v_uv = in_uv;
    // Paint vertices are straight-alpha; image/font textures are
    // premultiplied. Premultiply the vertex modulation exactly once and apply
    // layer opacity to all channels before the fragment shader combines them.
    v_color = vec4(in_color.rgb * in_color.a, in_color.a) *
        pc.u_translate_opacity.z;
    vec2 local = vec2(
        pc.u_linear_ab.x * in_pos.x + pc.u_linear_ab.z * in_pos.y + pc.u_translate_opacity.x,
        pc.u_linear_ab.y * in_pos.x + pc.u_linear_ab.w * in_pos.y + pc.u_translate_opacity.y);
    gl_Position = pc.u_proj * vec4(local, 0.0, 1.0);
}
