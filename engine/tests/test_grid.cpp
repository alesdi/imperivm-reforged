// GRID container tests.
//
// The bit order is the whole game here. One-bit cells are packed least
// significant first, and a reader that gets it backwards still produces
// coherent-looking blobs — so the tests below pin the exact bit, not just the
// shape of the output.

#include <span>
#include <utility>
#include <vector>

#include "builder.hpp"
#include "imperivm/core/formats/grid.hpp"
#include "test.hpp"

using namespace imperivm::core;
using imperivm::test::Builder;

namespace {

/// An 8x2 one-bit grid: cell size 16, so 128 by 32 world units.
Builder one_bit_grid(std::uint8_t row0, std::uint8_t row1) {
  Builder grid;
  grid.text("DIRG").u32(16).u32(1).u32(128).u32(32);
  grid.u8(row0).u8(row1);
  return grid;
}

}  // namespace

TEST(grid_parses_a_one_bit_mask) {
  const Builder data = one_bit_grid(0b00000101, 0b10000000);
  const auto grid = Grid::parse(data.span());
  CHECK(grid.ok());
  CHECK(grid->width() == 8);
  CHECK(grid->height() == 2);
  CHECK(grid->stride() == 1);
  CHECK(grid->expected_size() == 22);
  CHECK(grid->validate(data.size()).ok());
}

TEST(grid_packs_one_bit_cells_least_significant_first) {
  const Builder data = one_bit_grid(0b00000101, 0b10000000);
  const auto grid = Grid::parse(data.span());
  // Byte 0b00000101 means cells 0 and 2 are set. Read the other way round it
  // would mean cells 5 and 7, which is exactly the mistake that produces
  // plausible masks with diagonal tearing.
  CHECK(grid->cell(0, 0) == 1);
  CHECK(grid->cell(1, 0) == 0);
  CHECK(grid->cell(2, 0) == 1);
  CHECK(grid->cell(5, 0) == 0);
  CHECK(grid->cell(7, 0) == 0);
  CHECK(grid->cell(7, 1) == 1);
  CHECK(grid->count_set() == 3);
}

TEST(grid_reports_blocked_cells) {
  const Builder data = one_bit_grid(0b00000101, 0);
  const auto grid = Grid::parse(data.span());
  // A set bit means impassable; masks are overwhelmingly clear.
  CHECK(grid->blocked(0, 0));
  CHECK(!grid->blocked(1, 0));
}

TEST(grid_reads_outside_the_grid_as_clear) {
  const Builder data = one_bit_grid(0xFF, 0xFF);
  const auto grid = Grid::parse(data.span());
  // Callers stamp overlapping grids against each other, so clipping at the
  // edge is the useful behaviour rather than an error.
  CHECK(grid->cell(8, 0) == 0);
  CHECK(grid->cell(0, 2) == 0);
  CHECK(!grid->blocked(1000, 1000));
}

TEST(grid_parses_eight_bit_cells) {
  Builder data;
  data.text("DIRG").u32(64).u32(8).u32(256).u32(128);
  for (std::uint32_t i = 0; i < 8; ++i) data.u8(i * 3);

  const auto grid = Grid::parse(data.span());
  CHECK(grid.ok());
  CHECK(grid->width() == 4);
  CHECK(grid->height() == 2);
  CHECK(grid->stride() == 4);
  CHECK(grid->cell(0, 0) == 0);
  CHECK(grid->cell(3, 0) == 9);
  CHECK(grid->cell(1, 1) == 15);
  CHECK(grid->count_set() == 7);
  CHECK(grid->validate(data.size()).ok());
}

TEST(grid_addresses_cells_in_world_units) {
  const Builder data = one_bit_grid(0, 0);
  const auto grid = Grid::parse(data.span());
  // The anchor is the centre of the grid, so cell 0 starts half an extent to
  // the left of it.
  CHECK(grid->world_x_of(0) == -64);
  CHECK(grid->world_x_of(4) == 0);
  CHECK(grid->world_y_of(1) == 0);
}

TEST(grid_rejects_a_foreign_file) {
  Builder data;
  data.text("GRID").zeros(32);  // the FourCC the wrong way round
  CHECK(Grid::parse(data.span()).error() == FormatError::bad_magic);
}

