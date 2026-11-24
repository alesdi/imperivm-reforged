#pragma once

// What `gbr.exe` keeps for the life of the process rather than of a game.
//
// A process global in the original is written by a game and read by a game,
// and **neither a save writes it nor a load or a new game resets it**. So it
// lasts from the process's start to its end: a game loaded in the process
// that set it finds it set, and a game started or loaded in a fresh process
// finds it zero. It is not in the state hash either, so two peers that
// disagree about it do not desync on the hash -- they play different blows.
//
// This engine's process is whoever owns the `ProcessGlobals` a session is
// given (`SessionInputs::process`): the app keeps one for as long as it runs
// and hands it to every session it builds, new game and load alike. A tool
// that gives none (`imrun`, `imconform`, `imsave`, the tests) gets one owned
// by the session itself, which is a fresh process per session -- and, for
// `imconform`'s lockstep peers, one process per peer, which is what a
// networked game is.
//
// It belongs to no system's serialize or hash, deliberately: putting it in
// either would be the departure this file exists to remove.

namespace imperivm::core::sim {

struct ProcessGlobals {
  /// `SetGlobalBloodlust`'s byte, the dword at 0x00996ae4 (0x004c6610 stores
  /// the argument's low byte, zero-extended). Its two readers are the blow
  /// calculator's (0x0050ff30: 0x00510b8a, C15, and 0x00510d4e, C20), which
  /// test it against zero. Nothing else in the image writes it.
  bool bloodlust = false;
};

}  // namespace imperivm::core::sim
