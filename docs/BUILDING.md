# Building mGBADC

## Prerequisites

KOS toolchain installed at `/opt/toolchains/dc/kos` (this machine has it).
Always source the environment first, in every fresh shell, before any KOS
command (`kos-cmake`, `kos-cc`, `sh-elf-*`) — otherwise they're simply not
on `PATH`:

```bash
source /opt/toolchains/dc/kos/environ.sh
```

## Configure

Run from `build-dc/` (create it if starting fresh: `mkdir build-dc`).
**If this tree was moved after a previous configure, delete
`CMakeCache.txt` and `CMakeFiles/` first** — CMake caches absolute source/
binary directory paths and generated Makefiles embed them throughout;
editing the cache alone is not sufficient.

```bash
source /opt/toolchains/dc/kos/environ.sh
export PKG_CONFIG_PATH=/opt/toolchains/dc/kos/addons/lib/dreamcast/pkgconfig:$PKG_CONFIG_PATH
cd build-dc
kos-cmake -S .. -B . \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr/local \
  -DCMAKE_C_FLAGS="-D_GNU_SOURCE" \
  -DCMAKE_CXX_FLAGS="-D_GNU_SOURCE" \
  -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON \
  -DSDL2_DIR=/opt/toolchains/dc/kos/addons/lib/dreamcast/cmake/SDL2 \
  -DBUILD_LTO=OFF \
  -DBUILD_SDL=ON \
  -DBUILD_QT=OFF \
  -DBUILD_GL=OFF \
  -DBUILD_GLES2=OFF \
  -DBUILD_GLES3=OFF \
  -DUSE_EPOXY=OFF \
  -DUSE_FFMPEG=OFF \
  -DUSE_SQLITE3=OFF \
  -DUSE_ZLIB=OFF \
  -DUSE_MINIZIP=OFF \
  -DUSE_LIBZIP=OFF \
  -DUSE_PNG=OFF \
  -DUSE_LZMA=OFF \
  -DUSE_ELF=OFF \
  -DUSE_EDITLINE=OFF \
  -DUSE_DISCORD_RPC=OFF \
  -DENABLE_SCRIPTING=OFF \
  -DBUILD_STATIC=ON \
  -DBUILD_SHARED=OFF
```

### Why each non-obvious flag is there

