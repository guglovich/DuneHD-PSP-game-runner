# Dune HD PSP Game Runner

Run PlayStation Portable games **natively** on a Dune HD Max media player
(Sigma SMP8645, MIPS32 little-endian) — a **MIPS-on-MIPS** approach: no CPU
emulator, no JIT. The guest game binary executes as ordinary native code
inside the player's UI process, and the PSP operating system is replaced by a
hand-written HLE (high-level emulation) layer.

> Status: **work in progress**. The target title (LocoRoco 2, ULJS00042)
> boots, initializes its threading/sound/font subsystems and reaches a
> stable multi-threaded idle state. The frame pipeline is the current
> frontier. This repository is the engineering diary as much as the code:
> `docs/` contains a detailed, dated log of every finding, wrong turn and
> measurement.

---

## Why MIPS-on-MIPS is different

On a conventional emulator you translate or interpret guest code. Here the
guest *is* native code for the host CPU — which removes the interpreter
entirely and changes the problem set:

* **No CPU loop.** The guest program counter is a real program counter.
  There is nothing between the game and the silicon except the syscalls it
  makes.
* **What replaces the kernel is everything.** The PSP firmware is not there.
  Every `sceKernel*` / `sceDisplay*` / `sceGe*` call the game makes must be
  intercepted and answered, with *correct PSP semantics*, or the game logic
  stalls, spins or dies — usually silently.
* **Failure is process failure.** The guest runs inside `shell`, the UI
  process of the player. A wild pointer in the guest is a wild pointer in
  the player's UI. Every mitigation (fault handler, thread isolation,
  clean shutdown) is written with that in mind.

## Architecture

```
             ┌────────────────────────────────────────────────┐
             │   shell  (Dune HD UI process, MIPS32 LE)        │
             │                                                │
             │   plugin_run_native_code()                     │
             │        │                                       │
             │        ▼                                       │
             │   native_code.so   ← this project               │
             │        │                                       │
             │        ├─ ELF loader      (PSP EBOOT.BIN)      │
             │        ├─ relocator      (R_MIPS_26/HI16/LO16) │
             │        ├─ import patcher (.lib.stub → trampoline)
             │        ├─ HLE dispatcher (269 syscalls)       │
             │        ├─ fault handler (SIGSEGV/BUS/ILL)      │
             │        └─ diagnostics   (counters, PC sampling)│
             │        │                                       │
             │        ▼                                       │
             │   guest code @ 0x08804000 + mirrors            │
             │   (LocoRoco 2 ELF, executed natively)          │
             └────────────────────────────────────────────────┘
```

### 1. Loading (`src/guest_load.c`, `include/guest_load.h`)

* Decrypt/extract the EBOOT (tools: `pspdecrypt`, `iso_extract_eboot`,
  `cso_extract`) and map the MIPS ELF into the guest address range.
* The image is mapped at the PSP-user-space base `0x08804000`, with the
  **four PSP address mirrors** (`0x08…` cached, `0x48…` uncached, `0x88…`
  KSEG0, `0xA8…` KSEG1) aliasing the same backing — the GE command lists
  arrive through the uncached mirror, so it has to exist for real.
* All relocations are applied at load time (`R_MIPS_26`, `R_MIPS_HI16`,
  `R_MIPS_LO16`, …). The `+0x8000` carry for `addiu`-composed halves is a
  documented trap: getting it wrong sends every call `0x10000` too high.

### 2. Import interception: stub patching + trampolines

The guest calls firmware through PSP-style **import stubs** (8-byte entries
per library, `.lib.stub` descriptors, NID tables). We:

1. Parse `.lib.stub` and the NID tables at load (see `docs/nid-table.md`
   for the exact on-disk layout — the section order is *not* the library
   order, which cost us a wrong-identification bug once).
2. For every imported function, generate a **hand-assembled trampoline**
   in the guest mapping and rewrite the stub to jump to it
   (`src/guest_load.c: guest_build_import_trampoline`).

The trampoline saves the guest ABI state, switches `$gp` to the host value,
calls the C dispatcher `guest_import(index, args, k0, ra)` and restores
everything. Details that mattered on real hardware:

* **o32 calling convention everywhere.** The trampoline is written as raw
  instruction words (`I_LUI`, `I_ADDIU`, …) because the plugin is freestanding
  and its own code must not depend on libc.
