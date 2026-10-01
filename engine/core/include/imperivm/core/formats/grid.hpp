#pragma once

// Reader and writer for the HMMSYS GRID container (magic `DIRG`).
//
// Specification: docs/formats/pass.md
//
// One small container serves two jobs: the per-entity passability stamps
// (`.pass`, 1 bit per cell) and the map terrain layers stored inside a `.bfhp`
// (`Terrain.height.grid` and friends, 4, 8 or 16 bits per cell). Decoding the
// container gets you both.
//
// The grid is addressed in **world units**, not cells: the header carries a
// cell size and an extent, and width and height are derived. That is how the
// engine thinks about it, so the accessors below keep the distinction rather
// than collapsing everything to cell indices.
//
// Bits within a byte run **least significant first**. Getting this backwards
// does not produce obvious noise — it produces plausible-looking blobs with
// diagonal tearing — so it is worth stating twice. That is established by the
// 626 one-bit grids in the install. The four-bit nibble order is **not**:
// every four-bit byte the game ships has two equal nibbles, so low-nibble-first
// is an assumption by analogy, and `docs/formats/pass.md` says so.
//
// The grid does not copy the cell bytes; it views the caller's span. A grid
// made over a *mutable* span (`parse_mutable`, or `OwnedGrid`, which brings
// its own storage) can also be edited, and `write_grid` turns any grid back
// into the file image. The in-memory form is the on-disk form: the body is
// stored packed exactly as the file packs it, so the writer copies it and the
// only packing code is `cell` and `set_cell`, which are each other's inverse.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/result.hpp"

namespace imperivm::core {

/// The FourCC 'GRID' stored little-endian, which is how it reads on disk.
inline constexpr std::string_view kGridMagic = "DIRG";
inline constexpr std::size_t kGridHeaderSize = 20;

/// Every shipped `.pass` file uses these, and is therefore exactly 2068 bytes:
/// a hut and a shipyard both get the full 128x128 canvas.
inline constexpr std::uint32_t kPassCellSize = 16;
inline constexpr std::uint32_t kPassExtent = 2048;

class Grid {
 public:
  /// Parse the header and take a view of the cells. The span must hold at
  /// least the header plus `stride * height` bytes; use `validate` to insist
  /// it holds exactly that, which every retail grid does.
  static Result<Grid> parse(std::span<const std::byte> data);

  /// The same, over storage the caller lets the grid write: `set_cell` and
  /// `fill` write through to `data`, so whoever owns the bytes sees every
  /// edit and can hand them to `write_grid` -- or straight to a container --
  /// afterwards. `WorldMap` makes its layers this way over its own vectors.
  static Result<Grid> parse_mutable(std::span<std::byte> data);

  std::uint32_t cell_size() const noexcept { return cell_size_; }
  std::uint32_t bits_per_cell() const noexcept { return bits_per_cell_; }
  std::uint32_t extent_x() const noexcept { return extent_x_; }
  std::uint32_t extent_y() const noexcept { return extent_y_; }

  std::uint32_t width() const noexcept { return extent_x_ / cell_size_; }
  std::uint32_t height() const noexcept { return extent_y_ / cell_size_; }
  /// Bytes per stored row. Rows never carry padding beyond this. The rounding
  /// up is defensive: `parse` refuses a sub-byte depth whose row is not a whole
  /// number of bytes, so on any grid that exists the division is exact.
  std::uint32_t stride() const noexcept { return (width() * bits_per_cell_ + 7) / 8; }
  std::size_t expected_size() const noexcept {
    return kGridHeaderSize + static_cast<std::size_t>(stride()) * height();
  }

  std::span<const std::byte> cells() const noexcept { return cells_; }

  /// Value of one cell, or 0 outside the grid. Out of range reads are silent
  /// because callers stamp overlapping grids against each other and clipping
  /// at the edge is the useful behaviour, not an error.
  std::uint32_t cell(std::uint32_t x, std::uint32_t y) const noexcept;

  /// True when the cell is impassable. A set bit means blocked: masks are
  /// overwhelmingly zero and the set cells form a blob the size of the object.
  bool blocked(std::uint32_t x, std::uint32_t y) const noexcept {
    return bits_per_cell_ == 1 && cell(x, y) != 0;
  }

  /// World offset of a cell's top-left corner from the grid's anchor, which is
  /// the centre of the grid. The horizontal half of this is established; the
  /// vertical may carry a one-cell correction (see the specification).
  std::int32_t world_x_of(std::uint32_t x) const noexcept {
    return static_cast<std::int32_t>(x * cell_size_) - static_cast<std::int32_t>(extent_x_ / 2);
  }
  std::int32_t world_y_of(std::uint32_t y) const noexcept {
    return static_cast<std::int32_t>(y * cell_size_) - static_cast<std::int32_t>(extent_y_ / 2);
  }

