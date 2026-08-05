# Handoff

Where the engine is, what it does on a real disc, and what is worth doing next.

Written to be read by someone who has none of the conversation that produced it.

## Building and running

```sh
make            # the Linux build, into build/linux/victoriaSims
make web        # the WebAssembly build, into build/web/
make verify     # 18 C suites; all should say "checks passed"
make verifyWeb  # the wasm module and the browser runtime, under node
make check      # proves no allocator symbol is linked in
```

`make verify` does not include `verifyWeb`: that one needs the module built and
a `node` to run it under, and a machine with neither should still get the rest.
Run both. The two defects the web checks catch — an odd index count, and a pose
that stripped a Sim of its skins — each reached a browser because for a while
nothing ran them at all.

`make armv5`, `make oabi` and `make armv7` need an `arm-linux-gnueabi-gcc`
cross compiler. Without one they fail at the first object file with
`commande introuvable`, which is a missing toolchain and not a broken build —
they are compile-only targets that prove the portable core stays portable.

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
| `--still-camera[=DEGREES]` | Stops the camera orbiting and holds it at an angle. Half a turn by default, because **nought is a Sim's back**. |
| `--still-pose[=TICK]` | Holds the animation on one frame instead of playing it. |
| `--morph=N` | Holds deformation channel N at full strength instead of sweeping. The run's log lists the channels and their numbers. |

Use both together whenever anything is being judged by eye across frames. An
orbiting camera makes two captures two different views, and the difference gets
attributed to whatever changed in the code — that has cost a round of work here
once already. The orbit stays the default because a still model hides its own
silhouette, and three of this engine's bugs were visible only in motion.

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

**A material binds to a primitive by name, not by position.** Both names must be
non-empty, too: an unnamed primitive and an unnamed binding compare equal, which
is two blanks agreeing rather than a match. `discContent.c` already carried this
warning before any of it was written, and it got made anyway.

**The base face resource really does bind its face to a brow material**, because
it cannot know which face a Sim has. The game overrides it per subset. The
override here takes the tone from whatever the body ended up wearing — `s1` out
of `ambodynaked-nude-s1` — and asks for `amface-s1`, so a disc toned differently
follows its own naming. It stands in for real skin-tone resolution.

An earlier version of this document said a face shape lists several materials —
face, brows, eyes, lips — and that taking the first was how the face ended up
wearing an eyebrow. **That was wrong**, and it is worth leaving the correction
here because it was a comfortable story. Each of a Sim's three shapes names
exactly one geometry node and exactly one material binding:

```
engine:   amFace_cres names 1 geometry node(s) and 1 material binding(s)
engine:     node amFace_tslocator_gmnd at detail 0 — on this disc
engine:     binds face to uuface_browbushy_brown
```

There was never a list to choose badly from. The brows, eyes and lips are not in
that shape at all — they are catalogue entries, and the section on the catalogue
below is where they actually live.

## Deformation

A container declares its deformation channels and carries per-vertex data to
move by them. Both are read; a Sim's face deforms and its body does not.

```
amFace      27 channels, 4 delta sets, a map over all 521 vertices reaching 352
amBodyNaked  3 channels, 2 delta sets, a map over all 1173 vertices, all nought
amHairBald   none

channel 1 botmorphs/fatbot  moves 1173 vertices, furthest by 0.094
channel 2 botmorphs/pregbot moves 1173 vertices, furthest by 0.086
```

**A body's map is empty and its deltas are not, and the slot is the channel.**
This is the one inference in the deformation path and it is flagged as one —
`morphChannelsInferred` on the mesh, and the run says `channels were INFERRED
from an empty map` in as many words.

The evidence, because an inference deserves its working shown. Every body mesh
on the disc — the nude one and eight outfits across ages, genders and ages —
carries a map of all noughts while its delta sets hold displacements of three
hundredths. Three other places were ruled out first: a catalogue entry's key
list holds only CRES, SHPE and TXMT, so no morph resource; a body's shape names
exactly one geometry node, so no morph mesh; and the container's unused elements
are normal deltas and indices, not a second addressing scheme.

What separates a body from a face is arithmetic. A face declares twenty-six
channels and carries four delta sets, so a vertex must say which four of the
twenty-six its slots stand for — a map is the only way to say it. A body
declares one or two and carries exactly that many sets. Nothing to
disambiguate, and the file does not bother.

**If a Sim ever deforms into something that is not a fatter Sim, that rule is
the first thing to doubt.** `verifyGeometryReader.c` holds both halves: an empty
map over deltas is filled in, and a map that says something is left alone —
the second matters more, because firing the inference on a face would silently
re-address every channel it has.

**The channel list keeps a blank first entry, and compacting it would be
catastrophic.** A slot's byte of nought means "this slot moves nothing", so
channel nought can never be referred to. A reader that closed the gap would
shift every real channel down one and rename all of them.

**The map packs slot nought in the word's MOST significant byte.** The bone
assignment word a few elements away packs slot nought in its LEAST significant.
Two packed words in one container read in opposite directions, and nothing in
the file says so. `verifyGeometryReader.c` writes a map that yields nothing at
all when read backwards; five checks fail on the reversal.

