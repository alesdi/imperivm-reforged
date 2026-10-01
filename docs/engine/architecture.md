# Engine architecture

Two layers, one rule.

```
engine/core/       simulation, script VM, entity system, map loading, formats
                   freestanding: bytes and state in, state out
engine/platform/   window, input, audio, GPU, filesystem, clock
                   depends on core; core never depends on it
engine/net/        the UDP socket and the clock a networked match needs; no SDL,
                   so the headless tools link it. What a peer decides -- the
                   turn packet, the link layer, the lobby -- is core
engine/app/        the thin wiring between them
engine/tests/      core tests, headless, no dependencies
```

**The core may not touch the platform.** No SDL, no filesystem, no clock, no threads, no
ambient randomness. [`tools/check_core_boundary.py`](../../tools/check_core_boundary.py)
enforces it in CI, because boundaries erode one reasonable-looking include at a time.

## Why the boundary is load-bearing

It would be easy to read this as architectural fashion. It is not. The project's central
verification strategy requires it.

Imperivm is lockstep-deterministic, and the original engine writes `desync.txt` dumps
containing per-tick world-state hashes and RNG seeds. That gives us a conformance oracle:
replay a recorded command stream through both engines and compare hashes tick by tick, and
the first divergent tick names the system we got wrong. Finding a bug in seconds instead of
after a two-hour playtest is the difference between this project finishing and not.

**That harness must run headless.** No window, no GPU, no audio, deterministic from the first
instruction. So the core has to be freestanding whether or not we care about portability.

Portability is then the same property viewed from another angle. A core that runs with no
platform beneath it runs under any platform: a native window, a WebAssembly page, an iOS
view, a test binary. We do not pay for those targets separately — we pay once, for the
oracle, and get them.

## Determinism rules

These apply to everything under `engine/core` without exception.

1. **No floating point.** Fixed point only. The same expression can round differently across
   compilers, architectures and optimisation levels, and one divergent bit desynchronises a
   match. The boundary check rejects `float` and `double`; a genuine exception is marked
   `// imperivm: allow-float: <reason>` and should be rare enough to argue about.
   The build also sets `-ffp-contract=off -fno-fast-math` so that contraction cannot
   reintroduce the problem behind our backs.
2. **One RNG**, explicitly seeded, serialised as world state. The original treats its seeds
   that way — they appear in the desync dump — and so do we.
3. **Iteration order is state.** Never iterate an unordered container in the simulation.
   Stable entity ordering is a correctness requirement, not a style preference.
4. **No wall clock.** The tick count is state; the time of day is not.

## Where things go

| Concern | Layer | Notes |
|---|---|---|
| Reading a `.pak` | core | takes a byte span; the caller supplies the bytes |
| Opening the file the span came from | platform | |
| Decoding a sprite frame | core | pure transformation |
| Uploading it to a texture atlas | platform | |
| Deciding a unit's next order | core | |
| Reading the mouse that requested it | platform | |
| Advancing a tick | core | |
| Deciding when to advance one | platform | |

The pattern throughout: **core computes, platform supplies and presents.** If a piece of code
needs to know where bytes came from or where pixels go, it is in the wrong layer.

## Targets

| Target | Status |
|---|---|
| macOS, Linux, Windows (SDL3) | building |
| WebAssembly (Emscripten) | wired into CI from the start |
| iOS, Android | no work done, no work needed yet — the boundary keeps the door open |

The web target is in CI from the first commit deliberately. A build that is green from the
beginning never bit-rots; one retrofitted in year two usually cannot be.

## Building

```
cmake -S . -B build -G Ninja
cmake --build build
./build/engine/tests/core_tests
./build/engine/app/imperivm
```

Requires CMake 3.24+, a C++20 compiler, and SDL3 for the windowed application. The core and
its tests need neither SDL nor any other dependency:

```
cmake -S . -B build-core -G Ninja -DIMPERIVM_BUILD_APP=OFF
```

That configuration is the one to keep working. If it ever needs a package, the boundary has
already been crossed.
