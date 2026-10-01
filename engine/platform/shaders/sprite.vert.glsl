// Sprite vertex stage. One instance per sprite; the quad corners are generated
// from gl_VertexIndex so there is no vertex buffer beyond the instance stream
// and no index buffer at all.
//
// Resource sets follow the SDL GPU convention for SPIR-V vertex shaders:
//   set 0 = sampled textures / storage, set 1 = uniform buffers.
#version 450

layout(set = 1, binding = 0) uniform Viewport {
    // (2/width, -2/height): pixels -> clip, origin top-left.
    vec2 to_clip;
    vec2 pad;
} viewport;

layout(location = 0) in vec4 i_dst;       // x, y, w, h in target pixels
layout(location = 1) in vec4 i_src;       // u0, v0, u1, v1 in atlas texels, normalised
layout(location = 2) in vec4 i_modulate;  // multiplied over the palette colour
layout(location = 3) in float i_palette;  // v coordinate of this sprite's palette row

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_modulate;
layout(location = 2) out float v_palette;

void main() {
    // 0---1   two triangles, counter-clockwise in a y-down pixel space.
    // | \ |
    // 2---3
    vec2 corner;
    int c = gl_VertexIndex % 6;
    if (c == 0)      corner = vec2(0.0, 0.0);
    else if (c == 1) corner = vec2(1.0, 0.0);
    else if (c == 2) corner = vec2(0.0, 1.0);
    else if (c == 3) corner = vec2(0.0, 1.0);
    else if (c == 4) corner = vec2(1.0, 0.0);
    else             corner = vec2(1.0, 1.0);

    vec2 pixel = i_dst.xy + corner * i_dst.zw;
    gl_Position = vec4(pixel.x * viewport.to_clip.x - 1.0,
                       pixel.y * viewport.to_clip.y + 1.0,
                       0.0, 1.0);

    v_uv = mix(i_src.xy, i_src.zw, corner);
    v_modulate = i_modulate;
    v_palette = i_palette;
}
