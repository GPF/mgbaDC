# Testing mGBADC on real Dreamcast hardware

This project shares its hardware setup (console, network loader, GDB stub)
with the sibling `gpsp-sh4-jit` project — see
`gpsp-sh4-jit/docs/dreamcast-test-harness.md` for the fuller reference on
`kos-tool`/GDB mechanics in general. This doc covers what's specific to
mGBADC and the hard lessons from actually getting this port running.

## Quick reference

- **Console IP: `192.168.0.128`.**
- Ping it first: `ping -c 2 -W 2 192.168.0.128`. No response means the
  console is off/rebooting/wedged — don't launch, check first.
- Before every launch: `ps aux | grep -i "kos-tool\|sh-elf-gdb" | grep -v grep`
  must be empty, **unless** you've just independently confirmed via ping
  that the console came back up after a reboot/power-cycle, in which case a
  leftover `kos-tool` process is safely orphaned and can be killed directly.
- **Never wrap a `kos-tool` launch in a shell `timeout`.** A `timeout`-forced
  SIGTERM against `kos-tool` is the same mistake as killing it directly —
  it can burn the GDB stub's one-shot connection or leave the console in an
  unclear state. Launch with `nohup ... &`/`disown` and just wait/poll the
  log file instead.
- **This GDB stub does not support interrupting a running/hung target** —
  confirmed by the stub replying "Cannot execute this command while the
  target is running. Use the 'interrupt' command..." to a `Ctrl-C`/SIGINT.
  Don't try it. If a `-g` session's `continue` never returns, the target is
  genuinely hung (not paused, not waiting) and needs a physical
  power-cycle; the GDB connection itself is also burned at that point (this
  stub accepts exactly one client per session — a client that dies
  mid-`continue` without a clean detach cannot reconnect).

## Staged test files (`build-dc/sdl/cd/`)

Mirrors gpSP's Dreamcast port's `cd/` convention:

```
build-dc/sdl/cd/gba_bios.bin          # real GBA BIOS, from gpsp-sh4-jit's cd/gba_bios.bin
build-dc/sdl/cd/roms/DangerousXmas.gba  # small homebrew test ROM (~1.3MiB)
```

`kos-tool -m cd/` maps this local directory to `/pc/` on the console over
dc-load-ip's network filesystem — so `/pc/roms/DangerousXmas.gba` on the
Dreamcast side is `build-dc/sdl/cd/roms/DangerousXmas.gba` on the host,
live, no re-upload needed between launches. Swap in other ROMs by copying
them into `cd/roms/` and updating the hardcoded fallback path in `main.c`
(see `AGENTS.md`'s note on argv not being forwarded — there is currently no
way to pick a ROM at launch time other than editing that hardcoded path and
rebuilding).

The Dreamcast build uses a 4 MiB cartridge window and disables mGBA rewind
to leave enough heap for SDL2, the emulator thread, and audio on the console's
16 MiB RAM. ROMs larger than 4 MiB are not supported yet.

## Launching (normal, no debugger)

```bash
source /opt/toolchains/dc/kos/environ.sh
cd build-dc/sdl
nohup kos-tool -t 192.168.0.128 -x mgba.elf -m cd/ > /tmp/kostool.log 2>&1 &
disown
sleep 8
tail -30 /tmp/kostool.log
```

For a foreground controller test, run:

```bash
kos-tool -t 192.168.0.128 -x sdl/mgba.elf -m sdl/cd/
```

Dreamcast controller bindings are A/B/X/Y/Start/C for GBA A/B/L/R/Start/Select,
the D-pad for directions, and the left/right triggers for GBA L/R.

Then poll `/tmp/kostool.log` and `ping`/`ps` as needed rather than blocking
the shell on `kos-tool` directly — this keeps you able to run other
diagnostic commands (checking console state, killing an orphaned process
after a confirmed reboot, etc.) without fighting a foregrounded process.

### Recovering from a hang or crash

- A **clean crash** (an actual fault) drops back to dc-load on its own —
  `kos-tool` prints `Program returned 1` (or similar) and exits; `ping`
  keeps responding throughout. Nothing to do except investigate the log.
- A **hard reboot** (the console visibly resets/reboots) means `ping` stops
  responding, then comes back on its own once the console finishes
  rebooting — wait for `ping` to succeed again before doing anything else.
  There is then a **stale, orphaned `kos-tool` process** on the host side
  (it was talking to a session that no longer exists) — safe to `kill` it
  directly, but only *after* independently confirming via `ping` that the
  console is back up.
