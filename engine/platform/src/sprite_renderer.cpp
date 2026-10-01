#include "imperivm/platform/sprite_renderer.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include <SDL3/SDL.h>

#include "imperivm/platform/window.hpp"
#include "generated/sprite_shaders.inc"

namespace imperivm::platform {
namespace {

/// Round up to a texture-upload-friendly boundary. Backends differ on what
/// they require of a transfer buffer offset; 256 satisfies all of them and
/// costs a few hundred bytes per frame uploaded.
std::size_t align_up(std::size_t value, std::size_t to) {
  return (value + to - 1) / to * to;
}

SDL_GPUShader* create_shader(SDL_GPUDevice* device, SDL_GPUShaderStage stage,
                             std::uint32_t samplers, std::uint32_t uniform_buffers) {
  const SDL_GPUShaderFormat available = SDL_GetGPUShaderFormats(device);

  SDL_GPUShaderCreateInfo info{};
  info.stage = stage;
  info.num_samplers = samplers;
  info.num_uniform_buffers = uniform_buffers;

  if ((available & SDL_GPU_SHADERFORMAT_SPIRV) != 0) {
    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    info.entrypoint = "main";
    if (stage == SDL_GPU_SHADERSTAGE_VERTEX) {
      info.code = reinterpret_cast<const Uint8*>(shaders::kSpriteVertexSpirv);
      info.code_size = sizeof(shaders::kSpriteVertexSpirv);
    } else {
      info.code = reinterpret_cast<const Uint8*>(shaders::kSpriteFragmentSpirv);
      info.code_size = sizeof(shaders::kSpriteFragmentSpirv);
    }
  } else if ((available & SDL_GPU_SHADERFORMAT_MSL) != 0) {
    info.format = SDL_GPU_SHADERFORMAT_MSL;
    info.entrypoint =
        stage == SDL_GPU_SHADERSTAGE_VERTEX ? "sprite_vertex" : "sprite_fragment";
    info.code = reinterpret_cast<const Uint8*>(shaders::kSpriteMsl);
    // MSL is source text, so the length includes the terminator SDL expects.
    info.code_size = sizeof(shaders::kSpriteMsl);
  } else {
    SDL_SetError(
        "no shader format this build ships (SPIR-V, MSL) is accepted by the %s backend",
        SDL_GetGPUDeviceDriver(device));
    return nullptr;
  }

  return SDL_CreateGPUShader(device, &info);
}

}  // namespace

SpriteRenderer::~SpriteRenderer() { destroy(); }

bool SpriteRenderer::fail(std::string* error, const char* what) {
  if (error != nullptr) *error = std::string(what) + ": " + SDL_GetError();
  return false;
}

bool SpriteRenderer::create(Window& window, std::string* error) {
  return create(window.device(), window.swapchain_format(), error);
}

bool SpriteRenderer::create(SDL_GPUDevice* device, std::uint32_t target_format,
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

  // Nearest everywhere. An index texture must never be filtered: interpolating
  // index 12 and index 200 gives index 106, which is an unrelated colour rather
  // than a blend of two. The palette lookup is nearest for the same reason.
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

  SDL_GPUTextureCreateInfo palette{};
  palette.type = SDL_GPU_TEXTURETYPE_2D;
  palette.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  palette.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
  palette.width = kPaletteEntries;
  palette.height = kPaletteRows;
  palette.layer_count_or_depth = 1;
  palette.num_levels = 1;
  palette_texture_ = SDL_CreateGPUTexture(device_, &palette);
  if (palette_texture_ == nullptr) {
    (void)fail(error, "SDL_CreateGPUTexture (palette)");
    destroy();
    return false;
  }

  if (!add_page(error)) {
    destroy();
    return false;
  }

  // The primitives' art: one opaque texel and the alpha ramp, staged now so
  // the first `commit_uploads` makes them resident. They cost one texel of
  // the first page and one palette row.
  std::vector<std::uint32_t> ramp(kPaletteEntries);
  for (std::uint32_t i = 0; i < kPaletteEntries; ++i) ramp[i] = (i << 24) | 0x00FFFFFFu;
  alpha_ramp_ = upload_palette(ramp);
  const std::uint8_t opaque = 0xFF;
  solid_ = upload_indices(std::span<const std::uint8_t>(&opaque, 1), 1, 1);
  return true;
}

void SpriteRenderer::fill_rect(float x, float y, float width, float height, const Rgba& colour) {
  if (width <= 0.0F || height <= 0.0F) return;
  draw_scaled(solid_, x, y, width, height, alpha_ramp_, colour);
}

void SpriteRenderer::draw_mask(const AtlasRegion& region, float x, float y, const Rgba& colour) {
  draw(region, x, y, alpha_ramp_, colour);
}

bool SpriteRenderer::build_pipeline(std::string* error) {
  SDL_GPUShader* vertex =
      create_shader(device_, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
  if (vertex == nullptr) return fail(error, "vertex shader");

  SDL_GPUShader* fragment =
      create_shader(device_, SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 0);
  if (fragment == nullptr) {
    SDL_ReleaseGPUShader(device_, vertex);
    return fail(error, "fragment shader");
  }

  // One instance per sprite. There is no per-vertex stream at all: the quad
  // corners come from the vertex index, so a sprite costs 52 bytes and no
  // index buffer.
  SDL_GPUVertexBufferDescription buffers[1]{};
  buffers[0].slot = 0;
  buffers[0].pitch = sizeof(Instance);
  buffers[0].input_rate = SDL_GPU_VERTEXINPUTRATE_INSTANCE;
  buffers[0].instance_step_rate = 0;

  SDL_GPUVertexAttribute attributes[4]{};
  attributes[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(Instance, dst)};
  attributes[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(Instance, src)};
  attributes[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(Instance, modulate)};
  attributes[3] = {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(Instance, palette)};

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

  // The pipeline holds its own reference; ours is done either way.
  SDL_ReleaseGPUShader(device_, vertex);
  SDL_ReleaseGPUShader(device_, fragment);

  if (pipeline_ == nullptr) return fail(error, "SDL_CreateGPUGraphicsPipeline");
  return true;
}

bool SpriteRenderer::add_page(std::string* error) {
  SDL_GPUTextureCreateInfo info{};
  info.type = SDL_GPU_TEXTURETYPE_2D;
  info.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
  info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
  info.width = kPageSize;
  info.height = kPageSize;
  info.layer_count_or_depth = 1;
  info.num_levels = 1;

  Page page;
  page.texture = SDL_CreateGPUTexture(device_, &info);
  if (page.texture == nullptr) return fail(error, "SDL_CreateGPUTexture (atlas page)");
  pages_.push_back(page);
  atlas_bytes_ += static_cast<std::size_t>(kPageSize) * kPageSize;
  return true;
}

void SpriteRenderer::destroy() {
  if (device_ != nullptr) {
    for (Page& page : pages_) {
      if (page.texture != nullptr) SDL_ReleaseGPUTexture(device_, page.texture);
    }
    if (palette_texture_ != nullptr) SDL_ReleaseGPUTexture(device_, palette_texture_);
    if (sampler_ != nullptr) SDL_ReleaseGPUSampler(device_, sampler_);
    if (instance_buffer_ != nullptr) SDL_ReleaseGPUBuffer(device_, instance_buffer_);
    if (pipeline_ != nullptr) SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
  }
  pages_.clear();
  palette_texture_ = nullptr;
  sampler_ = nullptr;
  instance_buffer_ = nullptr;
  instance_capacity_ = 0;
  pipeline_ = nullptr;
  pending_textures_.clear();
  staging_.clear();
  pending_palettes_.clear();
  palette_rows_used_ = 0;
  palette_rows_uploaded_ = 0;
  palette_rows_shared_ = 0;
  palette_rows_refused_ = 0;
  palette_contents_.clear();
  palette_by_hash_.clear();
  atlas_bytes_ = 0;
  instances_.clear();
  instance_pages_.clear();
  solid_ = AtlasRegion{};
  alpha_ramp_ = PaletteRow{};
  device_ = nullptr;
}

std::size_t SpriteRenderer::atlas_bytes() const noexcept { return atlas_bytes_; }

// -- uploads --------------------------------------------------------------

AtlasRegion SpriteRenderer::upload_indices(std::span<const std::uint8_t> indices,
                                           std::uint32_t width, std::uint32_t height) {
  AtlasRegion region;
  if (width == 0 || height == 0) return region;
  if (width > kPageSize || height > kPageSize) {
    SDL_Log("sprite: %ux%u does not fit a %ux%u atlas page", width, height, kPageSize,
            kPageSize);
    return region;
  }
  if (indices.size() < static_cast<std::size_t>(width) * height) return region;

  // One pixel of padding so that a neighbouring sprite's indices can never be
  // reached by a sampling coordinate that lands exactly on a shared edge.
  constexpr std::uint32_t kPad = 1;

  for (std::size_t i = 0; i < pages_.size(); ++i) {
    Page& page = pages_[i];
    if (page.pen_x + width > kPageSize) {  // close the shelf, open the next
      page.shelf_y += page.shelf_height;
      page.shelf_height = 0;
      page.pen_x = 0;
    }
    if (page.shelf_y + std::max(page.shelf_height, height) > kPageSize) continue;

    region.page = static_cast<std::uint32_t>(i);
    region.x = static_cast<std::uint16_t>(page.pen_x);
    region.y = static_cast<std::uint16_t>(page.shelf_y);
    region.width = static_cast<std::uint16_t>(width);
    region.height = static_cast<std::uint16_t>(height);

    page.pen_x += width + kPad;
    page.shelf_height = std::max(page.shelf_height, height + kPad);
    break;
  }

  if (!region.valid()) {
    if (!add_page(nullptr)) return AtlasRegion{};
    return upload_indices(indices, width, height);
  }

  PendingTexture pending;
  pending.page = region.page;
  pending.x = region.x;
  pending.y = region.y;
  pending.width = width;
  pending.height = height;
  pending.staged_at = align_up(staging_.size(), 256);

  staging_.resize(pending.staged_at + static_cast<std::size_t>(width) * height);
  std::memcpy(staging_.data() + pending.staged_at, indices.data(),
              static_cast<std::size_t>(width) * height);
  pending_textures_.push_back(pending);
  return region;
}

PaletteRow SpriteRenderer::upload_palette(std::span<const std::uint32_t> rgba) {
  PaletteRow handle;

  // The row as it would be stored: exactly `kPaletteEntries`, the tail zero.
  std::array<std::uint32_t, kPaletteEntries> entries{};
  const std::size_t count = std::min<std::size_t>(rgba.size(), kPaletteEntries);
  std::copy_n(rgba.begin(), count, entries.begin());

  // FNV-1a over the entries -- only a bucket; a candidate is compared in full.
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (const std::uint32_t entry : entries) {
    hash ^= entry;
    hash *= 0x100000001b3ULL;
  }
  const auto [first, last] = palette_by_hash_.equal_range(hash);
  for (auto it = first; it != last; ++it) {
    const std::uint32_t* stored =
        palette_contents_.data() + static_cast<std::size_t>(it->second) * kPaletteEntries;
    if (std::equal(entries.begin(), entries.end(), stored)) {
      handle.row = it->second;
      handle.valid = true;
      ++palette_rows_shared_;
      return handle;
    }
  }

  if (palette_rows_used_ >= kPaletteRows) {
    // Once, not once a request: a full texture is asked again every frame.
    if (palette_rows_refused_++ == 0) {
      SDL_Log("sprite: palette lookup texture is full (%u rows); art prepared from here on "
              "has no palette and is not drawn",
              kPaletteRows);
    }
    return handle;
  }

  pending_palettes_.insert(pending_palettes_.end(), entries.begin(), entries.end());
  palette_contents_.insert(palette_contents_.end(), entries.begin(), entries.end());
  handle.row = static_cast<std::uint16_t>(palette_rows_used_);
  handle.valid = true;
  palette_by_hash_.emplace(hash, handle.row);
  ++palette_rows_used_;
  return handle;
}

bool SpriteRenderer::commit_uploads(std::string* error) {
  if (pending_textures_.empty() && pending_palettes_.empty()) return true;
  if (device_ == nullptr) return false;

  const std::size_t palette_bytes = pending_palettes_.size() * sizeof(std::uint32_t);
  const std::size_t palette_at = align_up(staging_.size(), 256);
  const std::size_t total = palette_at + palette_bytes;

  SDL_GPUTransferBufferCreateInfo transfer_info{};
  transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
  transfer_info.size = static_cast<Uint32>(total);
  SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &transfer_info);
  if (transfer == nullptr) return fail(error, "SDL_CreateGPUTransferBuffer");

  void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
  if (mapped == nullptr) {
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
    return fail(error, "SDL_MapGPUTransferBuffer");
  }
  auto* bytes = static_cast<std::uint8_t*>(mapped);
  if (!staging_.empty()) std::memcpy(bytes, staging_.data(), staging_.size());
  if (palette_bytes != 0) {
    std::memcpy(bytes + palette_at, pending_palettes_.data(), palette_bytes);
  }
  SDL_UnmapGPUTransferBuffer(device_, transfer);

  SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_);
  if (commands == nullptr) {
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
    return fail(error, "SDL_AcquireGPUCommandBuffer");
  }
  SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(commands);

