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

`make web` needs `wasm-ld`, which is the `lld` package on Arch and
`lld-<version>` on Debian. Without it the compile succeeds and the link fails
with `posix_spawn failed`, which reads like a compiler problem and is not.

To run the web build, serve `build/web/` over HTTP and open it —
`python3 -m http.server 8173 --bind 127.0.0.1` is enough. `localhost` counts as
a secure context, so WebGPU works without TLS. Drop an ISO on the page.

The Linux binary takes:

| Argument | What it does |
| --- | --- |
| `--disc=PATH` | An ISO **or a directory**. Which it is is decided by looking, not by the path's shape, so a mounted CD and a rip both work. |
| `--inspect-disc=PATH` | Lists what is on the disc and stops. No window, no rendering. |
| `--check` | Headless self-check; no window. It does **not** load a disc. |
| `--quiet` | Silences the periodic profiler line. |
| `--graphics-memory-mebibytes=N` | Pretends the driver has that much, so a small device can be simulated on a large machine. |

Shell note, since it has cost time twice: `--disc=~/path` does not work. Neither
zsh nor bash expands a tilde after `=` in an ordinary argument, so the engine
gets the literal string and says it cannot open that disc. Use `"$HOME/…"`.

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
6. If what was drawn has a skeleton, **assemble a whole Sim** from the parts the
   game names, painting each part separately.
7. **Index every package for animations** and play the first that stands on its
   own and targets this skeleton.

Every stage says what it saw. A run's log is meant to be self-describing enough
that this document is not needed to interpret it.

## A whole Sim

This is the frontier. A Sim is not one model: it is a skeleton and three things
that skin to it, and openTS2's own base case names them — `auskel_cres`,
`amBodyNaked_cres`, `amFace_cres`, `amHairBald_cres`. All four are on this disc.

```
engine: 4 of 4 of a whole Sim's parts are on this disc by name
engine:   amBodyNaked_cres — 1173 vertices, 1768 triangles, wearing ambodynaked_nude_s1
engine:   amFace_cres      —  521 vertices,  737 triangles, wearing uuface_browbushy_brown
engine:   amHairBald_cres  —  144 vertices,  244 triangles, wearing amhairbald_skin_s1
engine: a whole Sim — 3 part(s) joined into 1838 vertices and 2749 triangles across 3 range(s)
engine: hung on auskel_cres — 125 node(s)
engine:   part 0 painted with ambodynaked-nude-s1 at 1024x1024
engine:   part 1 painted with amface-s1 (overriding what its shape bound) at 512x512
engine:   part 2 painted with umhairbald-skin-s1 at 512x512
engine: posed 1838 of 1838 vertices over 63 bone(s); it moved by 0.030 against a model 1.879 across
engine: that was the rest pose ... so the pose composes the way the game does
```

Four things about that are not obvious and each was got wrong first.

**A scenegraph reference is disc-global, not package-local.** All three trees
are in `Sims06.package` and not one of the shapes they name is. Resolving
through the package found three trees and zero shapes; the chain has to ask the
index, which is how the game's own content manager resolves them.

**A shape does not name a container.** It names a geometry **node**, and that
node references the container. Looking for a container by the shape's mesh name
finds nothing at all.

**A material binds to a primitive by name, not by position** — and a shape lists
more materials than the part has parts. A face shape carries the face, the
brows, the eyes and the lips, so taking the first binding paints the face with a
bushy brown eyebrow. Both names must be non-empty, too: an unnamed primitive and
an unnamed binding compare equal, which is two blanks agreeing rather than a
match. `discContent.c` already carried this warning before any of it was
written, and it got made anyway.

**The base face resource really does bind its face to a brow material**, because
it cannot know which face a Sim has. The game overrides it per subset. The
override here takes the tone from whatever the body ended up wearing — `s1` out
of `ambodynaked-nude-s1` — and asks for `amface-s1`, so a disc toned differently
follows its own naming. It stands in for real skin-tone resolution.

