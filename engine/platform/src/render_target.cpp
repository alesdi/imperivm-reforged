#include "imperivm/platform/render_target.hpp"

#include <cstdio>
#include <cstring>

#include <SDL3/SDL.h>

namespace imperivm::platform {
namespace {

constexpr SDL_GPUTextureFormat kFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

// -- PNG ------------------------------------------------------------------

std::uint32_t crc32_of(const std::uint8_t* data, std::size_t size, std::uint32_t crc = 0) {
  static std::uint32_t table[256];
  static bool built = false;
  if (!built) {
    for (std::uint32_t n = 0; n < 256; ++n) {
      std::uint32_t c = n;
      for (int k = 0; k < 8; ++k) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[n] = c;
    }
    built = true;
  }
  crc = ~crc;
  for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value >> 24));
  out.push_back(static_cast<std::uint8_t>(value >> 16));
  out.push_back(static_cast<std::uint8_t>(value >> 8));
  out.push_back(static_cast<std::uint8_t>(value));
}

void append_chunk(std::vector<std::uint8_t>& out, const char tag[4],
                  const std::vector<std::uint8_t>& payload) {
  append_u32(out, static_cast<std::uint32_t>(payload.size()));
  const std::size_t start = out.size();
  out.insert(out.end(), tag, tag + 4);
  out.insert(out.end(), payload.begin(), payload.end());
  append_u32(out, crc32_of(out.data() + start, out.size() - start));
}

}  // namespace

std::uint32_t render_target_format() noexcept { return static_cast<std::uint32_t>(kFormat); }

RenderTarget::~RenderTarget() { destroy(); }

bool RenderTarget::ensure(SDL_GPUDevice* device, std::uint32_t width, std::uint32_t height) {
  if (device == nullptr || width == 0 || height == 0) return false;
  if (texture_ != nullptr && device_ == device && width_ == width && height_ == height) {
    return true;
  }
  destroy();
  device_ = device;

  SDL_GPUTextureCreateInfo info{};
  info.type = SDL_GPU_TEXTURETYPE_2D;
  info.format = kFormat;
  // SAMPLER so it can be the source of the blit and of a download.
  info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
  info.width = width;
  info.height = height;
  info.layer_count_or_depth = 1;
  info.num_levels = 1;

  texture_ = SDL_CreateGPUTexture(device_, &info);
  if (texture_ == nullptr) return false;
  width_ = width;
  height_ = height;
  return true;
}

void RenderTarget::destroy() {
  if (device_ != nullptr && texture_ != nullptr) SDL_ReleaseGPUTexture(device_, texture_);
  texture_ = nullptr;
  device_ = nullptr;
  width_ = 0;
  height_ = 0;
}

void RenderTarget::blit_to(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* destination,
                           std::uint32_t destination_width,
                           std::uint32_t destination_height) const {
  if (commands == nullptr || texture_ == nullptr || destination == nullptr) return;

  SDL_GPUBlitInfo info{};
  info.source.texture = texture_;
  info.source.w = width_;
  info.source.h = height_;
  info.destination.texture = destination;
  info.destination.w = destination_width;
  info.destination.h = destination_height;
  info.load_op = SDL_GPU_LOADOP_DONT_CARE;
  info.filter = SDL_GPU_FILTER_NEAREST;
  SDL_BlitGPUTexture(commands, &info);
}