- A **genuine hang** (no crash, no reboot, `kos-tool` process still alive
  and consuming CPU, but no further log output and — often — `ping` stops
  responding too, presumably because interrupts are disabled) has **no
  software recovery path** if it wasn't launched with `-g` (and even with
  `-g`, this stub can't interrupt a running target — see above). Use the
  controller exit chord (below) first; if that doesn't work within a few
  seconds, go straight to a physical power-cycle, no need to hesitate.

### Controller exit chord

`main.c` registers a correctly typed Maple callback for
`CONT_START | CONT_A | CONT_B | CONT_X | CONT_Y` as the very first thing
in `main()`, so holding **Start+A+B+X+Y together** calls `exit(EXIT_SUCCESS)` and
drops back to dc-load, independent of whatever the program is doing. This
is the primary recovery mechanism for a live/hung Dreamcast with no
debugger attached. It does **not** always work (confirmed: a genuine hang
with interrupts disabled can prevent even this from firing) — don't treat
it as guaranteed; fall back to a power-cycle without hesitation if it
doesn't respond within a few seconds.

## Using GDB (`DREAMCAST_GDB`)

`gdb_init()` (which routes SH-4 exceptions into kos-tool's GDB stub instead
of KOS's default abort handler) is gated behind `#ifdef DREAMCAST_GDB` in
`main.c`'s `main()` — **off by default, and it must stay off for a normal
launch.** With it active, a fault waits forever for a GDB connection
instead of dying cleanly or letting the controller chord recover it —
confirmed on hardware, needed a physical reboot when forgotten.

To enable it for one debug build, temporarily add near the top of
`src/platform/sdl/main.c` (before its first `#include`):

```c
#define DREAMCAST_GDB
```

Rebuild, then launch with `-g` and connect:

```bash
source /opt/toolchains/dc/kos/environ.sh
cd build-dc/sdl
nohup kos-tool -t 192.168.0.128 -g -x mgba.elf -m cd/ > /tmp/kostool.log 2>&1 &
disown
sleep 3   # wait for "waiting for gdb client connection..." in the log

sh-elf-gdb -batch mgba.elf \
  -ex "target remote localhost:2159" \
  -ex "continue" \
  -ex "bt full" \
  -ex "info registers" \
  -ex "quit"
```

`-t 192.168.0.128` is the console; **GDB connects to `localhost:2159`**,
kos-tool's local proxy port. Always pass the ELF as `sh-elf-gdb`'s first
argument (needed for symbols/backtraces).

**Remove `#define DREAMCAST_GDB` again before any normal launch.**

### Catching a specific point with `gdb_breakpoint()`

If the crash is a deliberate `abort()`/panic call rather than a raw CPU
exception, `continue` alone won't stop there — GDB only sees "Inferior
exited", the same as normal program exit. Set a real breakpoint on the
function that does the terminating instead, e.g. `break arch_abort` before
`continue`. For a genuine *hang* (no crash at all), sprinkle explicit
`gdb_breakpoint()` calls (from `<arch/gdb.h>`) at candidate points in the
code, guarded by `DREAMCAST_GDB`, and step through them one `continue` at a
time — there is no way to interrupt into an arbitrary point of a running
target with this stub (see "Quick reference" above).

**Only one `continue` should be queued if you intend to inspect state at
that specific stop** — queuing a second `continue` in the same batch
invocation resumes execution again immediately, past whatever you meant to
inspect. This bit the very first hardware debugging session on this port:
a fault was correctly caught at `arch_abort()`'s first instruction, but a
redundant second `continue` in the same batch let it run to completion
before `bt full` could execute.

## Diagnostic `printf` checkpoints

`DC_CHECKPOINT(msg)` (defined identically — copy-pasted, not shared via a
header, since it's meant to be temporary — near the top of `src/gba/core.c`
and `src/gba/gba.c`) does `printf("mgba-dc: %s\n", msg)` always, plus
`gdb_breakpoint()` when `DREAMCAST_GDB` is also defined. **stdout must be
unbuffered** for these to be visible before a crash/hang — already done via
`setvbuf(stdout, NULL, _IONBF, 0)` at the very top of `main()`. Without
that call, checkpoints can silently vanish into a stdio buffer that never
gets flushed, making a `printf` that *did* execute look identical to one
that never ran — this cost real hardware cycles before being caught (see
`NEXT_TASK.md`'s history for exactly where).

Search for `DC_CHECKPOINT` and `mgba-dc:` printf calls across the tree to
find/remove this instrumentation once it's no longer needed — it's not
meant to be permanent.