## The skeleton, the bind pose, and posing

Skinning is `Σ weight · bonePose · inverseBind · v`. The mesh on the disc **is**
in its bind pose, so at rest every pair multiplies out to the identity and a
correct skin moves nothing. Applying world transforms alone draws a Sim's face
with a limb stretched out of it.

**Bone numbers are the identifiers nodes carry, not positions in the node
list.** An earlier version of this document said small values meant positions
and large ones meant identifiers; that was **wrong**. Real identifiers are
small — this disc's face names bones 7, 6 and 5. What is large is `0x7FFFFFFF`,
the sentinel a node carries when it is *not* a bone.

**The inverse bind is in the file.** A GMDC carries its own pose-transform array
after the primitives, one quaternion and translation per bone, numbered the way
the primitives' bone lists are numbered. It holds the **inverse** bind, measured
rather than assumed: `world x stored` came out 0.000 from the identity while
`stored` sat 1.666 from world. So there is no matrix inverse in the engine and,
on this evidence, none is needed.

**The rest pose is the instrument.** `a-pose-neutral-stand_anim` is very nearly
the pose the mesh was authored in, so posing by it must move almost nothing. It
is asked for by name before the scan, and the run says outright whether the
result was what it had to be. It is the only animation on the disc whose correct
outcome is knowable in advance, and it has caught the wrong-skeleton class of
error twice. **Run it before trusting any change to the palette, the bind pose,
the bone numbering or the Euler convention.**

Know its limit as well: it is a single keyframe with nothing to interpolate, so
it passes whatever the interpolation does. See the tangents below.

## The animation

Played on the engine's clock. A tick is an eight hundredth of a second — the
format's own `FramesPerTick / 24` — computed from the clock each frame rather
than accumulated, so a dropped frame skips ahead instead of slowing the
animation and a long run cannot drift.

Skinned on the processor, not blended on the graphics one, which the note on
`geometryMeshApplySkin` would rather it were. The device ladder decides it: the
floor has no programmable shading at all and the software rasterizer is the
expected backend there.

**An animation marked to-object is authored in that object's space.** The mark
is `2o` and the letter before it says who — `a2o` adult, `t2o` teen, `c2o`
child. Played with no object in the scene the Sim tumbles through empty air with
every limb perfectly sensible: a pose missing its other half, not a pose gone
wrong. Those are skipped, out loud. Matching only `a2o` is not enough; it lands
on a teen at a mirror instead.

**The tangents are per second, and are still not followed.**
`animationMeasureTangentScale` totals what each slope accounts for across an
interval against the change that interval makes; on two real animations it came
back 594 over 602 intervals and 784 over 936, against 1 for per tick and 800 for
per second. Applied as a Hermite at that scale, a Sim that had been moving
fluidly flew about. So the unit was not the only unknown — which slope belongs
to which side of a keyframe is also unestablished, and openTS2 carries a "fix
tangentin and tangentout values" note over the same code. Sampling is a straight
line: exact at every keyframe, close between them.

The check asserts the curve is **not** used, at the scale the measurement
indicates. Restoring it means deliberately changing a check rather than quietly
passing one — which is exactly how the wrong curve shipped, twice.

## Drawing

`renderSetMesh` uploads a mesh; `renderUpdateMeshVertices` re-sends only its
vertices; `renderSetPartTexture` gives one of the mesh's parts its own image.
The ranges come from `GeometryPrimitive`, which has always recorded them.

**`renderSetMesh` releases everything the previous mesh took, textures
included.** Using it as the per-frame path leaks the graphics ledger — fifteen
megabytes in twelve seconds — and rebuilds a shader every frame. Using it after
per-part textures are set throws them away, which put a Sim's body back in a
face the moment it started moving.

The three backends differ deliberately: OpenGL ES 2.0 and WebGPU paint parts
separately; the software rasterizer ignores textures entirely, because the
hardware at the floor of the ladder cannot afford to sample per pixel.