**Channels are renumbered across a join.** Every part calls its first real
channel 1, so concatenating the lists without shifting the slots would let a
body's weight deform a face. Nought stays nought. The channel is widened from
the file's byte to a halfword on the way in, so a join cannot overflow it.

**A morph runs BEFORE the skin, between the bind-pose restore and the palette.**
A morph changes the shape the model was *authored* in — a fatter body is a
different bind pose, not a differently posed one — so its deltas are in rest
space. Applied after the skin they land further out the further a limb has
swung. There is no accumulation path: `restoreBindPose` copies the kept resting
positions back every frame before either runs.

Normals are not deformed. The file carries deltas for them and they are not read
yet, so a morphed vertex keeps its resting normal.

The engine sweeps one named channel at a time, four seconds each, and says which
in the log. That was every channel at once to begin with, which proved the data
reaches the mesh and nothing else — nine channels drive the mouth, so at full
strength they fight and the result says nothing about whether any one of them is
right. A caricature is not an instrument.

## The catalogue

Where the rest of a Sim is. The four names a whole Sim is built from above are
hardcoded, and everything else — clothing, hair, brows, eyes, lips, face
archetypes — is described here. It is the first resource in this engine that is
not part of the scenegraph: a scenegraph resource points at another scenegraph
resource, while a property set names things and leaves the resolving to whoever
reads it.

An entry carries eighteen properties:

```
age; category; creator; family; fitness; flags; gender; genetic; hairtone;
name; numoverrides; outfit; resourcekeyidx; shapekeyidx; shoe; skintone;
species; type=skin
```

`type=skin` covers everything selectable — outfits, hair, brows, eyes — and
**`category` is the body slot** that tells them apart. The chain to a mesh is:

```
skin entry (0xEBCF3E27) → shapekeyidx → key list (0xAC506764) → SHPE → GMND → GMDC
```

and the last three of those the engine already walks.

```
CASIE_efbodynightgown_floralpink       key 1 of 3 — a shape on this disc
afhairpagepunk_brown                   key 1 of 5 — a shape on this disc
embodypajamas_grey                     key 1 of 3 — a shape on this disc

followed 334 of 600 to a shape — 115 indexed past the end of a list,
151 named no mesh at all (an overlay or a tone)
```

**The property set has two format traps.** Its strings are prefixed by a flat
four-byte length, where the scenegraph's carry one to five bytes with a
continuation bit — either rule applied to the other's stream yields a plausible
length. And a boolean is one byte in a stream where every other scalar is four,
so reading it wide swallows the next property's type word.

**The key list has a version trap.** Version 1 keys are three words, version 2
four, the extra being the instance's high half. A version 1 list read as version
2 takes the next key's type as an instance half and is wrong about every key
after the first while still producing plausible numbers. A sentinel at the front
says which; a list without one is version 1, its count first.

**A sidecar is matched on group AND instance, and that was measured.** On
instance alone, 334 of 600 entries found a list and every key at the wanted
index was a shape — which looks like a clean result. On both, 485 found one: the
same 334 shapes plus 151 keys of another type. The stricter match finds more,
and the extra ones are not errors — an entry of kind skin covers overlays and
tones, which have no mesh to name. Counting those apart from a shape that could
not be found is the difference between a diagnosis and a tally.

**886 of the entries met are the XML spelling** of the same resource type, which
this does not read. Counted and reported rather than passed over, because a
reader silently ignoring a third of the catalogue looks exactly like one that
had read it all. The kinds the reference names but the binary sample never
showed — `facearchetype`, `facemodifier`, `meshoverlay` — are the obvious place
to look for the body's missing fat data.

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

**Both backends had that defect, and fixing one did not fix the other.** On
OpenGL `renderUpdateMeshVertices` became a `glBufferSubData`; on WebGPU it kept
calling `hostUploadMesh`, which creates new buffers, empties the part ranges and
destroys the part textures. So the web build lost its three skins on the first
animated frame and drew the whole Sim in one call under whatever was left —
which is what the banded arm was. The host now has an `updateMeshVertices` of
its own that writes over the buffer already there and refuses a vertex count
other than the one it holds. `tests/verifyRuntimeUpload.mjs` pins it.

The three backends differ deliberately: OpenGL ES 2.0 and WebGPU paint parts
separately; the software rasterizer ignores textures entirely, because the
hardware at the floor of the ladder cannot afford to sample per pixel. It also
needs no vertex update at all — it keeps the mesh by pointer and reads its
positions each frame, and the engine poses in place. That is correct by a
coupling rather than by construction, and is written down where it lives.

