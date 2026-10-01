// Layer fragment stage: a true-colour RGBA8 texture, drawn as it is.
//
// The interface is composited on the CPU into RGBA buffers -- the bars, a
// tooltip, a menu over the world -- and this puts one on the screen. It
// shares the sprite vertex stage (the same instance layout; the palette
// attribute is carried and ignored) and differs only here: no palette
// lookup, the texel is the colour. Alpha comes from the buffer, so a menu's
// dark frame halves the world under it and a frame's colour-keyed middle
// lets it through, which a blit cannot do.
//
// Resource sets follow the SDL GPU convention for SPIR-V fragment shaders:
//   set 2 = sampled textures, set 3 = uniform buffers.
#version 450

layout(set = 2, binding = 0) uniform sampler2D u_image;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_modulate;
layout(location = 2) in float v_palette;

layout(location = 0) out vec4 o_color;

void main() {
    o_color = texture(u_image, v_uv) * v_modulate;
    if (o_color.a <= 0.0) {
        discard;
    }
}