## The web build

Everything above runs in a browser, including the whole-Sim assembly. Two things
are specific to it.

**The disc store holds exactly one delivered range, and consuming it clears the
hold.** So anything reading a chain must do **exactly one read per step** and
record what it learned. Code that re-reads its chain on every attempt cannot
converge: it reads the tree, pends on the shape, comes back to find the tree no
longer held, pends on that instead, and alternates forever. That is written on
the skin search in `engineCore.c`, and the Sim assembly was nonetheless built
doing thirteen reads per step. It retried fifteen thousand times and got
nowhere. It is a stepper now.

**Nothing native exercises that.** A file descriptor never pends, so every
native run of the broken version was fine. A change to any multi-read path needs
the browser to test it.

## Known soft spots

- **The WebGPU build shows banding on the Sim's arm that the OpenGL ES build
  does not.** Unexplained. What is ruled out: all three materials resolve, all
  three textures decode at full resolution, all three bind groups build with no
  fallback logged, the sampler repeats on both axes as the GL one does for
  power-of-two images, the pixels come from shared C code, and the mesh is not
  re-uploaded after the parts are painted. First thing to look at next.
- **The web build is slow**, minutes rather than seconds. The store answers one
  read per step and a step is a frame, so indexing 1,411 packages costs at least
  1,411 frames before anything else. Letting several reads be outstanding at
  once is where the time is; it would also make the one-read-per-step rule a
  performance choice rather than a correctness one.
- **The web build has no folder support.** The engine already accepts a
  directory natively, so the catalogue side is done; `webDiscStore` is what would
  change. `<input webkitdirectory>` gives a `FileList` and works everywhere;
  `showDirectoryPicker()` gives a persistent handle in Chromium.
- **Inverse kinematics are counted and skipped.** Every run says how many; the
  animation currently played carries two.
- **The find-and-redirect path and the Sim assembly have no fixture.** The test
  disc holds no skinned mesh and no Sim, so `make verify` cannot catch a
  regression in either. A skinned container in `scripts/makeTestDisc.sh` would
  close the first.
- **The parts' bind poses agree to 0.015, not to nought**, over 195 bones. Small
  enough that nothing rests on it, measured every run rather than assumed.
- **Two unexplained singletons** in the index, unchanged for many runs:
  `refused 1 meshes — not a scenegraph resource`, and `1 would not be read`.

## How to be wrong here

The disc has now contradicted a reasonable inference more than a dozen times,
and the fix has always been the same: stop reasoning about what it should
contain and make the engine report what it saw. Beyond that, this session added
four failure modes worth knowing before repeating them.

**A test that cannot fail is not evidence.** The Hermite curve was added, the
rest-pose check was recorded *at the time* as unable to discriminate it, and it
shipped on that check anyway. If a check would pass either way, it is not
testing the thing.

**Two screenshots are not a diagnosis.** The camera orbits at
`elapsedSeconds * 0.6f`, so captures at different moments show different angles.
A correctly posed head seen from the side — a face mesh has no modelled skull
behind it — looks exactly like a torn one. That cost a full round of misdirected
work. Worse, three separate bugs this session were visible **only in motion**: a
body thrashing, part textures lost on the first pose, and a browser livelocking.
A still frame cannot see any of them.

**Read the warnings already in the tree.** Two of the day's bugs were things
this codebase had written down: materials bind by name not position, and exactly
one read per step against a pending store. Both comments were read, and in one
case copied verbatim, before the rule in them was broken.

**An invariant that holds inside one file does not survive being merged.** A
component index means something only within its own container, so every part's
first primitive draws from component nought. Joined without shifting them,
`geometryMeshApplySkin` sees two primitives over one component, skips the second
to avoid transforming shared vertices twice, and leaves a Sim's head behind
while its body lies down — reported as `posed 1173 of 1838`, which is exactly
the body's count and the number that gave it away.

The logs are verbose on purpose. That is the method, not clutter.
