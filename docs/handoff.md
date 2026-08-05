# Handoff

Where the engine is as of `3b62efd` on `claude/sims2-unity-bootstrap-kb9op7`,
what it does on a real disc, and what is worth doing next.

Written to be read by someone who has none of the conversation that produced it.

## Building and running

```sh
make            # the Linux build, into build/linux/victoriaSims
make web        # the WebAssembly build, into build/web/
make verify     # 15 C suites; all should say "checks passed"
make check      # proves no allocator symbol is linked in
node tests/verifyWebModule.mjs     # the wasm module against a fixture disc
node tests/verifyRuntimeUpload.mjs # the browser runtime's upload path
```

The Linux binary takes:

| Argument | What it does |
| --- | --- |
| `--disc=PATH` | An ISO **or a directory**. Which it is is decided by looking, not by the path's shape, so a mounted CD and a rip both work. |
| `--inspect-disc=PATH` | Lists what is on the disc and stops. No window, no rendering. |
| `--check` | Headless self-check; no window. Good for a machine with no display. |
| `--quiet` | Silences the periodic profiler line. |
| `--graphics-memory-mebibytes=N` | Pretends the driver has that much, so a small device can be simulated on a large machine. |

`--inspect-disc` and `--check` need no display, which makes them the two worth
reaching for first on an unfamiliar machine.

## What happens on a real disc

The retail ISO tested against is a repack: 605 loose packages plus `TSData.exe`,
a Delphi-stubbed program with a 2.7 GiB RAR appended past its three PE sections.
All 1,529 archive entries are stored rather than compressed, so no unpacker was
needed — 807 more packages mount as plain byte ranges inside that one file.

The load then goes, in order:

1. **Catalogue** the disc, and probe its large non-package files to say what
   they actually are rather than assuming.
2. **Mount** the installer's archive.
3. **Walk** for something to draw, preferring a model that carries bone data.
4. If nothing skinned turned up, **index every package for geometry** and open
   containers until one has bone assignments, then point the walk at that
   package.
5. **Fetch the texture** the material names, following a TXTR's reference to the
   LIFO holding its largest mip level.

On the tested disc that ends at a textured Sim face:
`#0x7f9bd9b9!age3_00_shpe` out of
`DATARUS/TSData/Res/UserData/Neighborhoods/Tutorial/Characters/Tutorial_User00001.package`,
521 vertices, all of them weighted, painted with `amface-s1` at 512×512.

Every stage says what it saw. A run's log is meant to be self-describing enough
that this document is not needed to interpret it.

## The skeleton, and why nothing moves

`#23` is complete as **reading** the skeleton. Nothing is applied, and that is a
finding rather than a gap:

> Skinning is `Σ weight · bonePose · inverseBind · v`. The inverse bind is the
> bone's transform in the pose the mesh was authored in. The mesh on the disc
> **is** in that pose, so at rest every pair multiplies out to the identity and
> a correct skin moves nothing at all.

Applying world transforms alone — the first attempt — double-transforms vertices
that are already in world space, and draws a Sim's face with a limb stretched
out of it. The disproof had been on screen for several runs beforehand: the face
drew correctly with no skinning applied whatsoever. The rule is documented on
`geometryMeshApplySkin` in `engine/include/victoria/geometryReader.h`, and a
check pins it: a palette of identities moves every vertex and lands each one
exactly where it started.

What is read and kept, ready for an animation:

- **Bone assignments** `0xFBD70111` — one word per vertex, four byte indices
  packed low-byte-first, 255 meaning an unused slot.
- **Bone weights** `0x3BD70105` — one to three floats stored, the last implied
  so they sum to one. The reader works the implied one back out.
- **The per-primitive bone list** — the last index array in a primitive's
  record. A vertex's assignment slots are indices into *this*, not bones. Slot 1
  means a different bone on the head than on the hands, and a mesh skinned as
  though the slots were bones folds itself inside out.

The element identifiers are not guesswork and never were: the format's own table
lives in the repo at
`legacy/scripts/openTS2/Files/Formats/DBPF/Scenegraph/Block/GeometryData/GeometryElement.cs`,
with all nineteen and both their wiki and in-game names. Read it before
inferring anything about an element.

## The one open question

Every run with a skinned mesh logs the bones its primitives named:

```
engine: weighted to 126 bone(s) of the tree, left in its bind pose because
skinning it there would move nothing; its primitives named bones 4 9 2 …
```

Whether those numbers are **positions in the tree's node list** or the
**identifiers its nodes carry** (`TransformNode.boneIdentifier`) has not been
settled. Small values mean the first; large ones mean the second, and want
matching against nodes rather than indexing into them. One glance at that line
answers it, and the answer decides how a pose palette gets built.

## Next

- **#26 — pose from an animation.** ANIM resources are `0xFB00791E` and the disc
  holds plenty; the index can find them exactly as `DISC_PHASE_SEEK_SKIN` finds
  geometry. The palette is `animatedTransform · inverse(bindTransform)`, the
  bind one being what `resourceNodeGetWorldTransform` returns. There is no
  matrix inverse in the engine yet — an affine one is a transposed rotation and
  a negated, rotated translation, worth writing as that rather than as a general
  inverse.
- **#24 — paint each primitive range separately.** The data is already recorded:
  each part's `firstIndex`, `indexCount` and its own material. What is missing is
  N draw calls over those ranges in all three backends, and a per-part texture
  fetch that survives the browser's pending reads.
- **#22 — walk the archive in fewer reads.** 1,529 entries at one round trip
  each.

## Known soft spots

- **The find-and-redirect path has no fixture.** The test disc holds no skinned
  mesh, so `make verify` cannot catch a regression in the search that finds one
  or the redirect that draws it — only the real disc exercises those. A skinned
  container in `scripts/makeTestDisc.sh` would close it, and #26 will want one
  anyway to have something to test a pose against.
- **The fixture disc reads more bytes than the image is long** (about 1.8×).
  That is not a leak: every model in it is rigid, so it takes the worst case of
  both searches — read every package, come back for the first, then index and
  survey. `tests/verifyWebModule.mjs` bounds it at 3× and explains which two
  costs make it up.
- **Load is slower than it was**, deliberately. Finding a skinned mesh means a
  second index pass over every package and up to 256 container reads. It runs
  after the picture is already on screen, so it delays "loaded" rather than the
  render.
- **Two unexplained singletons** in the index, unchanged for many runs:
  `refused 1 meshes — not a scenegraph resource`, and `1 would not be read`.
  One resource each. Small enough to have been left alone, large enough to be
  worth a look eventually.

## The rule that kept working

Four separate times, the disc contradicted an entirely reasonable inference —
that the installer kept a table at `0x30`, that its offset table would be near
the front, that a Sim is several shapes, that character meshes live where the
directory names suggest. Each time the fix was the same: stop reasoning about
what the disc should contain and make the engine report what it actually saw.

The logs are verbose on purpose. That is the method, not clutter.