  for (const PendingTexture& pending : pending_textures_) {
    SDL_GPUTextureTransferInfo source{};
    source.transfer_buffer = transfer;
    source.offset = static_cast<Uint32>(pending.staged_at);
    source.pixels_per_row = pending.width;
    source.rows_per_layer = pending.height;

    SDL_GPUTextureRegion destination{};
    destination.texture = pages_[pending.page].texture;
    destination.x = pending.x;
    destination.y = pending.y;
    destination.w = pending.width;
    destination.h = pending.height;
    destination.d = 1;

    SDL_UploadToGPUTexture(pass, &source, &destination, false);
  }

  if (palette_bytes != 0) {
    const std::uint32_t rows =
        static_cast<std::uint32_t>(pending_palettes_.size() / kPaletteEntries);
    SDL_GPUTextureTransferInfo source{};
    source.transfer_buffer = transfer;
    source.offset = static_cast<Uint32>(palette_at);
    source.pixels_per_row = kPaletteEntries;
    source.rows_per_layer = rows;

    SDL_GPUTextureRegion destination{};
    destination.texture = palette_texture_;
    destination.x = 0;
    destination.y = palette_rows_uploaded_;
    destination.w = kPaletteEntries;
    destination.h = rows;
    destination.d = 1;

    SDL_UploadToGPUTexture(pass, &source, &destination, false);
    palette_rows_uploaded_ += rows;
  }

