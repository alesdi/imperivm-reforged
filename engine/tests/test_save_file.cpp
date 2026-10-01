// The saved game on disk: a container holding the session beside a manifest.
//
// The envelope (`test_save.cpp`) proves the simulation's bytes round-trip;
// this proves the file around them does -- the manifest survives its INI
// spelling, the entries come back by name, and what a loader needs to refuse
// is refused. The byte-identity of the container with the Python reference
// writer is `tests/test_corpus_imsave.py`'s, over a save `imsave` wrote.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/bfhp.hpp"
#include "imperivm/core/sim/save_file.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;

namespace {

std::vector<std::byte> bytes_of(std::string_view text) {
  std::vector<std::byte> out(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) out[i] = static_cast<std::byte>(text[i]);
  return out;
}

SaveFileContents sample() {
  SaveFileContents contents;
  contents.manifest.container = "Conquests/mediterranean.bfhp";
  contents.manifest.map_index = "7";
  contents.manifest.seed = 12345;
  contents.manifest.turns = 40;
  contents.manifest.time = 19995;
  contents.manifest.engine = "0.1.0";
  // Not a real envelope: the file does not look inside the session, and a
  // payload that crosses the direct-block limit is the one worth carrying.
  contents.session.assign(70'000, std::byte{0x5A});
  return contents;
}

}  // namespace

TEST(a_save_manifest_survives_its_ini_spelling) {
  const SaveFileContents contents = sample();
  const std::vector<std::byte> ini = encode_save_manifest(contents.manifest);
  const Result<SaveFileManifest> back = decode_save_manifest(ini);
  REQUIRE(back.ok());
  CHECK(*back == contents.manifest);

  // The first map is spelled as an empty `map=`, and it comes back empty
  // rather than as a zero, because `read_payloads` takes the digits.
  SaveFileManifest first = contents.manifest;
  first.map_index.clear();
  const Result<SaveFileManifest> again = decode_save_manifest(encode_save_manifest(first));
  REQUIRE(again.ok());
  CHECK(again->map_index.empty());
}

TEST(a_save_manifest_refuses_what_a_loader_cannot_use) {
  // No `[Save]` section: some other INI.
  CHECK(decode_save_manifest(bytes_of("[Other]\ncontainer=x\n")).error() ==
        FormatError::malformed);
  // No container: nothing to build a session from.
  CHECK(decode_save_manifest(bytes_of("[Save]\nmap=3\n")).error() == FormatError::malformed);
  // A number read half-way is not a number.
  CHECK(decode_save_manifest(bytes_of("[Save]\ncontainer=x\nseed=12abc\n")).error() ==
        FormatError::malformed);
  CHECK(decode_save_manifest(bytes_of("[Save]\ncontainer=x\nturns=-1\n")).error() ==
        FormatError::malformed);
  // Absent numbers are the defaults, not refusals.
  const Result<SaveFileManifest> sparse = decode_save_manifest(bytes_of("[Save]\ncontainer=x\n"));
  REQUIRE(sparse.ok());
  CHECK(sparse->seed == 1);
  CHECK(sparse->turns == 0);
  CHECK(sparse->map_index.empty());
}

TEST(a_save_file_round_trips_through_its_container) {
  const SaveFileContents contents = sample();
  const std::vector<std::byte> raw = encode_save_file(contents);

  // It is a container the reader accepts, with exactly the two entries, in
  // the order the header documents.
  const Result<BlockFile> file = BlockFile::open(raw);
  REQUIRE(file.ok());
  CHECK(file->validate().ok());
  const Result<BlockFileIndex> index = BlockFileIndex::build(*file);
  REQUIRE(index.ok());
  REQUIRE(index->size() == 2);
  CHECK(index->entries()[0].path == kSaveManifestEntry);
  CHECK(index->entries()[1].path == kSaveSessionEntry);

  const Result<SaveFileContents> back = decode_save_file(raw);
  REQUIRE(back.ok());
  CHECK(back->manifest == contents.manifest);
  CHECK(back->session == contents.session);
}

TEST(a_save_file_refuses_a_container_that_is_not_a_save) {
  // Not a container at all.
  CHECK(decode_save_file(bytes_of("ISAV....")).error() == FormatError::bad_magic);
  // A container with no manifest, and one with no session: both are
  // `malformed`, not `not_found`, because the file as a whole is the wrong
  // shape rather than one lookup having missed.
  {
    BlockFileBuilder builder;
    REQUIRE(builder.file(kSaveSessionEntry, bytes_of("x")).ok());
    CHECK(decode_save_file(builder.build()).error() == FormatError::malformed);
  }
  {
    BlockFileBuilder builder;
    REQUIRE(builder.file(kSaveManifestEntry, bytes_of("[Save]\ncontainer=x\n")).ok());
    CHECK(decode_save_file(builder.build()).error() == FormatError::malformed);
  }
  // A manifest that does not parse is the manifest's own error.
  {
    BlockFileBuilder builder;
    REQUIRE(builder.file(kSaveManifestEntry, bytes_of("[Save]\nseed=1\n")).ok());
    REQUIRE(builder.file(kSaveSessionEntry, bytes_of("x")).ok());
    CHECK(decode_save_file(builder.build()).error() == FormatError::malformed);
  }
}