TEST(grid_rejects_a_zero_cell_size) {
  Builder data;
  data.text("DIRG").u32(0).u32(1).u32(128).u32(32).zeros(2);
  CHECK(Grid::parse(data.span()).error() == FormatError::malformed);
}

TEST(grid_rejects_an_extent_that_is_not_a_whole_number_of_cells) {
  Builder data;
  data.text("DIRG").u32(16).u32(1).u32(130).u32(32).zeros(2);
  CHECK(Grid::parse(data.span()).error() == FormatError::malformed);
}

TEST(grid_rejects_a_row_that_is_not_a_whole_number_of_bytes) {
  // Four one-bit cells would be half a byte, and rows never straddle bytes.
  Builder data;
  data.text("DIRG").u32(16).u32(1).u32(64).u32(32).zeros(2);
  CHECK(Grid::parse(data.span()).error() == FormatError::malformed);
}

TEST(grid_rejects_unsupported_cell_widths) {
  Builder data;
  data.text("DIRG").u32(16).u32(3).u32(128).u32(32).zeros(64);
  CHECK(Grid::parse(data.span()).error() == FormatError::unsupported);
}

TEST(grid_rejects_a_body_that_does_not_fit) {
  Builder data;
  data.text("DIRG").u32(16).u32(1).u32(128).u32(2048).u8(0);
  CHECK(Grid::parse(data.span()).error() == FormatError::truncated);
}

TEST(grid_validate_rejects_a_trailing_byte) {
  Builder data = one_bit_grid(0, 0);
  data.u8(0);
  const auto grid = Grid::parse(data.span());
  CHECK(grid.ok());  // parsing only needs the body to fit
  CHECK(!grid->validate(data.size()).ok());
}

// --------------------------------------------------------------------------
// writing and editing
// --------------------------------------------------------------------------
//
// The writer copies the body as stored, so the packing under test is
// `set_cell`'s -- and the one fact worth pinning by byte value, not merely
// by a read-back through `cell`, is that it agrees with the *file*: a setter
// and a getter that were both most-significant-first would round-trip each
// other happily and disagree with every retail mask.

