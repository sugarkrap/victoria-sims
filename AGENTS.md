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
| Linux | OpenGL ES 2.0 via EGL + X11 | ARMv5 and up. Kernels as old as 2.4. |
| WebAssembly | WebGPU, no wrapper library | `wasm32`, freestanding, no Emscripten. |

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

**Open question:** true OABI support is not yet proven. Continuous integration
cross-builds the portable engine core with `arm-linux-gnueabi-gcc` at
`-march=armv5te -mfloat-abi=soft`, which is ARMv5TE but *EABI*. A genuine OABI
toolchain has to be built by hand, and nobody has done it yet. Do not claim
OABI works until something has actually run on it.

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
  identifier: `APIWanted`, `displayEGL`, `renderOpenGLES2`. An acronym at the
  start of a variable name therefore begins with a capital — that is intended.

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