- `-D_GNU_SOURCE` (both C and CXX flags) — without this, KOS's newlib
  hides `strdup`/`strlcpy`/`locale_t` etc. behind feature-test macros the
  same way glibc does, but glibc defaults them on and newlib doesn't. Every
  platform mGBA ships on until now happened to get `_GNU_SOURCE` some other
  way (Linux's own CMake branch sets it via `add_definitions`); Dreamcast's
  branch doesn't, so it must come from here.
- `-DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON` + `-DSDL2_DIR=...` — without
  this, `find_package(SDL2)` defaults to MODULE mode (looking for a
  `FindSDL2.cmake` CMake ships, which doesn't exist), silently never
  finding SDL2 at all. KOS's SDL2 port only ships a CONFIG-mode package
  (`SDL2Config.cmake`). Do **not** rely on `sdl2.pc` (pkg-config) as a
  fallback here either — it exists but its `prefix=`/`includedir=` are
  baked from the SDL2 build machine's own paths and get double-prefixed
  incorrectly under this toolchain's `PKG_CONFIG_SYSROOT_DIR`; the CONFIG
  package's paths are correct absolute paths and don't have this problem.
- `-DBUILD_LTO=OFF` — see `NEXT_TASK.md`'s current blocker. Not confirmed
  as the actual cause (disabling it did **not** fix the bug), but keep it
  off for now to eliminate it as a variable while debugging; this toolchain
  is an experimental GCC snapshot (`17.0.0 20260703 (experimental)` per the
  KOS boot banner) and SH-4 LTO codegen is under-exercised in general.
- `-DBUILD_GL=OFF -DBUILD_GLES2=OFF -DBUILD_GLES3=OFF -DUSE_EPOXY=OFF` —
  without these, the build tries to compile mGBA's OpenGL renderer path
  (`epoxy/gl.h`), which isn't available/wanted here; the software renderer
  (`sw-sdl2.c`) is what's patched for the "Dreamcast PVR" SDL render driver.
- Everything else under `USE_*`/`BUILD_QT`/`ENABLE_SCRIPTING` is turned off
  to keep the dependency surface minimal — none of zlib/libpng/sqlite3/
  liblzma/ELF-loading/editline/Discord RPC/Python are built or available
  in this environment, and none are required for booting a plain `.gba`
  ROM.
- `-DBUILD_STATIC=ON -DBUILD_SHARED=OFF` — a Dreamcast ELF is a single
  static binary; there's no shared-library loading story here.

## Build

```bash
source /opt/toolchains/dc/kos/environ.sh
cd build-dc
cmake --build . -j"$(nproc)"
```

Output: `build-dc/sdl/mgba.elf`.

### If you see a link error, check these first (in order they were hit)

Recorded here because they're easy to reintroduce if `CMakeLists.txt`'s
Dreamcast branch (search `CMAKE_SYSTEM_NAME MATCHES "^[Dd]ream[Cc]ast$"`)
ever gets touched:

1. `undefined reference to VFileOpenFD / VDirOpen` — CMake's `UNIX` variable
   is false for this custom `CMAKE_SYSTEM_NAME`, so the generic POSIX VFS
   backend (`vfs-fd.c`/`vfs-dirent.c`) silently never got added to
   `CORE_VFS_SRC`. Confirm the Dreamcast branch in `CMakeLists.txt` still
   appends those.
2. `undefined reference to mCoreThread*` (all of them) — `USE_PTHREADS`
   wasn't actually defined for the compile (see "no pthread_create in base
   libc" above); `mgba-util/threading.h` silently self-defines
   `DISABLE_THREADING` as a fallback when no real thread backend is
   detected, which compiles out nearly all of `src/core/thread.c`. Confirm
   `find_function(pthread_create)`'s `CMAKE_REQUIRED_LIBRARIES pthread` is
   still set before that check runs, and that `pthread` is in `OS_LIB`.
3. `fatal error: sys/mman.h` while compiling `platform/posix/memory.c` —
   `DISABLE_ANON_MMAP` wasn't defined; confirm `add_definitions(-DDISABLE_ANON_MMAP)`
   is still in the Dreamcast branch.
4. `fatal error: SDL.h` while compiling `platform/sdl/*.c` — SDL2 wasn't
   actually found (see the `CMAKE_FIND_PACKAGE_PREFER_CONFIG`/`SDL2_DIR`
   note above); check `cmake`'s configure-time summary printed
   `SDL (2): ON`, not `OFF`.
5. `undefined reference to ftruncate / fsync` — KOS's newlib has neither;
   `src/util/vfs/vfs-fd.c` stubs both for `__DREAMCAST__` already, confirm
   those guards are intact.
6. `undefined reference to pthread_sigmask` — KOS's `libpthread` addon
   doesn't implement it (no real POSIX signal delivery model on this
   target to block signals from anyway); `src/core/thread.c` guards both
   call sites with `!defined(__DREAMCAST__)`, confirm intact.

### Compile-time bugs hit along the way (not link errors — real latent bugs)

`include/mgba-util/vector.h`'s `IntList` was declared `DECLARE_VECTOR(IntList, int)`
but *defined* `DEFINE_VECTOR(IntList, int32_t)` in `src/debugger/parser.c` — a
silent no-op mismatch on every platform mGBA has shipped on before (where
`int` and `int32_t` are the same type), but SH-4 newlib's `stdint.h` makes
`int32_t` a `long`, exposing it as a hard "conflicting types" error. Fixed
by matching the declaration to the definition. A second instance of the
same `int`/`int32_t` latent bug was in a local variable in
`src/debugger/parser.c` itself (`tmpSegment`). Neither fix is Dreamcast-
specific — they're correctness fixes that happen to only matter on a
toolchain where `int32_t != int`.