  SDL_EndGPUCopyPass(pass);

  // Load-time upload: wait, so the caller can free its source bytes and draw
  // on the very next frame without tracking a fence.
  SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
  if (fence != nullptr) {
    SDL_WaitForGPUFences(device_, true, &fence, 1);
    SDL_ReleaseGPUFence(device_, fence);
  }
  SDL_ReleaseGPUTransferBuffer(device_, transfer);

  pending_textures_.clear();
  staging_.clear();
  staging_.shrink_to_fit();
  pending_palettes_.clear();
  pending_palettes_.shrink_to_fit();
  return true;
}

// -- drawing --------------------------------------------------------------

void SpriteRenderer::begin(std::uint32_t width, std::uint32_t height) {
  viewport_width_ = width;
  viewport_height_ = height;
  instances_.clear();
  instance_pages_.clear();
}

void SpriteRenderer::draw(const AtlasRegion& region, float x, float y, PaletteRow palette,
                          const Rgba& modulate) {
  draw_scaled(region, x, y, static_cast<float>(region.width),
              static_cast<float>(region.height), palette, modulate);
}

void SpriteRenderer::draw_scaled(const AtlasRegion& region, float x, float y, float width,
                                 float height, PaletteRow palette, const Rgba& modulate) {
  if (!region.valid() || !palette.valid) return;

  const float page = static_cast<float>(kPageSize);
  Instance instance{};
  instance.dst[0] = x;
  instance.dst[1] = y;
  instance.dst[2] = width;
  instance.dst[3] = height;
  instance.src[0] = static_cast<float>(region.x) / page;
  instance.src[1] = static_cast<float>(region.y) / page;
  instance.src[2] = static_cast<float>(region.x + region.width) / page;
  instance.src[3] = static_cast<float>(region.y + region.height) / page;
  instance.modulate[0] = modulate.red;
  instance.modulate[1] = modulate.green;
  instance.modulate[2] = modulate.blue;
  instance.modulate[3] = modulate.alpha;
  // The row centre, so a nearest fetch cannot drift into a neighbour.
  instance.palette = (static_cast<float>(palette.row) + 0.5F) / kPaletteRows;

  instances_.push_back(instance);
  instance_pages_.push_back(region.page);
}

