<h1 align="center">Victoria Sims</h1>

An open reimplementation of The Sims 2 in plain C, targeting OpenGL ES 2.0 on
Linux and WebGPU on WebAssembly. Forked from
[OpenTS2](https://github.com/LazyDuchess/OpenTS2), which is written in C# on
Unity; the engine here is being rebuilt from scratch rather than ported.

The two rules everything else follows from:

* **No dynamic allocation.** All storage is reserved statically and handed out
  from arenas.
* **128 MiB, hard.** That is the whole budget, not a starting point.

This README is a stub and will be rewritten once the project has more than a
bootstrap to describe. See [AGENTS.md](AGENTS.md) for the conventions and
constraints that currently govern the code.

## Building

```sh
make linux    # native OpenGL ES 2.0 build
make linux RENDER_BACKEND=software   # no GL at all, for pre-shader hardware
make web      # WebAssembly + WebGPU build
make armv7    # Cortex-A8 with NEON, the reference handheld
make armv5    # the ARMv5TE portability floor
make oabi     # the same, as genuine ARM old-ABI
make verify   # rasterizer checks
make check    # enforce the no-dynamic-allocation rule
```

The machine this is meant to run on is the Sharp NetWalker PC-Z1 — i.MX515
Cortex-A8, 512 MB of RAM, Ubuntu 9.04. ARMv5TE handhelds remain the supported
floor, which is why the old-ABI target exists. Their graphics hardware — Intel
2700G, NVIDIA GoForce — is OpenGL ES 1.x class and cannot run shaders at all,
so those machines use the software renderer.

## Status

Early. Three renderers — OpenGL ES 2.0, WebGPU, and a software rasterizer with
a NEON path — all drawing the same triangle, plus a hierarchical frame profiler
reporting zone timings alongside arena and graphics memory usage. There is no
gameplay and no asset loading yet.

## Assets

No game assets live in this repository and none ever will. Reading the retail
data is the job of extraction tooling that has yet to be written; see `tools/`.

## Acknowledgements

* [OpenTS2](https://github.com/LazyDuchess/OpenTS2) — the project this is
  forked from, and the source of the format notes under `docs/formats/`.

## License

This Source Code Form is subject to the terms of the Mozilla Public License,
v. 2.0. If a copy of the MPL was not distributed with this file, You can obtain
one at http://mozilla.org/MPL/2.0/.