namespace {

bool same_bytes(std::span<const std::byte> a, std::span<const std::byte> b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

/// A value that differs from cell to cell and from its neighbours in the same
/// byte, cut to the depth. The shift folds the high bits down so that the low
/// bit varies too: a plain linear mix has a parity that cancels, which left a
/// one-bit grid of all zeroes and a test that could not see a setter packing
/// most-significant-first.
std::uint32_t pattern(std::uint32_t x, std::uint32_t y, std::uint32_t bits) {
  std::uint32_t raw = x * 0x9E3779B1u + y * 0x85EBCA77u;
  raw ^= raw >> 13;
  return bits >= 32 ? raw : raw & ((1u << bits) - 1u);
}

}  // namespace

TEST(write_grid_reproduces_a_parsed_file_byte_for_byte) {
  const Builder data = one_bit_grid(0b00000101, 0b10000000);
  const auto grid = Grid::parse(data.span());
  REQUIRE(grid.ok());
  const std::vector<std::byte> written = write_grid(grid.value());
  CHECK(written.size() == data.size());
  CHECK(same_bytes(written, data.span()));
}

TEST(write_grid_emits_the_extents_in_world_units_not_cells) {
  auto owned = OwnedGrid::create(64, 8, 256, 128);
  REQUIRE(owned.ok());
  const std::vector<std::byte> written = write_grid(owned->grid());
  REQUIRE(written.size() == 20 + 4 * 2);
  // "DIRG", then 64, 8, 256, 128 little-endian: a writer that put the width
  // (4) and height (2) there would produce a header the reader accepts and a
  // grid a quarter of the size in each direction.
  CHECK(static_cast<char>(written[0]) == 'D');
  CHECK(static_cast<char>(written[3]) == 'G');
  CHECK(static_cast<std::uint8_t>(written[4]) == 64);
  CHECK(static_cast<std::uint8_t>(written[8]) == 8);
  CHECK(static_cast<std::uint8_t>(written[12]) == 0x00);
  CHECK(static_cast<std::uint8_t>(written[13]) == 0x01);
  CHECK(static_cast<std::uint8_t>(written[16]) == 128);
  CHECK(static_cast<std::uint8_t>(written[17]) == 0);
  CHECK(same_bytes(written, owned->bytes()));
}

TEST(a_created_grid_is_zeroed_and_writable) {
  auto owned = OwnedGrid::create(16, 1, 2048, 2048);
  REQUIRE(owned.ok());
  CHECK(owned->grid().writable());
  CHECK(owned->grid().width() == 128);
  CHECK(owned->grid().height() == 128);
  CHECK(owned->grid().count_set() == 0);
  CHECK(owned->bytes().size() == 2068);
  CHECK(owned->grid().validate(owned->bytes().size()).ok());
  CHECK(owned->grid().max_cell_value() == 1);
}

TEST(create_refuses_what_parse_refuses) {
  CHECK(OwnedGrid::create(0, 1, 128, 32).error() == FormatError::malformed);
  CHECK(OwnedGrid::create(16, 1, 130, 32).error() == FormatError::malformed);
  // Twelve one-bit cells: a row and a half of bytes. The reader refuses it,
  // so the factory does too, and the stride's rounding never has to matter.
  CHECK(OwnedGrid::create(16, 1, 16 * 12, 32).error() == FormatError::malformed);
  CHECK(OwnedGrid::create(64, 4, 64 * 3, 64).error() == FormatError::malformed);
  CHECK(OwnedGrid::create(16, 3, 128, 32).error() == FormatError::unsupported);
  CHECK(OwnedGrid::create(64, 8, 64 * 5, 64 * 3).ok());
}

TEST(set_cell_packs_one_bit_cells_least_significant_first) {
  auto owned = OwnedGrid::create(16, 1, 128, 32);
  REQUIRE(owned.ok());
  Grid& grid = owned->grid();
  CHECK(grid.set_cell(0, 0, 1).ok());
  CHECK(grid.set_cell(3, 0, 1).ok());
  CHECK(grid.set_cell(7, 1, 1).ok());
  // Cells 0 and 3 of row 0 are bits 0 and 3: 0b00001001. Packed the other
  // way round the byte would read 0b10010000, and the mask would still look
  // like a mask.
  CHECK(static_cast<std::uint8_t>(owned->bytes()[20]) == 0b00001001);
  CHECK(static_cast<std::uint8_t>(owned->bytes()[21]) == 0b10000000);
  // Clearing puts the bit back without touching its neighbours.
  CHECK(grid.set_cell(0, 0, 0).ok());
  CHECK(static_cast<std::uint8_t>(owned->bytes()[20]) == 0b00001000);
  CHECK(grid.cell(3, 0) == 1);
  CHECK(grid.count_set() == 2);
}

TEST(set_cell_packs_nibbles_low_first_and_leaves_the_other_nibble_alone) {
  auto owned = OwnedGrid::create(64, 4, 64 * 4, 64);
  REQUIRE(owned.ok());
  Grid& grid = owned->grid();
  CHECK(grid.max_cell_value() == 15);
  CHECK(grid.set_cell(0, 0, 0xA).ok());
  CHECK(grid.set_cell(1, 0, 0x5).ok());
  // Cell 0 is the low nibble, cell 1 the high: 0x5A.
  CHECK(static_cast<std::uint8_t>(owned->bytes()[20]) == 0x5A);
  CHECK(grid.set_cell(0, 0, 0x0).ok());
  CHECK(static_cast<std::uint8_t>(owned->bytes()[20]) == 0x50);
  CHECK(grid.set_cell(1, 0, 0x3).ok());
  CHECK(static_cast<std::uint8_t>(owned->bytes()[20]) == 0x30);
  CHECK(grid.cell(0, 0) == 0);
  CHECK(grid.cell(1, 0) == 3);
}

TEST(set_cell_writes_sixteen_bit_cells_little_endian) {
  auto owned = OwnedGrid::create(64, 16, 64 * 2, 64);
  REQUIRE(owned.ok());
  Grid& grid = owned->grid();
  CHECK(grid.max_cell_value() == 0xFFFF);
  // A decor cell: kind 0x34, x nibble 2, y nibble 1.
  CHECK(grid.set_cell(1, 0, 0x1234).ok());
  CHECK(static_cast<std::uint8_t>(owned->bytes()[22]) == 0x34);
  CHECK(static_cast<std::uint8_t>(owned->bytes()[23]) == 0x12);
  CHECK(grid.cell(1, 0) == 0x1234);
  CHECK(grid.cell(0, 0) == 0);
}

TEST(set_cell_refuses_a_value_the_depth_cannot_hold) {
  auto owned = OwnedGrid::create(64, 4, 64 * 2, 64);
  REQUIRE(owned.ok());
  Grid& grid = owned->grid();
  CHECK(grid.set_cell(0, 0, 15).ok());
  // Terrain type 16 does not fit a 4-bit template layer; clamped or masked
  // it would become a different terrain, so it is refused and nothing moves.
  CHECK(grid.set_cell(0, 0, 16).error() == FormatError::buffer_too_small);
  CHECK(grid.cell(0, 0) == 15);
  CHECK(grid.fill(16).error() == FormatError::buffer_too_small);
  CHECK(grid.cell(0, 0) == 15);

  auto bits = OwnedGrid::create(16, 1, 128, 32);
  REQUIRE(bits.ok());
  CHECK(bits->grid().set_cell(0, 0, 2).error() == FormatError::buffer_too_small);
  CHECK(bits->grid().set_cell(0, 0, 1).ok());
}

TEST(set_cell_refuses_a_cell_outside_the_grid) {
  auto owned = OwnedGrid::create(64, 8, 64 * 4, 64 * 2);
  REQUIRE(owned.ok());
  Grid& grid = owned->grid();
  // Unlike `cell`, which clips silently for the stampers' sake.
  CHECK(grid.set_cell(4, 0, 1).error() == FormatError::out_of_range);
  CHECK(grid.set_cell(0, 2, 1).error() == FormatError::out_of_range);
  CHECK(grid.set_cell(3, 1, 1).ok());
  CHECK(grid.count_set() == 1);
}

TEST(a_read_only_view_refuses_to_be_edited) {
  const Builder data = one_bit_grid(0, 0);
  auto grid = Grid::parse(data.span());
  REQUIRE(grid.ok());
  CHECK(!grid->writable());
  CHECK(grid->set_cell(0, 0, 1).error() == FormatError::unsupported);
  CHECK(grid->fill(1).error() == FormatError::unsupported);
  CHECK(grid->count_set() == 0);
}

TEST(parse_mutable_writes_through_to_the_callers_bytes) {
  Builder data = one_bit_grid(0, 0);
  auto grid = Grid::parse_mutable(data.raw());
  REQUIRE(grid.ok());
  CHECK(grid->writable());
  CHECK(grid->set_cell(2, 1, 1).ok());
  CHECK(static_cast<std::uint8_t>(data.raw()[21]) == 0b00000100);
  // And a second, const view over the same bytes sees it.
  const auto again = Grid::parse(data.span());
  CHECK(again->cell(2, 1) == 1);
}

TEST(every_depth_sets_every_cell_and_round_trips_through_the_writer) {
  struct Shape {
    std::uint32_t cell_size;
    std::uint32_t bits;
    std::uint32_t width;
    std::uint32_t height;
  };
  // Odd widths on the byte-and-wider depths, where a stride computed from the
  // width alone (rather than width times bits over eight) would go wrong;
  // byte-aligned widths on the sub-byte ones, which is all they may have.
  const Shape shapes[] = {
      {16, 1, 128, 128}, {16, 1, 24, 3}, {64, 4, 6, 5},  {64, 4, 256, 2}, {32, 8, 5, 7},
      {32, 8, 1, 1},     {64, 16, 7, 3}, {64, 16, 256, 1}, {1, 2, 4, 3},  {1, 32, 3, 2},
  };
  for (const Shape& shape : shapes) {
    auto owned = OwnedGrid::create(shape.cell_size, shape.bits, shape.width * shape.cell_size,
                                   shape.height * shape.cell_size);
    REQUIRE(owned.ok());
    Grid& grid = owned->grid();
    CHECK(grid.width() == shape.width);
    CHECK(grid.height() == shape.height);
    CHECK(grid.stride() == shape.width * shape.bits / 8);
    CHECK(owned->bytes().size() == grid.expected_size());

    for (std::uint32_t y = 0; y < shape.height; ++y) {
      for (std::uint32_t x = 0; x < shape.width; ++x) {
        CHECK(grid.set_cell(x, y, pattern(x, y, shape.bits)).ok());
      }
    }
    bool read_back = true;
    for (std::uint32_t y = 0; y < shape.height; ++y) {
      for (std::uint32_t x = 0; x < shape.width; ++x) {
        read_back = read_back && grid.cell(x, y) == pattern(x, y, shape.bits);
      }
    }
    CHECK(read_back);

    const std::vector<std::byte> written = write_grid(grid);
    CHECK(same_bytes(written, owned->bytes()));
    const auto parsed = Grid::parse(written);
    REQUIRE(parsed.ok());
    CHECK(parsed->validate(written.size()).ok());
    bool same_cells = parsed->width() == shape.width && parsed->height() == shape.height &&
                      parsed->bits_per_cell() == shape.bits &&
                      parsed->extent_x() == shape.width * shape.cell_size &&
                      parsed->extent_y() == shape.height * shape.cell_size;
    for (std::uint32_t y = 0; same_cells && y < shape.height; ++y) {
      for (std::uint32_t x = 0; x < shape.width; ++x) {
        same_cells = same_cells && parsed->cell(x, y) == pattern(x, y, shape.bits);
      }
    }
    CHECK(same_cells);

    // A second grid built cell by cell from the first ends up the same bytes:
    // there is no state in a cell beyond its value.
    auto rebuilt =
        OwnedGrid::create(shape.cell_size, shape.bits, grid.extent_x(), grid.extent_y());
    REQUIRE(rebuilt.ok());
    for (std::uint32_t y = 0; y < shape.height; ++y) {
      for (std::uint32_t x = 0; x < shape.width; ++x) {
        CHECK(rebuilt->grid().set_cell(x, y, grid.cell(x, y)).ok());
      }
    }
    CHECK(same_bytes(rebuilt->bytes(), owned->bytes()));
  }
}

TEST(fill_sets_every_cell) {
  auto owned = OwnedGrid::create(64, 4, 64 * 4, 64 * 2);
  REQUIRE(owned.ok());
  CHECK(owned->grid().fill(15).ok());
  CHECK(owned->grid().count_set() == 8);
  CHECK(static_cast<std::uint8_t>(owned->bytes()[20]) == 0xFF);
  CHECK(owned->grid().fill(0).ok());
  CHECK(owned->grid().count_set() == 0);
}

TEST(an_owned_copy_is_independent_of_its_source) {
  const Builder data = one_bit_grid(0b00000101, 0b10000000);
  const auto source = Grid::parse(data.span());
  REQUIRE(source.ok());
  auto copy = OwnedGrid::copy(source.value());
  REQUIRE(copy.ok());
  CHECK(same_bytes(copy->bytes(), data.span()));
  CHECK(copy->grid().set_cell(1, 0, 1).ok());
  CHECK(copy->grid().cell(1, 0) == 1);
  CHECK(source->cell(1, 0) == 0);
  CHECK(static_cast<std::uint8_t>(data.span()[20]) == 0b00000101);
}

TEST(an_owned_grid_moves_with_its_bytes) {
  auto first = OwnedGrid::create(64, 8, 64 * 4, 64 * 2);
  REQUIRE(first.ok());
  CHECK(first->grid().set_cell(2, 1, 42).ok());
  const std::byte* buffer = first->bytes().data();

  OwnedGrid second(std::move(first.value()));
  // The vector's buffer travelled, and the grid still views it.
  CHECK(second.bytes().data() == buffer);
  CHECK(second.grid().cells().data() == buffer + 20);
  CHECK(second.grid().cell(2, 1) == 42);
  CHECK(second.grid().set_cell(0, 0, 7).ok());
  CHECK(static_cast<std::uint8_t>(second.bytes()[20]) == 7);
  // The source no longer views anything.
  CHECK(first->bytes().empty());
  CHECK(!first->grid().writable());
  CHECK(first->grid().cell_size() == 0);

  OwnedGrid third;
  third = std::move(second);
  CHECK(third.bytes().data() == buffer);
  CHECK(third.grid().cell(2, 1) == 42);
  CHECK(third.grid().cell(0, 0) == 7);
  CHECK(second.bytes().empty());
}