bool SpriteRenderer::ensure_instance_capacity(std::size_t instances) {
  if (instances <= instance_capacity_) return true;
  if (instance_buffer_ != nullptr) SDL_ReleaseGPUBuffer(device_, instance_buffer_);

  std::size_t capacity = std::max<std::size_t>(instance_capacity_ * 2, 1024);
  while (capacity < instances) capacity *= 2;

  SDL_GPUBufferCreateInfo info{};
  info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
  info.size = static_cast<Uint32>(capacity * sizeof(Instance));
  instance_buffer_ = SDL_CreateGPUBuffer(device_, &info);
  instance_capacity_ = instance_buffer_ != nullptr ? capacity : 0;
  return instance_buffer_ != nullptr;
}

bool SpriteRenderer::render(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target,
                            const Rgba* clear) {
  if (device_ == nullptr || pipeline_ == nullptr || commands == nullptr ||
      target == nullptr) {
    return false;
  }

  // Draw order is queue order, full stop. Grouping by atlas page would cost
  // fewer draw calls and would be wrong: a scene sorted into depth order by the
  // ZBINS rule has no depth buffer behind it, so reordering two sprites that
  // happen to live on different pages puts a tree in front of the building it
  // stands behind. The loop below still coalesces every *consecutive* run on
  // one page into a single draw, which is most of the win and costs nothing.
  std::vector<std::uint32_t> order(instances_.size());
  for (std::uint32_t i = 0; i < order.size(); ++i) order[i] = i;

  if (!instances_.empty()) {
    if (!ensure_instance_capacity(instances_.size())) return false;

    SDL_GPUTransferBufferCreateInfo transfer_info{};
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = static_cast<Uint32>(instances_.size() * sizeof(Instance));
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &transfer_info);
    if (transfer == nullptr) return false;

    auto* mapped = static_cast<Instance*>(SDL_MapGPUTransferBuffer(device_, transfer, false));
    if (mapped == nullptr) {
      SDL_ReleaseGPUTransferBuffer(device_, transfer);
      return false;
    }
    for (std::size_t i = 0; i < order.size(); ++i) mapped[i] = instances_[order[i]];
    SDL_UnmapGPUTransferBuffer(device_, transfer);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUTransferBufferLocation source{transfer, 0};
    SDL_GPUBufferRegion destination{instance_buffer_, 0, transfer_info.size};
    SDL_UploadToGPUBuffer(copy, &source, &destination, true);
    SDL_EndGPUCopyPass(copy);
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
  }

  SDL_GPUColorTargetInfo color{};
  color.texture = target;
  color.load_op = clear != nullptr ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
  color.store_op = SDL_GPU_STOREOP_STORE;
  if (clear != nullptr) {
    color.clear_color = SDL_FColor{clear->red, clear->green, clear->blue, clear->alpha};
  }

  SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &color, 1, nullptr);
  if (pass == nullptr) return false;

  if (!instances_.empty() && viewport_width_ != 0 && viewport_height_ != 0) {
    SDL_BindGPUGraphicsPipeline(pass, pipeline_);

    struct {
      float to_clip[2];
      float pad[2];
    } uniform{{2.0F / static_cast<float>(viewport_width_),
               -2.0F / static_cast<float>(viewport_height_)},
              {0.0F, 0.0F}};
    SDL_PushGPUVertexUniformData(commands, 0, &uniform, sizeof(uniform));

    std::size_t first = 0;
    while (first < order.size()) {
      const std::uint32_t page = instance_pages_[order[first]];
      std::size_t last = first;
      while (last < order.size() && instance_pages_[order[last]] == page) ++last;

      SDL_GPUTextureSamplerBinding textures[2]{};
      textures[0] = {pages_[page].texture, sampler_};
      textures[1] = {palette_texture_, sampler_};
      SDL_BindGPUFragmentSamplers(pass, 0, textures, 2);

      SDL_GPUBufferBinding binding{instance_buffer_,
                                   static_cast<Uint32>(first * sizeof(Instance))};
      SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);

      SDL_DrawGPUPrimitives(pass, 6, static_cast<Uint32>(last - first), 0, 0);
      first = last;
    }
  }

  SDL_EndGPURenderPass(pass);
  return true;
}

}  // namespace imperivm::platform
