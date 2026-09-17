# Next task: root-cause the `core->init` corruption

## Current blocker (read this first)

The `mCore::init` crash is resolved. The root cause was a Dreamcast-only
header-order ABI mismatch: KOS headers exposed `PATH_MAX=4096`, while SDL's
networking include path later introduced `PATH_MAX=1024`. Since
`mDirectorySet.baseName[PATH_MAX]` is embedded before the function pointers,
the core library and SDL caller computed different `mCore::init` offsets
(4504 versus 1432). Including `sys/syslimits.h` from the Dreamcast common
header makes both translation units agree at `PATH_MAX=1024` and offset 1432.
Hardware now reaches ROM execution with video, input, and audio initialized.

The new blocker is performance: the test ROM runs very slowly on hardware.
Investigate this after removing the temporary diagnostics below, starting
with SDL/PVR presentation cost and thread/audio scheduling.

### Historical pre-fix notes

The original build booted on real hardware through SDL2/PVR video init and ROM
detection (`mCoreFind()` correctly identifies `DangerousXmas.gba` as a GBA
ROM and constructs a `struct mCore` via `GBACoreCreate()`), then crashes —
sometimes as a clean "Program returned 1", sometimes as a full hardware
reboot — the moment `main.c` calls `renderer.core->init(renderer.core)`.

**Confirmed root cause of the crash itself:** `renderer.core->init` is
`NULL` at that call site, so it's jumping through a null function pointer.

**Not yet root-caused: why it's NULL.** This is the actual mystery, and
it's a genuinely strange one — not a logic bug in any obvious sense.

### What's been proven, step by step, with `printf` checkpoints

1. Inside `GBACoreCreate()` (`src/gba/core.c`), immediately after
   `core->init = _GBACoreInit;`, printing `core->init` shows the correct
   address (e.g. `0x8c0399cc`, matching `_GBACoreInit`'s own printed
   address).
2. Still inside `GBACoreCreate()`, right before its final `return core;`,
   `core->init` is still correct.
3. Back in `mCoreFindVF()`/`mCoreFind()` (`src/core/core.c`), immediately
   after `core = mCoreFindVF(vf)` returns from that call, `core->init` is
   *still* correct.
4. Still inside `mCoreFind()`, immediately after `vf->close(vf)`,
   `core->init` is *still* correct.
5. **The instant `mCoreFind()` returns to its caller in `main.c`** — before
   any assignment to `renderer.core`, checked via a separate local
   `struct mCore* dbgcore = mCoreFind(args.fname);` — `dbgcore->init` reads
   back as `0x0`. `dbgcore` itself (the pointer value) is correct — same
   address as printed in steps 1-4. Only the memory *behind* the pointer
   reads differently.

So: the same pointer, dereferenced on two sides of a single function
return, gives a correct value on the callee side and a zeroed value on the
caller side, immediately, with the file-close diagnostic ruling out any
intervening syscall.

### What's been ruled out

- **`vf->close(vf)` / dc-load-ip network close syscall.** Skipped it
  entirely (diagnostic-only — see `src/core/core.c`, currently has
  `vf->close(vf)` commented out under `__DREAMCAST__`, **this leaks the
  file descriptor and must be reverted** once the real bug is found).
  Corruption persisted identically with `close()` never called at all.
- **LTO.** Rebuilt with `-DBUILD_LTO=OFF` and did a fully clean rebuild
  (`rm -rf` both `.dir` build directories, not just a cache-variable flip,
  to rule out a stale mixed LTO/non-LTO link). Corruption persisted
  identically.
- **Struct-layout mismatch from differing preprocessor defines between
  translation units.** `struct mCore` (`include/mgba/core/core.h`) has
  `#ifdef MINIMAL_CORE`-conditional fields (`dirs`, `inputMap`) sitting
  before the function-pointer fields including `init` — if `MINIMAL_CORE`
  (or anything else affecting that region) were defined differently
  between `src/gba/core.c` and `src/platform/sdl/main.c`, every field after
  that point would land at different offsets in each translation unit,
  which would look exactly like this. Checked both `flags.make` files
  directly (`build-dc/CMakeFiles/mgba.dir/flags.make` vs.
  `build-dc/sdl/CMakeFiles/mgba-sdl.dir/flags.make`) — identical `-D`
  defines except `-DBUILD_SDL` (only affects `src/platform/qt/*`, not
  touched by this build at all) and SDL include paths. No header-based
  `#define MINIMAL_CORE` exists anywhere in the tree (`grep -rn` came back
  empty). Reasonably confident this isn't it, though not airtight.
- **Manual SH-4 disassembly of `mCoreFind`'s compiled object
  (`sh-elf-objdump -d -j .text.mCoreFind ...`).** The compiled return
  sequence looks structurally correct: `core` (kept in a callee-saved
  register, r8, across the `vf->close()` call) gets moved into r0 (the
  SH-4 return-value register) after that call returns, and none of the
  subsequent callee-saved-register restores (r12/r11/r10/r9/r8/pr) touch
  r0 before `rts`. This does not rule out a caller-side codegen bug in how
  `main.c`'s object reads the return value or computes `->init`'s offset —
  that side hasn't been disassembled yet (see below).

