#include "imperivm/platform/layer_renderer.hpp"

#include <algorithm>
#include <cstddef>

#include <SDL3/SDL.h>

#include "generated/sprite_shaders.inc"

namespace imperivm::platform {
namespace {

bool fail(std::string* error, const char* what) {
  if (error != nullptr) *error = std::string(what) + ": " + SDL_GetError();
  return false;
}

/// The sprite vertex stage and the layer fragment stage, in whichever form
/// the backend takes.
SDL_GPUShader* create_shader(SDL_GPUDevice* device, SDL_GPUShaderStage stage) {
  const SDL_GPUShaderFormat available = SDL_GetGPUShaderFormats(device);
  const bool vertex = stage == SDL_GPU_SHADERSTAGE_VERTEX;

  SDL_GPUShaderCreateInfo info{};
  info.stage = stage;
  info.num_samplers = vertex ? 0 : 1;
  info.num_uniform_buffers = vertex ? 1 : 0;

  if ((available & SDL_GPU_SHADERFORMAT_SPIRV) != 0) {
    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    info.entrypoint = "main";
    if (vertex) {
      info.code = reinterpret_cast<const Uint8*>(shaders::kSpriteVertexSpirv);
      info.code_size = sizeof(shaders::kSpriteVertexSpirv);
    } else {
      info.code = reinterpret_cast<const Uint8*>(shaders::kLayerFragmentSpirv);
      info.code_size = sizeof(shaders::kLayerFragmentSpirv);
    }
  } else if ((available & SDL_GPU_SHADERFORMAT_MSL) != 0) {
    info.format = SDL_GPU_SHADERFORMAT_MSL;
    info.entrypoint = vertex ? "sprite_vertex" : "layer_fragment";
    info.code = reinterpret_cast<const Uint8*>(shaders::kSpriteMsl);
    info.code_size = sizeof(shaders::kSpriteMsl);
  } else {
    SDL_SetError("no shader format this build ships (SPIR-V, MSL) is accepted by the %s backend",
                 SDL_GetGPUDeviceDriver(device));
    return nullptr;
  }
  return SDL_CreateGPUShader(device, &info);
}

}  // namespace

LayerRenderer::~LayerRenderer() { destroy(); }

bool LayerRenderer::create(SDL_GPUDevice* device, std::uint32_t target_format,
                           std::string* error) {
  destroy();
  device_ = device;
  target_format_ = target_format;
  if (device_ == nullptr) {
    if (error != nullptr) *error = "no GPU device";
    return false;
  }
  if (!build_pipeline(error)) {
    destroy();
    return false;
  }
  // Nearest: a layer is drawn at its own size, pixel for pixel, and the
  // interface art is not to be smoothed.
  SDL_GPUSamplerCreateInfo sampler{};
  sampler.min_filter = SDL_GPU_FILTER_NEAREST;
  sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
  sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
  sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  sampler_ = SDL_CreateGPUSampler(device_, &sampler);
  if (sampler_ == nullptr) {
    (void)fail(error, "SDL_CreateGPUSampler");
    destroy();
    return false;
  }
  // And a linear one, for a layer stretched over the view.
  sampler.min_filter = SDL_GPU_FILTER_LINEAR;
  sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
  smooth_sampler_ = SDL_CreateGPUSampler(device_, &sampler);
  if (smooth_sampler_ == nullptr) {
    (void)fail(error, "SDL_CreateGPUSampler");
    destroy();
    return false;
  }
  return true;
}

void LayerRenderer::draw_scaled(SDL_GPUTexture* texture, std::int32_t x, std::int32_t y,
                                std::int32_t draw_width, std::int32_t draw_height, bool smooth) {
  if (texture == nullptr || draw_width <= 0 || draw_height <= 0) return;
  Queued queued{};
  queued.texture = texture;
  queued.smooth = smooth;
  queued.instance.dst[0] = static_cast<float>(x);
  queued.instance.dst[1] = static_cast<float>(y);
  queued.instance.dst[2] = static_cast<float>(draw_width);
  queued.instance.dst[3] = static_cast<float>(draw_height);
  queued.instance.src[0] = 0.0F;
  queued.instance.src[1] = 0.0F;
  queued.instance.src[2] = 1.0F;
  queued.instance.src[3] = 1.0F;
  for (float& channel : queued.instance.modulate) channel = 1.0F;
  queued.instance.palette = 0.0F;
  queue_.push_back(queued);
}

bool LayerRenderer::build_pipeline(std::string* error) {
  SDL_GPUShader* vertex = create_shader(device_, SDL_GPU_SHADERSTAGE_VERTEX);
  if (vertex == nullptr) return fail(error, "layer vertex shader");
  SDL_GPUShader* fragment = create_shader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT);
  if (fragment == nullptr) {
    SDL_ReleaseGPUShader(device_, vertex);
    return fail(error, "layer fragment shader");
  }

  SDL_GPUVertexBufferDescription buffers[1]{};
  buffers[0].slot = 0;
  buffers[0].pitch = sizeof(Instance);
  buffers[0].input_rate = SDL_GPU_VERTEXINPUTRATE_INSTANCE;

  SDL_GPUVertexAttribute attributes[4]{};
  attributes[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(Instance, dst)};
  attributes[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(Instance, src)};
  attributes[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(Instance, modulate)};
  attributes[3] = {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(Instance, palette)};

  // Source-over, straight alpha: the canvases are composited that way.
  SDL_GPUColorTargetDescription color{};
  color.format = static_cast<SDL_GPUTextureFormat>(target_format_);
  color.blend_state.enable_blend = true;
  color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
  color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
  color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
  color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

