# mGBADC — mGBA on Dreamcast

This is upstream mGBA 0.10.5, patched to cross-compile and (mostly) run under
KallistiOS (KOS) on the Sega Dreamcast, using mGBA's existing SDL frontend
(`src/platform/sdl/`) against KOS's own SDL2 port rather than writing a new
platform frontend from scratch.

**Status as of 2026-09-16: builds and links clean, boots on real hardware
through SDL2/PVR video init and ROM detection, then crashes inside
`core->init()` due to what looks like a `struct mCore*` pointer surviving a
function return with a corrupted/zeroed tail (see `NEXT_TASK.md` — this is
the current blocker, not yet root-caused).**

## Why this exists / how it's structured

- mGBA only ships a CMake build. KOS projects normally use plain Makefiles
  (see `gpsp-sh4-jit/ports/dreamcast/Makefile` for the sibling GBA-emulator
  port), but KOS *does* have real CMake toolchain support (`kos-cmake`, see
  `cannonball/build-dc/dreamcast.sh` for another project using it) plus a
  working SDL2 port under `kos/addons`. Building mGBA's stock CMake project
  against that SDL2, instead of hand-porting its CMake source lists into a
  bespoke Makefile, was the pragmatic choice — see `docs/BUILDING.md`.
- All Dreamcast-specific changes follow the same pattern mGBA's own PSP2/
  3DS/Wii ports use: `#ifdef __DREAMCAST__` (and `CMAKE_SYSTEM_NAME`) guards
  sprinkled into a handful of shared files, not a forked/duplicated
  codebase. `__DREAMCAST__` is predefined by the KOS toolchain itself.
- The video/audio/input backend is KOS's own SDL2 port (`kos/addons`),
  specifically its "Dreamcast PVR" render driver — not raw `dc/video.h`/
  `dc/sound/stream.h`/`dc/maple.h` calls.
  **`/home/gpf/code/dreamcast/MGBAVitaEX-dc/` was built first, before
  deciding to go the SDL2 route** — it's a from-scratch KOS platform layer
  (`dc_input.c`/`dc_video.c`/`dc_audio.c`/`dc_fs.c`) calling those raw APIs
  directly: Maple controller polling, a software framebuffer blit (no PVR
  texture pipeline), `snd_stream` ring-buffer audio, `/pc/`→`/cd/` file
  fallback. It was never wired to a working mGBA core (mCore integration
  was still TODO when the SDL2 path proved faster to get booting). Keep
  this in mind as the concrete starting point for removing the SDL2
  indirection layer later (e.g. once mGBADC is stable and the SDL2 overhead
  on a 200MHz SH-4 turns out to matter) — the mCore wiring learned here
  (video buffer format, `GBA_KEY_*` input mapping, ROM loading via
  `mCoreFind`/`mCoreLoadFile`) carries over directly, only the presentation
  layer would need swapping.

## Directory layout

- Standard upstream mGBA 0.10.5 source tree (`src/`, `include/`, etc.)
- `build-dc/` — the KOS/CMake build directory. **Always reconfigure with
  `kos-cmake` (see `docs/BUILDING.md`) after moving this tree** — CMake
  caches absolute paths throughout the generated build files.
- `build-dc/sdl/mgba.elf` — the actual built binary.
- `build-dc/sdl/cd/` — staged test files, mirroring gpSP's Dreamcast port's
  `cd/` convention: `gba_bios.bin` + `roms/DangerousXmas.gba` (a small
  homebrew test ROM, copied from
  `gpsp-sh4-jit/ports/dreamcast/roms/DangerousXmas.bin`). `kos-tool -m cd/`
  maps this local directory to `/pc/` on the console over dc-load-ip — see
  `docs/TESTING.md`.

## Key facts worth not re-deriving

- **KOS has no `pthread_create()` in its base libc** — pthreads only exist
  via the separate `libpthread` KOS port addon
  (`kos/addons/lib/dreamcast/libpthread.a`, a real implementation over
  KOS's native `kthread_t`/`mutex_t`/`condvar_t` API, not a stub). mGBA's
  threading (`mCoreThread*`, the SDL frontend's whole architecture) requires
  this — see `CMakeLists.txt`'s Dreamcast branch.
- **KOS has no `mmap()`** — mGBA's `anonymousMemoryMap()`/`mappedMemoryFree()`
  fall back to `calloc()`/`free()` via `-DDISABLE_ANON_MMAP`. This makes
  mGBA's cart-ROM-window allocation (normally a cheap lazily-committed 32MiB
  `mmap`) into a real, immediate 32MiB `calloc()` — impossible on the
  Dreamcast's 16MiB total RAM. Fixed by shrinking `SIZE_CART0` to 8MiB for
  `__DREAMCAST__` (matching gpSP's own proven `dc_pagecache.c` cap) — see
  `include/mgba/internal/gba/memory.h`.
- **KOS's dc-load-ip loader does not forward `argc`/`argv` into `main()`**,
  confirmed on hardware (`argc==0` even when `kos-tool -x ... -- <path>`
  reports sending them). There is no argv-based ROM path on this platform —
  `main.c` falls back to a hardcoded path when `args.fname` is NULL, same
  reasoning as gpSP's Dreamcast port never using argv either.
- **`gdb_init()` must never be compiled into a normal (non-`-g`) launch.**
  With it active, a crash waits forever for a GDB connection that isn't
  there instead of dying or letting the controller exit-chord recover it —
  confirmed on hardware, needed a physical reboot. It's gated behind
  `DREAMCAST_GDB`, off by default; see `docs/TESTING.md`.
- **This KOS GDB stub does not support interrupting a running/hung target**
  (no Ctrl-C equivalent) — confirmed by the stub itself replying "Cannot
  execute this command while the target is running." `gdb_breakpoint()`
  calls compiled into the code (guarded by `DREAMCAST_GDB`) are the only way
  to get a stop at a specific point.
- **stdout is buffered over dc-load's console redirect.** `printf` calls
  sprinkled in for diagnosis can silently disappear before a crash/hang
  unless stdout is set unbuffered (`setvbuf(stdout, NULL, _IONBF, 0)`,
  already done at the top of `main()`).

## Where the actual patches are

See `git diff` in this tree, or grep for `__DREAMCAST__` / `DC_CHECKPOINT` /
`DREAMCAST_GDB`. `NEXT_TASK.md` has a running list of what's been touched
and why, including which diagnostic-only instrumentation still needs
cleanup once the current blocker is resolved.
