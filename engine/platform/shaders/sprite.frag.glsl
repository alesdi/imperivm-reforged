// Sprite fragment stage: the palette indirection that makes team colour a
// per-player palette row rather than a per-player atlas.
//
// u_index is R8_UNORM. Its texel is a palette *index*, not a colour, so it must
// be sampled with a NEAREST filter -- interpolating two indices produces a third
// index, which is a different colour entirely rather than a blend of two.
//
// u_palette is RGBA8. Row v_palette is the 256-entry palette this sprite is
// being drawn with; a class-2 (player_color) sprite gets one row per player,
// with slots 0..63 replaced by that player's ramp, and every row points at the
// same index texels.
//
// Transparency comes out of the palette too. The sprite format has no alpha
// channel: gaps in the run encoding are the only transparency, and the atlas
// fills them with an index the image does not use, whose palette entry has
// alpha 0.
//
// Resource sets follow the SDL GPU convention for SPIR-V fragment shaders:
//   set 2 = sampled textures, set 3 = uniform buffers.
#version 450

layout(set = 2, binding = 0) uniform sampler2D u_index;
layout(set = 2, binding = 1) uniform sampler2D u_palette;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_modulate;
layout(location = 2) in float v_palette;

layout(location = 0) out vec4 o_color;

void main() {
    float index = texture(u_index, v_uv).r;
    // n/255 -> the centre of texel n of a 256-wide lookup.
    float lookup = (index * 255.0 + 0.5) / 256.0;
    vec4 colour = texture(u_palette, vec2(lookup, v_palette));

    o_color = colour * v_modulate;
    if (o_color.a <= 0.0) {
        discard;
    }
}