  SDL_GPUGraphicsPipelineCreateInfo info{};
  info.vertex_shader = vertex;
  info.fragment_shader = fragment;
  info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  info.vertex_input_state.vertex_buffer_descriptions = buffers;
  info.vertex_input_state.num_vertex_buffers = 1;
  info.vertex_input_state.vertex_attributes = attributes;
  info.vertex_input_state.num_vertex_attributes = 4;
  info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
  info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
  info.target_info.color_target_descriptions = &color;
  info.target_info.num_color_targets = 1;

  pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
  SDL_ReleaseGPUShader(device_, vertex);
  SDL_ReleaseGPUShader(device_, fragment);
  if (pipeline_ == nullptr) return fail(error, "SDL_CreateGPUGraphicsPipeline (layer)");
  return true;
}

void LayerRenderer::destroy() {
  if (device_ != nullptr) {
    if (sampler_ != nullptr) SDL_ReleaseGPUSampler(device_, sampler_);
    if (smooth_sampler_ != nullptr) SDL_ReleaseGPUSampler(device_, smooth_sampler_);
    if (instance_buffer_ != nullptr) SDL_ReleaseGPUBuffer(device_, instance_buffer_);
    if (pipeline_ != nullptr) SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
  }
  sampler_ = nullptr;
  smooth_sampler_ = nullptr;
  instance_buffer_ = nullptr;
  instance_capacity_ = 0;
  pipeline_ = nullptr;
  queue_.clear();
  device_ = nullptr;
}

void LayerRenderer::draw(SDL_GPUTexture* texture, std::uint32_t width, std::uint32_t height,
                         std::int32_t x, std::int32_t y) {
  if (texture == nullptr || width == 0 || height == 0) return;
  Queued queued{};
  queued.texture = texture;
  queued.instance.dst[0] = static_cast<float>(x);
  queued.instance.dst[1] = static_cast<float>(y);
  queued.instance.dst[2] = static_cast<float>(width);
  queued.instance.dst[3] = static_cast<float>(height);
  queued.instance.src[0] = 0.0F;
  queued.instance.src[1] = 0.0F;
  queued.instance.src[2] = 1.0F;
  queued.instance.src[3] = 1.0F;
  for (float& channel : queued.instance.modulate) channel = 1.0F;
  queued.instance.palette = 0.0F;
  queue_.push_back(queued);
}

bool LayerRenderer::ensure_capacity(std::size_t instances) {
  if (instances <= instance_capacity_) return true;
  if (instance_buffer_ != nullptr) SDL_ReleaseGPUBuffer(device_, instance_buffer_);
  std::size_t capacity = std::max<std::size_t>(instance_capacity_ * 2, 16);
  while (capacity < instances) capacity *= 2;
  SDL_GPUBufferCreateInfo info{};
  info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
  info.size = static_cast<Uint32>(capacity * sizeof(Instance));
  instance_buffer_ = SDL_CreateGPUBuffer(device_, &info);
  instance_capacity_ = instance_buffer_ != nullptr ? capacity : 0;
  return instance_buffer_ != nullptr;
}

bool LayerRenderer::render(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target,
                           std::uint32_t viewport_width, std::uint32_t viewport_height,
                           std::string* error) {
  if (device_ == nullptr || pipeline_ == nullptr || commands == nullptr || target == nullptr) {
    return false;
  }
  if (queue_.empty() || viewport_width == 0 || viewport_height == 0) {
    queue_.clear();
    return true;
  }
  if (!ensure_capacity(queue_.size())) return fail(error, "SDL_CreateGPUBuffer (layers)");

  SDL_GPUTransferBufferCreateInfo transfer_info{};
  transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
  transfer_info.size = static_cast<Uint32>(queue_.size() * sizeof(Instance));
  SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &transfer_info);
  if (transfer == nullptr) return fail(error, "SDL_CreateGPUTransferBuffer (layers)");
  auto* mapped = static_cast<Instance*>(SDL_MapGPUTransferBuffer(device_, transfer, false));
  if (mapped == nullptr) {
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
    return fail(error, "SDL_MapGPUTransferBuffer (layers)");
  }
  for (std::size_t i = 0; i < queue_.size(); ++i) mapped[i] = queue_[i].instance;
  SDL_UnmapGPUTransferBuffer(device_, transfer);

  SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
  SDL_GPUTransferBufferLocation source{transfer, 0};
  SDL_GPUBufferRegion destination{instance_buffer_, 0, transfer_info.size};
  SDL_UploadToGPUBuffer(copy, &source, &destination, true);
  SDL_EndGPUCopyPass(copy);
  SDL_ReleaseGPUTransferBuffer(device_, transfer);

  SDL_GPUColorTargetInfo color{};
  color.texture = target;
  color.load_op = SDL_GPU_LOADOP_LOAD;
  color.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &color, 1, nullptr);
  if (pass == nullptr) return fail(error, "SDL_BeginGPURenderPass (layers)");

  SDL_BindGPUGraphicsPipeline(pass, pipeline_);
  struct {
    float to_clip[2];
    float pad[2];
  } uniform{{2.0F / static_cast<float>(viewport_width), -2.0F / static_cast<float>(viewport_height)},
            {0.0F, 0.0F}};
  SDL_PushGPUVertexUniformData(commands, 0, &uniform, sizeof(uniform));

  // One draw per layer: each has its own texture, and there are a handful.
  for (std::size_t i = 0; i < queue_.size(); ++i) {
    SDL_GPUTextureSamplerBinding binding{queue_[i].texture, queue_[i].smooth ? smooth_sampler_ : sampler_};
    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
    SDL_GPUBufferBinding vertices{instance_buffer_, static_cast<Uint32>(i * sizeof(Instance))};
    SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
    SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
  }
  SDL_EndGPURenderPass(pass);
  queue_.clear();
  return true;
}

}  // namespace imperivm::platform
