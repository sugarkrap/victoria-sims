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
make web      # WebAssembly + WebGPU build
make check    # enforce the no-dynamic-allocation rule
```

## Status

Early. Both targets build and draw a triangle, and there is a hierarchical
frame profiler that reports zone timings alongside arena usage — printed to the
terminal on Linux, shown as an overlay on the web. There is no gameplay and no
asset loading yet.

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
