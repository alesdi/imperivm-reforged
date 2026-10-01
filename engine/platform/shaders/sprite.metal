// Metal Shading Language port of sprite.vert.glsl / sprite.frag.glsl, and
// of layer.frag.glsl, which shares the vertex stage.
//
// It is hand-written rather than cross-compiled: the shader is forty lines, a
// second toolchain to install is a real cost for contributors, and SDL's Metal
// backend compiles MSL source at device creation anyway. Keep it in step with
// the GLSL by hand; docs/engine/rendering.md explains the trade.
//
// Binding order follows SDL_CreateGPUShader's MSL rules:
//   [[buffer]]  uniform buffers first, then storage; vertex buffers from 14 up
//   [[texture]] sampled textures in declaration order
//   [[sampler]] samplers matching the sampled textures

#include <metal_stdlib>
using namespace metal;

struct Viewport {
    float2 to_clip;  // (2/width, -2/height): pixels -> clip, origin top-left
    float2 pad;
};

struct Instance {
    float4 dst      [[attribute(0)]];
    float4 src      [[attribute(1)]];
    float4 modulate [[attribute(2)]];
    float  palette  [[attribute(3)]];
};

struct Varying {
    float4 position [[position]];
    float2 uv;
    float4 modulate;
    float  palette;
};

vertex Varying sprite_vertex(Instance in [[stage_in]],
                             uint vertex_index [[vertex_id]],
                             constant Viewport &viewport [[buffer(0)]])
{
    const float2 corners[6] = {
        float2(0.0, 0.0), float2(1.0, 0.0), float2(0.0, 1.0),
        float2(0.0, 1.0), float2(1.0, 0.0), float2(1.0, 1.0),
    };
    float2 corner = corners[vertex_index % 6];

    float2 pixel = in.dst.xy + corner * in.dst.zw;

    Varying out;
    out.position = float4(pixel.x * viewport.to_clip.x - 1.0,
                          pixel.y * viewport.to_clip.y + 1.0,
                          0.0, 1.0);
    out.uv = mix(in.src.xy, in.src.zw, corner);
    out.modulate = in.modulate;
    out.palette = in.palette;
    return out;
}

fragment float4 sprite_fragment(Varying in [[stage_in]],
                                texture2d<float> index_texture   [[texture(0)]],
                                sampler          index_sampler   [[sampler(0)]],
                                texture2d<float> palette_texture [[texture(1)]],
                                sampler          palette_sampler [[sampler(1)]])
{
    float index = index_texture.sample(index_sampler, in.uv).r;
    float lookup = (index * 255.0 + 0.5) / 256.0;
    float4 colour = palette_texture.sample(palette_sampler, float2(lookup, in.palette));

    float4 result = colour * in.modulate;
    if (result.a <= 0.0) {
        discard_fragment();
    }
    return result;
}

// layer.frag.glsl: the texel is the colour.
fragment float4 layer_fragment(Varying in [[stage_in]],
                               texture2d<float> image_texture [[texture(0)]],
                               sampler          image_sampler [[sampler(0)]])
{
    float4 result = image_texture.sample(image_sampler, in.uv) * in.modulate;
    if (result.a <= 0.0) {
        discard_fragment();
    }
    return result;
}