### What was in progress when this session paused for docs

Adding `printf("... offsetof(struct mCore, init) = %u\n", (unsigned)offsetof(struct mCore, init));`
independently in both `src/gba/core.c` (inside `GBACoreCreate`) and
`src/platform/sdl/main.c` (right before reading `dbgcore->init`), to get
each translation unit's own authoritative computed offset for that field,
side by side, on a single hardware run. If they differ, that's a real
struct-layout/ABI mismatch (root cause found, next step is finding *why*
they differ). If they're identical, struct layout is conclusively ruled
out and the bug is somewhere stranger — likely worth disassembling the
*caller* side (`main.c`'s object file, the `mCoreFind` call site and the
immediately following `->init` load) the same way `mCoreFind` itself was
disassembled, or trying a completely different optimization level (`-O0`)
as a next isolation step given this is an experimental compiler snapshot.

**This diagnostic was written but not yet rebuilt/tested on hardware** —
do that first when resuming.

### Diagnostic instrumentation currently in the tree (clean up once resolved)

- `DC_CHECKPOINT` macro + calls throughout `_GBACoreInit`/`GBAInit`
  (`src/gba/core.c`, `src/gba/gba.c`) — harmless to leave short-term
  (no-ops without `__DREAMCAST__`), but noisy.
- Multiple `printf("mgba-dc: ...")` checkpoints in `src/platform/sdl/main.c`
  (main entered/argc/argv, parsed args, mCoreFind, GBACoreCreate pointer
  dump, etc.) and `src/core/core.c` (`mCoreFind`'s before/after-close
  prints).
- **`vf->close(vf)` is currently skipped entirely under `__DREAMCAST__`**
  in `src/core/core.c`'s `mCoreFind()` — this is a real resource leak, not
  just noise. Revert this specific one as soon as it's no longer needed for
  the close-causality test, regardless of whether it turns out to be
  related to the actual bug.
- The temporary `#define DREAMCAST_GDB` at the top of `main.c` should be
  **removed** (not just left commented) before any non-debugging launch —
  see `docs/TESTING.md`'s warning about why.

## Backlog (after the blocker is fixed)

1. **Revert/clean up all diagnostic instrumentation** listed above once
   `core->init` reliably survives into `main()` and the emulator actually
   runs a frame.
2. **ROM selection**: currently hardcoded to `/pc/roms/DangerousXmas.gba`
   in `main.c` (argv isn't forwarded by KOS's loader — see `AGENTS.md`).
   Replace with a directory scan (`/pc/roms/*.gba`, falling back to
   `/cd/roms/*.gba`) once boot is otherwise proven, mirroring gpSP's own
   `dc_rom_catalog.c` scan-and-pick approach.
3. **Input mapping**: KOS's SDL2 "Dreamcast PVR" driver presumably maps
   Maple controller input into SDL's joystick/gamepad API somehow, but this
   hasn't been verified or wired into mGBA's key bindings yet
   (`mSDLInitBindingsGBA`/`sdl-events.c`) — next real test once boot works.
4. **Save data / config paths**: `mCoreConfigDirectory()` now points at
   `/pc/mgba` (see `src/core/config.c`'s `__DREAMCAST__` branch), which
   only exists during a `kos-tool -m` dev session, not a real CD boot.
   Revisit once there's a real `/cd/`-based or VMU-based save story
   (gpSP's Dreamcast port has no VMU support either, for reference — this
   is genuinely unsolved territory here, not just unported).
5. **8MiB ROM cap**: `SIZE_CART0` is shrunk to 8MiB for `__DREAMCAST__`
   (see `include/mgba/internal/gba/memory.h`), matching gpSP's own
   `dc_pagecache.c` cap, but with no actual paging — ROMs over 8MiB are
   simply unsupported right now. `DangerousXmas.gba` (~1.3MiB) is well
   under this. If bigger-ROM support is ever wanted, gpSP's
   `dc_pagecache.c` is the proven design to port (page-cache with on-demand
   eviction, not a fully-resident buffer).
6. **Consider dropping SDL2 for the raw-KOS-API scaffold** at
   `/home/gpf/code/dreamcast/MGBAVitaEX-dc/` once mGBADC is stable — see
   `AGENTS.md`'s note. That scaffold already has working Maple/PVR/
   `snd_stream`/`/pc/`↔`/cd/` glue; only the mCore wiring (proven here)
   would need porting over, dropping the SDL2 indirection layer.
7. **Audit `_GNU_SOURCE`/`DISABLE_ANON_MMAP`/pthread wiring for upstream-
   ability**: none of the `CMakeLists.txt` Dreamcast branch is currently
   upstreamable as-is (it's development-expedient, not reviewed for
   correctness on other configurations) — worth a pass before considering
   contributing any of this back, if that's ever a goal.
