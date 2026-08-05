# Handoff

Where the engine is, what it does on a real disc, and what is worth doing next.

Written to be read by someone who has none of the conversation that produced it.

## Building and running

```sh
make            # the Linux build, into build/linux/victoriaSims
make web        # the WebAssembly build, into build/web/
make verify     # 16 C suites; all should say "checks passed"
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
6. If what was drawn has a skeleton, **index every package for animations** and
   open them until one names this skeleton, then pose the mesh by it.

On the tested disc that ends at a textured Sim face:
`#0x7f9bd9b9!age3_00_shpe` out of
`DATARUS/TSData/Res/UserData/Neighborhoods/Tutorial/Characters/Tutorial_User00001.package`,
521 vertices, all of them weighted, painted with `amface-s1` at 512×512.

Every stage says what it saw. A run's log is meant to be self-describing enough
that this document is not needed to interpret it.

## The skeleton, and why a resting mesh still does not move

`#23` read the skeleton and applied nothing. That was a finding rather than a
gap, and it still holds — what changed with #26 is that there is now an
animation to supply transforms that are not the bind pose:

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

What is read and kept, and now fed to one:

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

## The question that was open, and the answer

Whether a primitive's bone numbers were **positions in the node list** or the
**identifiers its nodes carry** is settled: they are identifiers.

The heuristic written down here for telling them apart — small means positions,
large means identifiers — was **wrong**, and worth recording as wrong. Real bone
identifiers are small: this disc's face names bones 7, 6 and 5. What is large is
`0x7FFFFFFF`, the sentinel a node carries when it is *not* a bone, and seeing
that on a root node is what made identifiers look like they ought to be big.

The reference settles it without any inference at all: `ScenegraphComponent.cs`
keys its own lookup as `_boneIdToTransform[transformNode.BoneId]`.
`resourceNodeFindByBoneIdentifier` now searches rather than indexes, and a check
pins it against a tree whose two orders deliberately disagree.

## The bind pose was in the file all along

The plan recorded here was to invert what `resourceNodeGetWorldTransform`
returns. That is not necessary: the GMDC carries its own pose-transform array,
one quaternion and translation per bone, in a section straight after the
primitives that this reader used to stop short of. It is numbered the way the
primitives' bone lists are numbered, so a bone number indexes it directly.

It holds the **inverse** bind, which was measured rather than assumed:

```
engine: 3 bone name(s) matched a node by identifier, 0 matched none; over 3
measured, world x stored is 0.000 from the identity and stored is 1.666 from
world — the smaller says which the file holds
```

Both directions are printed on every run, because the number that matters is
whichever is near nought and a reader told only the winner has to take the
comparison on trust. So the engine still has no matrix inverse and, on this
evidence, needs none.

## #26 — posing from an animation

Done. A skinned mesh on screen now sends the load into
`DISC_PHASE_SEEK_ANIMATION`, which indexes the disc for `0xFB00791E` exactly as
the skin search indexes for geometry, and opens animations until one will pose
the model:

```
engine: 11757 animation(s) across 1411 package(s) to choose from
engine: animation a2o-exerciseMachine-benchPress-start_anim — 89 channel(s) over
5 target(s), 3400 tick(s) long, authored against auskel, and 4 inverse
kinematics chain(s) this does not follow
engine: posed 521 of 521 vertices over 63 bone(s), 52 channel(s) of the
animation reaching them; it moved by 0.410 against a model 0.242 across
```

Two things about that run are the whole lesson of it.

**An animation is matched by name, not by number.** A channel carries a bone's
name as a string and the tree carries nodes with names, so no hash is resolved
and no index is trusted. The model's own bones on this disc are `head`, `neck`
and `spine2`.

**The rest pose is what proves it, and nothing else can.** Every animation on
the disc produces a shape nobody here can check by looking — except
`a-pose-neutral-stand_anim`, which is very nearly the pose the mesh was authored
in and so should move it almost not at all. The animation phase now asks for
that one by name before falling back to the scan, and says outright whether the
result was what it had to be:

```
engine: posed 521 of 521 vertices over 63 bone(s), 48 channel(s) of the
animation reaching them; it moved by 0.011 against a model 0.242 across
engine: that was the rest pose, which should move the mesh almost not at all —
and it did not, so the pose composes the way the game does
```

Under five per cent of the model's own span. That number is the standing proof
that the palette, the bind pose, the bone numbering and the Euler convention all
agree with the game; run it before trusting any change to them.

**Do not diagnose this by looking at two screenshots.** The camera orbits at
`elapsedSeconds * 0.6f` (`render/openGLES2/renderOpenGLES2.c`), so two captures
taken at different moments show the model from different angles. A 521-vertex
face has no modelled back to the skull, and seen from the side it is a profile
with a smooth shapeless mass behind it — which looks exactly like a mesh torn
apart. That cost a full round of misdirected diagnosis here: a working pose was
called a spike, and two hypotheses were built and refuted before the rest pose
settled it in one run. The instrument to reach for is that check, not the eye.

**The first version of it did lie, though.** It accepted the first animation that read, and
reported 521 vertices posed by a lighting rig's animation — because
`geometryMeshApplySkin` returns the number of vertices it *walked*, and an
all-identity palette walks every one of them to put each back where it was. Two
guards followed, and neither is optional: the animation's skeleton tag has to
name a node this model actually has, and the movement is measured against the
model's own size. A shift on the order of the span is a pose; nought is a
no-op; many times larger is the spike this project drew once already.

## Next

- **#24 — paint each primitive range separately.** The data is already recorded:
  each part's `firstIndex`, `indexCount` and its own material. What is missing is
  N draw calls over those ranges in all three backends, and a per-part texture
  fetch that survives the browser's pending reads.
- **#22 — walk the archive in fewer reads.** 1,529 entries at one round trip
  each.
- **Sample the pose on a clock.** The tick posed at is nought, hard-coded. What
  is read supports any tick — the keyframes and their times are all kept — so
  what is missing is a caller that advances one.

## Known soft spots

- **The find-and-redirect path has no fixture.** The test disc holds no skinned
  mesh, so `make verify` cannot catch a regression in the search that finds one
  or the redirect that draws it — only the real disc exercises those. A skinned
  container in `scripts/makeTestDisc.sh` would close it, and it would also give
  the pose path something to run against, which it likewise has no fixture for:
  `verifyAnimationReader` covers the format thoroughly and covers the palette,
  the tag check and the skinning composition not at all.
- **Past the rest pose, the animation is whichever the scan reaches first.** The
  named rest pose is tried first; if it is missing, the fallback opens indexed
  animations until one names this skeleton, so which one poses the Sim then
  depends on index order, and up to 64 are opened looking for it.
- **Keyframes are sampled on straight lines.** The tangents are in the file and
  are read past, not followed. Right at every keyframe, close between them, and
  wrong in a way that will matter as soon as anything samples a tick that is not
  a keyframe's.
- **Inverse kinematics chains are counted and skipped.** Every run says how
  many, which is what stops the omission from being invisible: the animation
  above carries four.
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
