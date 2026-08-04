# Tools

Build and asset tooling, written in C alongside the engine.

* `checkNoDynamicAllocation.sh` — enforces the project's no-allocation rule
  against our sources and compiled objects. Run via `make check`.

## Planned: asset extraction

The retail game's data lives in DBPF `.package` archives. Nothing from those
archives may be committed here — not textures, not audio, not test fixtures.
The intended approach is a command line extractor that reads a user's own
installation and writes into a local, git-ignored directory that the engine
loads at run time.

Format notes inherited from the upstream project are in `docs/formats/`.