  /// Number of non-zero cells: the object's footprint area, in cells.
  std::uint32_t count_set() const noexcept;

  /// Insist the file is exactly as long as the header describes.
  Status validate(std::size_t file_size) const noexcept;

  // -- editing -----------------------------------------------------------

  /// True when the grid was made over storage it may write.
  [[nodiscard]] bool writable() const noexcept { return !mutable_cells_.empty(); }

  /// The largest value a cell of this depth holds: 1, 3, 15, 255, 65535 or
  /// 2^32-1. The terrain layer ships at both 4 and 8 bits, so a caller that
  /// paints a terrain type must ask rather than assume.
  [[nodiscard]] std::uint32_t max_cell_value() const noexcept;

  /// Set one cell, packed exactly as `cell` unpacks it. Refuses rather than
  /// silently doing something else:
  ///
  ///   * `out_of_range` for a cell outside the grid -- unlike `cell`, which
  ///     clips, because an editor that paints off the edge should learn so;
  ///   * `buffer_too_small` for a value the depth cannot hold. **Not clamped**:
  ///     a terrain type of 16 written into the 4-bit layer of a blank template
  ///     would come back as 0 (`Ground 1`), which is a different terrain and
  ///     not an approximation of the one asked for;
  ///   * `unsupported` on a read-only view (`parse` rather than
  ///     `parse_mutable`), which is a caller holding the wrong handle.
  [[nodiscard]] Status set_cell(std::uint32_t x, std::uint32_t y, std::uint32_t value) noexcept;

  /// Every cell to `value`, with the same refusals as `set_cell`.
  [[nodiscard]] Status fill(std::uint32_t value) noexcept;

 private:
  static Result<Grid> parse_header(std::span<const std::byte> data);

  std::span<const std::byte> cells_{};
  /// The same bytes as `cells_` when writable, empty otherwise.
  std::span<std::byte> mutable_cells_{};
  std::uint32_t cell_size_ = 0;
  std::uint32_t bits_per_cell_ = 0;
  std::uint32_t extent_x_ = 0;
  std::uint32_t extent_y_ = 0;
};

/// The file image of a grid: the 20-byte header from the grid's own fields
/// followed by the cell rows as stored. `write_grid(*Grid::parse(bytes))` is
/// `bytes` for every one of the 772 grids in the retail install (the 597
/// passability masks, `DATA\RANDOM_MAP.TERRAIN.GRID`, and the six layers of
/// each of the 29 maps): `tests/test_corpus_grid.py` holds it there.
[[nodiscard]] std::vector<std::byte> write_grid(const Grid& grid);

/// A grid with storage of its own: what an editor makes from nothing, or the
/// copy it takes of a layer before painting into it.
///
/// Holds the whole file image -- header and cells -- so that `bytes()` is
/// exactly what a container entry should receive, with no writer in between.
/// Move-only: a `std::vector` move keeps its buffer, so the `Grid` over it
/// stays pointed at the right bytes, but a copy would leave the new grid
/// viewing the old storage. The move operations below reset the source's grid
/// rather than leaving it over a buffer it no longer owns.
class OwnedGrid {
 public:
  /// A zero-filled grid of the given geometry. Refuses, with the reader's own
  /// errors, any geometry the reader would refuse: a zero cell size, an extent
  /// that is not a whole number of cells, a row that is not a whole number of
  /// bytes, a depth other than 1, 2, 4, 8, 16 or 32.
  static Result<OwnedGrid> create(std::uint32_t cell_size, std::uint32_t bits_per_cell,
                                  std::uint32_t extent_x, std::uint32_t extent_y);

  /// A writable copy of an existing grid, cell for cell.
  static Result<OwnedGrid> copy(const Grid& source);

  OwnedGrid() = default;
  OwnedGrid(const OwnedGrid&) = delete;
  OwnedGrid& operator=(const OwnedGrid&) = delete;
  OwnedGrid(OwnedGrid&& other) noexcept;
  OwnedGrid& operator=(OwnedGrid&& other) noexcept;
  ~OwnedGrid() = default;

  [[nodiscard]] Grid& grid() noexcept { return grid_; }
  [[nodiscard]] const Grid& grid() const noexcept { return grid_; }

  /// The file image: header and cells, as `write_grid` would produce it.
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }

 private:
  std::vector<std::byte> bytes_;
  Grid grid_;
};

}  // namespace imperivm::core