* **`$k0` is the PSP thread context pointer.** PSP games expect
  `SceKernelThreadContext` in `$k0` and *write to it*. The trampoline
  re-installs `$k0` on every return into the guest — measured: setting it
  once at thread start does not survive four calls.
* **64-bit returns come back as `$v0` + `$v1`.** A stub that only sets
  `$v0` turns `sceKernelGetSystemTimeWide` into a random-number generator:
  the guest's frame timer measured garbage and its 17-iteration timeout
  loop ran 2.2 million times. The trampoline now finishes with
  `move $v1,zero` in the `jr $ra` delay slot — zero extra bytes.

### 3. HLE: the PSP OS, implemented one syscall at a time

`src/plugin/guest_run.c` is a long dispatch chain keyed by NID
(`data/psp_nids.tsv` maps NIDs to names). The hard-learned rules:

* **Returns must be PSP-true, not merely non-zero.** PSP convention:
  `0` is success, negative is error. Some calls want a *descriptor*
  (VPL, semaphore, thread, callback IDs must be non-zero and distinct).
* **Blocking calls must actually block.** Our first stubs returned
  instantly, and the game's idle loops became 2 MHz spinners that ate the
  whole call budget. Now:
  * `sceKernelWaitSema` parks 1 ms per call,
  * `sceKernelDelayThread` sleeps `a0` µs (rounded up to ms),
  * `sceDisplayWaitVblank` sleeps one frame (16 ms).
  Result: the guest went from 50M calls/30 s to a stable ~2.8k calls/s —
  and the host scheduler finally gives every guest thread its share.
* **`SleepThread` vs `SleepThreadCB`.** Task-queue workers poll a flag
  with `SleepThread` (10 ms parking); the callback-dispatcher thread parks
  in `SleepThreadCB` and *serves callbacks there* (below).

### 4. Fault recovery: the guest must not kill the UI

A signal handler (`guest_fault_handler`) catches SIGSEGV / SIGBUS / SIGILL
*in guest threads* and keeps the process alive:

* **Null-page emulation.** PSP games legitimately `memset(NULL,…)` /
  `strlen(NULL)` through any of the four mirrors. Loads from the null page
  return zero into the destination register, stores are skipped, the PC is
  advanced past the instruction. A 124-byte `memset` through the uncached
  mirror is ~129 individually handled faults — per call, on purpose.
* **MIPS32r2 emulation.** The PSP's Allegrex CPU has r2 instructions that
  the player's 4KEc core (r1) does not: `ins`, `ext` were first, and later
  `seb`, `seh`, `wsbh`, `rotr`, `rotrv` (a scan of the game's `.text`
  found 95 / 51 / 53 / 15 sites). The handler emulates them from the signal
  frame — including the famous "which ucontext offset is the PC, really"
  problem, solved by printing all candidates and picking the one inside
  the guest image.
* **Spin-of-death guard.** A fault storm at one PC (not the null page)
  ends the *thread*, not the process — with a `SPINDEAD=` line in the log.
* **Every kill is logged.** A silent `plat_thread_exit()` in the SIGILL
  path once erased the main game thread without a trace; it now prints
  `SIGILLD= pc= ins=` first. Rule: *a death without a log line is a bug in
  the harness, not in the guest.*

### 5. Callbacks: PSP semantics without a kernel

PSP executes registered callbacks inside whichever thread calls a
`*CB`-suffixed service. Since the guest is native code in the same address
space, we can do **exactly that**: `sceKernelCreateCallback` stores
`(func, common)` in a table; notifications (UMD inserted, power state,
MemoryStick events) are queued by our stubs and delivered by calling the
guest's function *directly, as a plain function pointer* — from the thread
parked in `sceKernelSleepThreadCB`. MIPS o32 makes this safe: same ABI,
callee-saved registers respected, and this guest does not use `$gp` for
globals (verified by disassembly).

### 6. The `dlclose` hazard — the one that bites plugins

The host unloads the plugin (`dlclose`) as soon as our entry point returns.
Any host thread still looping *inside our library* is then executing in
freed memory — the first `nanosleep` turns into a SIGSEGV that kills the UI
and puts the player in a restart loop (we did this to a real player once;
it is documented in `docs/guest-boot-args.md`, appendix 8). The rule set:

1. **No HLE stub may run forever.** Every park has an exit condition.
2. `guest_run_wait` raises `g_time_to_exit` *before returning to the host*
   and waits out the 50 ms park quantum, so callback/sleep loops unwind and
   guest threads end in `ExitDeleteThread` — which, like on PSP, really
   terminates the thread (`plat_thread_exit`, raw `exit` syscall — thread
   only, never `exit_group`, never libc `exit` with its stdio flushes).

### 7. Diagnostics — because the guest cannot talk

Everything below ships in the report file the plugin writes (`docs/`
explains how to read it):

* **Per-import counters with `last ra` and `last a0`** — who called what,
  with which argument; `ra` decodes to the exact call site in the guest.
* **Early call log** (first 1000 imports, each tagged with the calling
  thread's tid) — the *chronology* of boot.
* **PC sampling**: every ~2 s the callback thread sends SIGALRM (via
  `tgkill`) to every guest thread; the handler prints each thread's PC
  candidates from the signal frame. This found the exact point where the
  main game thread vanished.
* **Thread liveness**: `/proc/self/task` scan (raw `getdents64`,
  syscall 4219 on MIPS o32 — 4204 is `rt_sigprocmask`, ask us how we
  know) plus a `tgkill(…, 0)`-probe and a `GONE!` log line with the dead
  thread's entry point.
* **Frame dumps**: every `sceDisplaySetFrameBuf` can snapshot the guest's
  framebuffer to PPM.

## Repository layout

```
src/
  guest_load.c        loader, relocator, import patcher, trampolines
  elf_loader.c, psp_*.c, ge_*.c, gfx_*.c, main.c
    (earlier standalone-toolchain stages: ISO/CSO, EBOOT decryption,
     GE command walker, GFX translator experiments)
  plugin/
    dune_plugin.c     plugin entry point + report writer
    guest_run.c       HLE dispatcher, fault handler, callbacks, diagnostics
    plat.c            freestanding platform layer (raw syscalls only)
include/              loader/plugin interfaces
tools/                host-side analysis: NID table builder, disassembler,
                      trampoline/uctx/reloc self-check gates, ISO/CSO tools
tests/                host-runnable probes (ucontext layout, fault filter,
                      GE math, trampoline dumps) — each reproduces one
                      finding from docs/
data/psp_nids.tsv     NID → name table (from pspsdk)
docs/                 dated engineering log (RU) + reference notes;
                      README-legacy-ru.md is the original design doc
Makefile             build + `check-plugin` gate + deploy targets
```

## Build

Cross-toolchain: Bootlin musl MIPS32el (the plugin is freestanding
`-nostdlib` code that resolves *nothing* — the `check-plugin` gate
verifies: no unresolved symbols, no NEEDED entries, entry point exported).

```sh
make -B native_code.so   # full rebuild (plain make does not rebuild the plugin)
make check-plugin        # mandatory gate before every deploy
```

Deploy target expects a player reachable over SSH with the key in
`../Dune Max/tmp/dune_key` (adjust `PLAYER`/`KEY` in the Makefile for your
setup). `scp` is broken on this firmware; deploy uses
`ssh 'cat > file'` streaming.

## What "works" means here — current status

* Loader, relocator, all 269 imports trampolined, 120 000+ relocations
  applied, zero unsupported.
* Guest reaches its multi-threaded steady state: 5+ native guest threads,
  VPL heap, font check, SAS audio init, four GE display lists parsed,
  60 Hz timer thread running with correct 64-bit time.
* The two "silent thread killers" found and fixed on real hardware:
  the `GetGPI` stub returning garbage (the game's logger checks a HOLD bit
  in it — an odd value sends the logger down a path that ends the thread)
  and `GetThreadStackFreeSize` answering `0` ("stack exhausted").
* Next frontier: the game's init-complete flag and the task queue feeding
  its renderer.

## Legal

No game content is (or will ever be) in this repository. The ISO, the
decrypted EBOOT and the guest ELF are the copyright of their respective
owners. `data/psp_nids.tsv` is a NID name table derived from the open
pspsdk project. Everything else is original engineering work for running
software you legitimately own on hardware you own.