**What the software backend shades with is `mathSquareRoot`, and it did not
used to be.** It normalised its face normals with four Newton steps started at
one, inline. Newton only doubles its digits once it is near the root; before
that it halves the error. A Sim is 1.879 units across with 1,838 vertices, so
its cross products are about a millionth, and four steps returned 0.063 for a
root of 0.001 — every normal a sixtieth of unit length, every lambert near
nought, the whole body flat at the 0.28 ambient floor. The teapot's triangles
are ten times larger, where that loop is right to within eight percent. The
square root is a range-reduced one in `freestandingRuntime.c` now, exact to a
part in ten million at every scale, and `tests/verifyFreestandingRuntime.c`
pins it there — including at a Sim's scale specifically.

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

- **The web build is slow**, minutes rather than seconds. The store answers one
  read per step and a step is a frame, so indexing 1,411 packages costs at least
  1,411 frames before anything else. Letting several reads be outstanding at
  once is where the time is; it would also make the one-read-per-step rule a
  performance choice rather than a correctness one.
- **The web build has no folder support.** The engine already accepts a
  directory natively, so the catalogue side is done; `webDiscStore` is what would
  change. `<input webkitdirectory>` gives a `FileList` and works everywhere;
  `showDirectoryPicker()` gives a persistent handle in Chromium.
- **The slot-is-the-channel rule for bodies is inferred, not read.** Measured
  and flagged, and the checks pin both halves — but no part of the format says
  it, and a body deforming into the wrong shape is where to start if one does.
- **886 XML catalogue entries are skipped.** Reading them needs an XML property
  set reader this does not have. `facearchetype`, `facemodifier` and
  `meshoverlay` never appeared in the binary sample and are presumably there.
- **115 catalogue entries index past the end of their key list.** Unexplained,
  and worth a measurement rather than a rationalisation: it may be a second
  sidecar, a different index property, or entries whose shape index means
  nothing. It is the first thing to look at in the catalogue.
- **Nothing wears anything yet.** The chain from a catalogue entry to a shape
  resolves; choosing an entry by `category` and swapping a Sim's part for it is
  the step not taken.
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
contain and make the engine report what it saw. Beyond that, these sessions
added nine failure modes worth knowing before repeating them.

**A test that cannot fail is not evidence.** The Hermite curve was added, the
rest-pose check was recorded *at the time* as unable to discriminate it, and it
shipped on that check anyway. If a check would pass either way, it is not
testing the thing.

**Two screenshots are not a diagnosis.** The camera orbits, so captures at
different moments show different angles — `--still-camera` and `--still-pose`
exist because of this and should be on whenever anything is judged by eye.
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

**A numerical method has a range, and the comment above it is not it.**
`mathSine` said "accurate to roughly 1e-6" and was off by seven thousandths at
a half turn — the far end of the range it reduced into, which is exactly where
a truncated series is worst. The face-normal loop said "good to a few parts in
a thousand" and was off by sixty times at the scale the disc actually produces.
Both claims were true where somebody had looked and false where the work went.
An approximation needs testing at the ends of its range and at the magnitudes
its callers really pass, not at one.

**A shared name is not a shared implementation.** The WebGPU banding was hunted
for a whole session — materials, decoders, samplers, bind groups, shaders — and
listed as unexplained with six things ruled out. One of those six was "the mesh
is not re-uploaded after the parts are painted", which was believed because the
call was named `renderUpdateMeshVertices` and the OpenGL one with that name does
not re-upload. The WebGPU one did. Every backend implements the same header;
that is a promise about the signature, not about the behaviour. When two
backends disagree, read the one that is wrong rather than the interface both
claim to satisfy — and when a fix is made on one backend, check whether the
defect it fixed exists on the others.

**A silent truncation reads as an absence.** The resource index clamped a
request for nine types down to eight and said nothing, so the sidecars a
catalogue entry needs were never indexed — and the log reported the disc held
none of them. Every symptom pointed at the disc; the cause was one `?:` in the
index. Anything that bounds what a caller asked for must say so. `log()` the
drop, count it, put it in the report — a limit that cannot announce itself is a
lie told once per run.

**A caricature is not an instrument.** The deformation sweep drove all
twenty-six face channels at once, which proved the data reaches the mesh and
proved nothing else: nine of them drive the mouth, so at full strength they
fight over the same vertices and the mangled result cannot say whether any
single channel is right. Driving one named channel at a time makes a frame
checkable against a claim — `l_growl` had better pull the left side.

**A test that crashes instead of failing reports nothing.** A check dereferenced
a lookup without guarding it. When the property genuinely went missing — which
is precisely when the check was supposed to speak — it segfaulted, printed no
FAIL line, and the deliberate regression that should have proved the test worked
came back looking clean. It took a sanitiser to find that the crash was in the
test rather than in the code under it. Guard every pointer a check follows.

There is a fourth worth folding in here rather than giving its own heading: the
arena is never zeroed, so **a struct field added to an existing record must be
initialised where its neighbours are**. Three fields were added to
`ComponentSpan` and none was cleared; a garbage count then sized an allocation
and asked for 3.9 GB against a mesh of a few hundred vertices. The refusal said
only "not enough arena space", which costs a run of the disc per guess — it now
reports the bytes it wanted, and that number named the array immediately.

The logs are verbose on purpose. That is the method, not clutter.
