#include "imperivm/core/formats/grid.hpp"

#include <algorithm>

#include "imperivm/core/formats/byte_reader.hpp"

namespace imperivm::core {

namespace {

void write_u32le(std::span<std::byte> out, std::size_t offset, std::uint32_t value) noexcept {
  for (int shift = 0; shift < 32; shift += 8) {
    out[offset++] = static_cast<std::byte>((value >> shift) & 0xFFu);
  }
}

/// What a grid may be, in one place, so that `OwnedGrid::create` cannot make
/// a grid `Grid::parse` would refuse.
Status check_geometry(std::uint32_t cell_size, std::uint32_t bits_per_cell,
                      std::uint32_t extent_x, std::uint32_t extent_y) noexcept {
  if (cell_size == 0) return FormatError::malformed;
  if (extent_x % cell_size != 0 || extent_y % cell_size != 0) return FormatError::malformed;
  // Sub-byte cells are packed without crossing a row boundary, so a row has to
  // be a whole number of bytes. Retail data only ships 1, 4, 8 and 16.
  switch (bits_per_cell) {
    case 1:
    case 2:
    case 4:
      if (((extent_x / cell_size) * bits_per_cell) % 8 != 0) return FormatError::malformed;
      return {};
    case 8:
    case 16:
    case 32:
      return {};
    default:
      return FormatError::unsupported;
  }
}

/// The header alone, from its four fields.
void write_header(std::span<std::byte> out, std::uint32_t cell_size, std::uint32_t bits_per_cell,
                  std::uint32_t extent_x, std::uint32_t extent_y) noexcept {
  for (std::size_t i = 0; i < kGridMagic.size(); ++i) {
    out[i] = static_cast<std::byte>(kGridMagic[i]);
  }
  // The extents go out in world units, as the header carries them -- not the
  // width and height, which are derived and would only agree at cell size 1.
  write_u32le(out, 4, cell_size);
  write_u32le(out, 8, bits_per_cell);
  write_u32le(out, 12, extent_x);
  write_u32le(out, 16, extent_y);
}

}  // namespace

Result<Grid> Grid::parse_header(std::span<const std::byte> data) {
  if (!has_magic(data, kGridMagic)) return FormatError::bad_magic;
  if (data.size() < kGridHeaderSize) return FormatError::truncated;

  Grid grid;
  grid.cell_size_ = read_u32le(data, 4);
  grid.bits_per_cell_ = read_u32le(data, 8);
  grid.extent_x_ = read_u32le(data, 12);
  grid.extent_y_ = read_u32le(data, 16);
  if (const Status status = check_geometry(grid.cell_size_, grid.bits_per_cell_, grid.extent_x_,
                                           grid.extent_y_);
      !status) {
    return status.error();
  }

  const std::uint64_t body = static_cast<std::uint64_t>(grid.stride()) * grid.height();
  if (body > data.size() - kGridHeaderSize) return FormatError::truncated;
  grid.cells_ = data.subspan(kGridHeaderSize, static_cast<std::size_t>(body));
  return grid;
}

Result<Grid> Grid::parse(std::span<const std::byte> data) { return parse_header(data); }

Result<Grid> Grid::parse_mutable(std::span<std::byte> data) {
  Result<Grid> parsed = parse_header(data);
  if (!parsed) return parsed;
  // The same bytes the const view covers, writable. `parse_header` has
  // already established that the body lies inside `data`.
  parsed->mutable_cells_ = data.subspan(kGridHeaderSize, parsed->cells_.size());
  return parsed;
}

std::uint32_t Grid::cell(std::uint32_t x, std::uint32_t y) const noexcept {
  if (x >= width() || y >= height()) return 0;
  const std::size_t row = static_cast<std::size_t>(y) * stride();

  switch (bits_per_cell_) {
    case 1: {
      // Least significant bit first: cell x lives in bit (x % 8) of byte x / 8.
      const std::uint8_t byte = read_u8(cells_, row + (x >> 3));
      return (byte >> (x & 7)) & 1u;
    }
    case 2: {
      const std::uint8_t byte = read_u8(cells_, row + (x >> 2));
      return (byte >> (2 * (x & 3))) & 0x3u;
    }
    case 4: {
      const std::uint8_t byte = read_u8(cells_, row + (x >> 1));
      return (byte >> (4 * (x & 1))) & 0xFu;
    }
    case 8:
      return read_u8(cells_, row + x);
    case 16:
      return read_u16le(cells_, row + 2 * x);
    case 32:
      return read_u32le(cells_, row + 4 * x);
    default:
      return 0;
  }
}

std::uint32_t Grid::max_cell_value() const noexcept {
  if (bits_per_cell_ >= 32) return 0xFFFFFFFFu;
  return (1u << bits_per_cell_) - 1u;
}

Status Grid::set_cell(std::uint32_t x, std::uint32_t y, std::uint32_t value) noexcept {
  if (!writable()) return FormatError::unsupported;
  if (x >= width() || y >= height()) return FormatError::out_of_range;
  if (value > max_cell_value()) return FormatError::buffer_too_small;
  const std::size_t row = static_cast<std::size_t>(y) * stride();

  // Sub-byte depths: clear the cell's bits, then or the value in at the same
  // shift `cell` reads it from. Least significant first, like the reader.
  const auto pack = [&](std::size_t index, unsigned shift, std::uint32_t mask) noexcept {
    std::uint8_t byte = static_cast<std::uint8_t>(mutable_cells_[index]);
    byte = static_cast<std::uint8_t>(byte & ~(mask << shift));
    byte = static_cast<std::uint8_t>(byte | ((value & mask) << shift));
    mutable_cells_[index] = static_cast<std::byte>(byte);
  };

  switch (bits_per_cell_) {
    case 1:
      pack(row + (x >> 3), x & 7, 0x1u);
      return {};
    case 2:
      pack(row + (x >> 2), 2 * (x & 3), 0x3u);
      return {};
    case 4:
      pack(row + (x >> 1), 4 * (x & 1), 0xFu);
      return {};
    case 8:
      mutable_cells_[row + x] = static_cast<std::byte>(value);
      return {};
    case 16:
      mutable_cells_[row + 2 * x] = static_cast<std::byte>(value & 0xFFu);
      mutable_cells_[row + 2 * x + 1] = static_cast<std::byte>((value >> 8) & 0xFFu);
      return {};
    case 32:
      write_u32le(mutable_cells_, row + 4 * x, value);
      return {};
    default:
      return FormatError::unsupported;
  }
}

Status Grid::fill(std::uint32_t value) noexcept {
  if (!writable()) return FormatError::unsupported;
  if (value > max_cell_value()) return FormatError::buffer_too_small;
  for (std::uint32_t y = 0; y < height(); ++y) {
    for (std::uint32_t x = 0; x < width(); ++x) {
      if (const Status status = set_cell(x, y, value); !status) return status;
    }
  }
  return {};
}

std::uint32_t Grid::count_set() const noexcept {
  std::uint32_t set = 0;
  for (std::uint32_t y = 0; y < height(); ++y) {
    for (std::uint32_t x = 0; x < width(); ++x) {
      if (cell(x, y) != 0) ++set;
    }
  }
  return set;
}

Status Grid::validate(std::size_t file_size) const noexcept {
  if (file_size != expected_size()) return FormatError::malformed;
  return {};
}

// --------------------------------------------------------------------------
// writing
// --------------------------------------------------------------------------

std::vector<std::byte> write_grid(const Grid& grid) {
  std::vector<std::byte> out(kGridHeaderSize + grid.cells().size());
  write_header(out, grid.cell_size(), grid.bits_per_cell(), grid.extent_x(), grid.extent_y());
  std::copy(grid.cells().begin(), grid.cells().end(), out.begin() + kGridHeaderSize);
  return out;
}

// --------------------------------------------------------------------------
// OwnedGrid
// --------------------------------------------------------------------------

Result<OwnedGrid> OwnedGrid::create(std::uint32_t cell_size, std::uint32_t bits_per_cell,
                                    std::uint32_t extent_x, std::uint32_t extent_y) {
  if (const Status status = check_geometry(cell_size, bits_per_cell, extent_x, extent_y);
      !status) {
    return status.error();
  }
  // The body from the header's own arithmetic, in 64 bits: the fields are
  // 32-bit and a hostile pair of extents could overflow the product.
  const std::uint64_t width = extent_x / cell_size;
  const std::uint64_t height = extent_y / cell_size;
  const std::uint64_t row_bits = width * bits_per_cell;
  const std::uint64_t stride = (row_bits + 7) / 8;
  const std::uint64_t body = stride * height;
  // `Grid::stride` does the same sum in 32 bits, so the row must fit there
  // too, or the grid made here would not be the grid `parse_mutable` sees.
  if (row_bits + 7 > 0xFFFFFFFFull || body > 0xFFFFFFFFull) return FormatError::out_of_range;

  OwnedGrid out;
  out.bytes_.resize(kGridHeaderSize + static_cast<std::size_t>(body));
  write_header(out.bytes_, cell_size, bits_per_cell, extent_x, extent_y);
  Result<Grid> grid = Grid::parse_mutable(out.bytes_);
  if (!grid) return grid.error();
  out.grid_ = grid.value();
  return out;
}

Result<OwnedGrid> OwnedGrid::copy(const Grid& source) {
  OwnedGrid out;
  out.bytes_ = write_grid(source);
  Result<Grid> grid = Grid::parse_mutable(out.bytes_);
  if (!grid) return grid.error();
  out.grid_ = grid.value();
  return out;
}

OwnedGrid::OwnedGrid(OwnedGrid&& other) noexcept
    : bytes_(std::move(other.bytes_)), grid_(other.grid_) {
  // The vector's buffer came with it, so `grid_` still points at the right
  // bytes; the source is left empty rather than over storage it gave away.
  other.grid_ = Grid{};
}

OwnedGrid& OwnedGrid::operator=(OwnedGrid&& other) noexcept {
  if (this != &other) {
    bytes_ = std::move(other.bytes_);
    grid_ = other.grid_;
    other.grid_ = Grid{};
  }
  return *this;
}

}  // namespace imperivm::core