bool RenderTarget::download(std::vector<std::uint8_t>& rgba, std::string* error) const {
  const auto fail = [&](const char* what) {
    if (error != nullptr) *error = std::string(what) + ": " + SDL_GetError();
    return false;
  };
  if (device_ == nullptr || texture_ == nullptr) {
    if (error != nullptr) *error = "no render target";
    return false;
  }

  const std::size_t bytes = static_cast<std::size_t>(width_) * height_ * 4;

  SDL_GPUTransferBufferCreateInfo info{};
  info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
  info.size = static_cast<Uint32>(bytes);
  SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &info);
  if (transfer == nullptr) return fail("SDL_CreateGPUTransferBuffer");

  SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_);
  if (commands == nullptr) {
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
    return fail("SDL_AcquireGPUCommandBuffer");
  }

  SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(commands);
  SDL_GPUTextureRegion source{};
  source.texture = texture_;
  source.w = width_;
  source.h = height_;
  source.d = 1;
  SDL_GPUTextureTransferInfo destination{};
  destination.transfer_buffer = transfer;
  destination.pixels_per_row = width_;
  destination.rows_per_layer = height_;
  SDL_DownloadFromGPUTexture(pass, &source, &destination);
  SDL_EndGPUCopyPass(pass);

  SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
  if (fence == nullptr) {
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
    return fail("SDL_SubmitGPUCommandBufferAndAcquireFence");
  }
  SDL_WaitForGPUFences(device_, true, &fence, 1);
  SDL_ReleaseGPUFence(device_, fence);

  const void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
  if (mapped == nullptr) {
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
    return fail("SDL_MapGPUTransferBuffer");
  }
  rgba.resize(bytes);
  std::memcpy(rgba.data(), mapped, bytes);
  SDL_UnmapGPUTransferBuffer(device_, transfer);
  SDL_ReleaseGPUTransferBuffer(device_, transfer);
  return true;
}

bool save_png(const char* path, const std::uint8_t* rgba, std::uint32_t width,
              std::uint32_t height, std::string* error) {
  if (path == nullptr || rgba == nullptr || width == 0 || height == 0) {
    if (error != nullptr) *error = "nothing to write";
    return false;
  }

  // Filter byte 0 (None) in front of every scanline; that is the whole of the
  // "compression" this writer does.
  std::vector<std::uint8_t> raw;
  raw.reserve(static_cast<std::size_t>(height) * (1 + width * 4));
  for (std::uint32_t y = 0; y < height; ++y) {
    raw.push_back(0);
    const std::uint8_t* row = rgba + static_cast<std::size_t>(y) * width * 4;
    raw.insert(raw.end(), row, row + static_cast<std::size_t>(width) * 4);
  }

  // zlib: a 0x78 0x01 header, stored deflate blocks of at most 65,535 bytes,
  // and an Adler-32 of the raw data.
  std::vector<std::uint8_t> zlib{0x78, 0x01};
  for (std::size_t at = 0; at < raw.size();) {
    const std::size_t take = std::min<std::size_t>(raw.size() - at, 65535);
    const bool last = at + take == raw.size();
    zlib.push_back(last ? 1 : 0);
    zlib.push_back(static_cast<std::uint8_t>(take));
    zlib.push_back(static_cast<std::uint8_t>(take >> 8));
    zlib.push_back(static_cast<std::uint8_t>(~take));
    zlib.push_back(static_cast<std::uint8_t>(~take >> 8));
    zlib.insert(zlib.end(), raw.begin() + static_cast<long>(at),
                raw.begin() + static_cast<long>(at + take));
    at += take;
  }
  std::uint32_t a = 1;
  std::uint32_t b = 0;
  for (std::uint8_t byte : raw) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  append_u32(zlib, b << 16 | a);

  std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<std::uint8_t> ihdr;
  append_u32(ihdr, width);
  append_u32(ihdr, height);
  ihdr.push_back(8);  // bit depth
  ihdr.push_back(6);  // colour type: RGBA
  ihdr.push_back(0);  // deflate
  ihdr.push_back(0);  // adaptive filtering
  ihdr.push_back(0);  // no interlace
  append_chunk(png, "IHDR", ihdr);
  append_chunk(png, "IDAT", zlib);
  append_chunk(png, "IEND", {});

  SDL_IOStream* out = SDL_IOFromFile(path, "wb");
  if (out == nullptr) {
    if (error != nullptr) *error = std::string("cannot write ") + path + ": " + SDL_GetError();
    return false;
  }
  const bool ok = SDL_WriteIO(out, png.data(), png.size()) == png.size();
  SDL_CloseIO(out);
  if (!ok && error != nullptr) *error = std::string("short write to ") + path;
  return ok;
}

}  // namespace imperivm::platform
