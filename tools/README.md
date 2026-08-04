# Tools

One directory per standalone tool, each a git submodule of its own repository.
Their rules are relaxed — see the tools section of [AGENTS.md](../AGENTS.md).

| Tool | Purpose |
| --- | --- |
| [`vic-extractor`](https://github.com/sugarkrap/vic-extractor) | Reads a Sims 2 retail ISO and lists the DBPF packages on it, as a command line tool and as a web page. |

A tool must never reimplement a format the engine already parses.
`vic-extractor` compiles `engine/source/packageReader.c` and calls it over FFI,
so there is one implementation of DBPF rather than two that can drift apart. It
finds the engine at `../../engine` when checked out here as a submodule, and its
own continuous integration checks the engine out separately;
`VICTORIA_ENGINE_DIR` overrides both.

Each tool owns its continuous integration, its deployment and its secrets, so a
tool never gates the engine build and the engine's credentials are not shared
with tooling.

Clone with `--recursive`, or run `git submodule update --init` afterwards.
