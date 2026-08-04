# Victoria Sims — working agreement

Rules for anyone, human or agent, writing code here. These are constraints, not
preferences. Where something is genuinely undecided it says so explicitly.

## What this project is

An open reimplementation of The Sims 2 in plain C, forked from
[OpenTS2](https://github.com/LazyDuchess/OpenTS2) (C#, Unity). The Unity engine
and all of its assets are gone. What survived the fork is reference material
only, under `legacy/`.

The aim is to be lighter than the original and far more portable. Two hard
constraints drive every design decision.

## The two hard constraints

### No dynamic allocation

The engine never calls `malloc`, `calloc`, `realloc`, `free`, `mmap`, `sbrk`,
or anything equivalent. Storage is reserved statically by the linker and handed
out by arena allocators (`engine/source/memoryArena.c`), which are bump
pointers over memory that already exists. Freeing means rewinding an arena to a
marker, so lifetimes are scoped rather than individually tracked.

`make check` enforces this by scanning our sources for allocator names and our
compiled objects for undefined allocator symbols. Platform libraries — X11,
Mesa, the browser — allocate internally and are outside our reach. That is a
known limit of the check, not an exemption for engine code.

An allocation that does not fit returns `NULL_POINTER`. There is no growth path
and no fallback allocator, so every call site must handle failure.

### 128 MiB, hard

`VICTORIA_MEMORY_BUDGET_BYTES` in `engine/include/victoria/memoryBudget.h` is
the entire budget. Not a starting point, not a soft target. Raising it is a
project-level decision and needs to be argued for, not slipped into a commit.

It is reserved as one uninitialised static array, so it costs nothing in the
shipped binary and nothing at startup beyond the operating system handing over
zeroed pages. On WebAssembly the module's linear memory is pinned at both
`--initial-memory` and `--max-memory`, so the module cannot grow past the
ceiling at run time even if something tried.

The web build currently reserves 129 MiB of linear memory: the 128 MiB arena
plus 1 MiB of slack for the shadow stack and static data. That slack is
tracked, and should stay small; it is not licence to spill over.

Whether 128 MiB is actually enough for a full game is unknown, and that is
fine. **We find out by hitting the ceiling, not by raising it pre-emptively.**

## Targets

These two are settled. They are the minimum, not the finish line.

| Target | Graphics | Notes |
| --- | --- | --- |
| Linux | OpenGL ES 2.0 via EGL + X11 | ARMv5 and up, EABI and OABI. Kernels as old as 2.4. |
| WebAssembly | WebGPU, no wrapper library | `wasm32`, freestanding, no Emscripten. |

The reference ARMv5 machine is the Sharp Zaurus SL-C760/C860 that
[piko](https://github.com/sugarkrap/piko) targets: PXA255, XScale ARMv5TE,
64 MB of RAM, and an ATI Imageon w100 with a few megabytes of its own memory.
Note that 64 MB is *half* the engine's 128 MiB ceiling, so the engine cannot
currently run there — ARMv5 is a compile-only portability target for now, and
that is a deliberate decision rather than an oversight.

Consequences worth internalising before writing platform code:

* **Assume nothing modern.** No `clock_gettime`, no KMS/DRM, no Wayland, no
  `io_uring`. `gettimeofday` and X11 are chosen deliberately for the old end of
  the range.
* **Dependencies are near-zero.** Linux links `-lEGL -lGLESv2 -lX11` and libc.
  The web build links nothing at all. Adding a dependency needs justification.
* **Nothing may be Emscripten-only.** The wasm build is `clang --target=wasm32
  -nostdlib`; the host page provides the handful of imports the engine needs.

## Toolchains

* **Clang** is the host compiler and the WebAssembly compiler. It is fast and
  targets `wasm32` natively.
* **GCC** is used for ARMv5. Clang's ARM backend is EABI-only, and old-ABI
  (OABI) support for kernel-2.4-era systems realistically needs GCC.
* Every toolchain in the `Makefile` is overridable (`HOST_COMPILER`,
  `WEB_COMPILER`, `ARM_COMPILER`), because targets outlive whatever is
  installed today.

**OABI is settled.** `-mabi=apcs-gnu` is a GCC ARM-backend codegen flag rather
than a property of one particular toolchain build, so the stock
`arm-linux-gnueabi-gcc` produces genuine old-ABI objects. `make oabi` builds
the engine core that way and verifies the result rather than assuming it: ELF
flags must read `0x600` (EABI version 0). An accidentally-EABI binary looks
perfectly fine right up until the device refuses to run it, so the check is not
optional and must never be relaxed to "it compiled".

This approach comes from the sibling [piko](https://github.com/sugarkrap/piko)
project, whose `tools/build-oabi-toolchain.sh` also has a from-source
crosstool-NG fallback for environments where no stock compiler works.

Still unproven: nothing has *run* on real hardware. The objects are correct;
that is not the same as the engine working on a device.

## Language

Plain C99. If C ever costs more than it pays for, switching to C++ is on the
table — but that is a deliberate decision to be made in the open, not something
to drift into one `.cpp` file at a time.

Assembly is allowed for optimisation. Always behind a portable C fallback, and
only once a measurement justifies it.

## Naming

* **camelCase** for variables, functions, and file names.
* **PascalCase** for types.
* **SCREAMING_SNAKE_CASE** for macros and compile-time constants.
* **No leading underscores.** Not on variables, not on include guards, not
  anywhere. Include guards look like `VICTORIA_MEMORY_ARENA_HEADER`.
* **Spell things out.** `widthInPixels`, not `w`. `elapsedSeconds`, not `dt`.
  `argumentCount`, not `argc`.
* **Acronyms keep every letter capitalised**, including at the start of an
  identifier: `APIWanted`, `readUTF8`, `displayEGL`, `renderOpenGLES2`. An
  acronym at the start of a variable name therefore begins with a capital —
  that is intended. This applies to the JavaScript glue as much as to the C.
  Names that come from someone else's API (`Uint8Array`, `getElementById`) stay
  spelled the way that API spells them.

Prefer no comments. Write one only when the *why* is not obvious from the code:
a hidden constraint, a subtle invariant, a deliberate choice a reader would
otherwise want to "fix".

## Assets and the law

The upstream project reads assets straight out of a retail install. This one
must not ship, vendor, or commit any game data — no `.package` files, no
textures, no audio, not even for tests. The original `TestAssets/` directory
was deleted in the fork for exactly this reason.

The plan is custom extraction tooling written in C, living in `tools/`, that
reads a user's own legitimate installation at run time. **No artifacts in the
repository, ever.** If a test needs data, it either generates it or is written
so it does not need it.

Format documentation reverse-engineered upstream is kept under `docs/formats/`.
Notes are fine; data is not.

## The profiler

`engine/include/victoria/profiler.h`. Instrumented and hierarchical: mark a
scope by hand, and the same numbers come out of every target. Sampling was not
an option — it needs stack unwinding and a signal or thread to sample from, and
WebAssembly gives us neither.

```c
VICTORIA_PROFILE_ZONE_BEGIN("renderDrawFrame");
/* ... */
VICTORIA_PROFILE_ZONE_END();
```

The macro caches its zone identifier in a block-scoped static, so a call site
pays for the name lookup once rather than once per frame. Every frame must be
bracketed by exactly one `engineBeginFrame` and one `engineEndFrame`; the
platform layer owns that boundary so work outside the engine — pumping events,
presenting — still lands inside the profiled frame.

Things worth knowing before you trust a number it prints:

* **Work and interval are not the same measurement.** Work is what the engine
  spent inside the frame. Interval is wall clock between frames, and includes
  whatever the platform does out of our reach: a vertical sync stall, or the
  browser waiting to present. Frames per second is derived from the interval
  only. On the web the two differ by two orders of magnitude, and reading fps
  off the work would have claimed 10000 fps at a real 60.
* **Storage comes from the global arena**, so profiling lives inside the
  128 MiB ceiling rather than beside it, and its own cost shows up in the
  report it prints. It is about 9.5 KiB with the default limits.
* **Capacities are fixed** — zones, nesting depth, and frame history. Raising
  them costs arena space and nothing else. Overflow is counted and reported,
  never silently dropped.
* **A zone's reported depth is fixed on first entry.** One reached at two
  different depths is shown at the first, rather than jumping between frames.
* `-DVICTORIA_PROFILER_ENABLED=0` compiles the whole thing out, macros
  included. Continuous integration builds that configuration so it cannot rot.

The report is plain text, formatted by the engine and displayed by the
platform: printed to the terminal on Linux, shown in an overlay on the web.
Both show the same text because both ask the engine for it.

## Graphics memory

`engine/include/victoria/graphicsMemoryBudget.h`. Graphics memory is handed out
by the driver, not by us — we cannot reserve it up front and we cannot see what
the driver adds on top. So unlike the system arena this is a **ledger and a
gate, not an allocator**: every resource the engine asks a backend to create is
declared first, and a request that would cross the ceiling is refused before
the driver is ever called.

The ceiling is dynamic because the hardware is not comparable. It is
established once at startup, in this order:

1. An explicit override, if given — `--graphics-memory-mebibytes=N` on Linux,
   `?graphicsMemoryMebibytes=N` on the web. This is how you simulate a
   small-memory device on a machine that has plenty, and it is the only
   practical way to exercise the refusal path.
2. Whatever the backend will admit to. Neither OpenGL ES 2.0 nor WebGPU has a
   portable query, so this means the `GL_NVX_gpu_memory_info` and
   `GL_ATI_meminfo` vendor extensions, or WebGPU's `maxBufferSize` as a proxy
   for device class. Frequently unavailable.
3. `VICTORIA_GRAPHICS_MEMORY_DEFAULT_BYTES`, deliberately conservative.

The report says which of the three it was — `detected`, `override`, or
`assumed` — because a number the driver told us and a number we guessed deserve
very different amounts of trust.

**Declare before you create, and release what you declared.** A refusal must
abort the resource, not be logged and ignored. Releases are clamped rather than
trusted, so a double release cannot wrap the counter and make the ledger read
as almost entirely free.

## Shaders

Compile every shader during initialisation, and then **draw with it once**.
Compiling alone is not enough: drivers routinely defer real code generation
until a program is first used, so eager compilation on its own moves the stall
to frame one rather than removing it.

The measured split on this project's own first shader, under llvmpipe, was
11 ms to compile and link and a further **49 ms** that only happened on first
draw. That second number is invisible to a compile-only approach and was four
fifths of the cost.

The warm-up draws with colour writes masked off into a one-pixel viewport, then
calls `glFinish` — the wait is the entire point. On the web the equivalent
draws into a one-pixel off-screen texture.

Initialisation is bracketed as a profiler frame of its own, so this cost is
attributed rather than hidden: startup shows up as frame one, and as the worst
frame until something beats it. Zones entered outside a frame accumulate
nothing and would have reported as zero.

## Layout

```
engine/          portable core — no platform or graphics API calls
  include/victoria/
  source/
platform/        one directory per platform backend
  linux/         X11 + EGL entry point
  web/           wasm entry point, host page, JavaScript glue
render/          one directory per graphics backend
  openGLES2/
  webGPU/
tools/           asset extraction and build tooling
docs/formats/    reverse-engineering notes inherited from upstream
legacy/          upstream C# and Unity shaders, reference only, never built
```

`engine/` must stay free of platform and graphics calls. Anything touching a
window, a device, or the clock belongs in `platform/` or `render/` behind
`platformInterface.h` or `renderInterface.h`.

`legacy/` is a reading room. It is not compiled, not linted, and not held to
the conventions above. Port from it deliberately; do not wire it into the
build.

## Before you push

```sh
make linux && make web && make check
```

The build runs with `-Wall -Wextra -Wshadow -Wstrict-prototypes
-Wmissing-prototypes -Wpointer-arith -Wcast-qual -Wwrite-strings -pedantic`,
and continuous integration adds `-Werror`. Keep it warning-free.
