# Handoff

Where the engine is, what it does on a real disc, and what is worth doing next.

Written to be read by someone who has none of the conversation that produced it.

## Building and running

```sh
make            # the Linux build, into build/linux/victoriaSims
make web        # the WebAssembly build, into build/web/
make verify     # 19 C suites; all should say "checks passed"
make verifyWeb  # the wasm module and the browser runtime, under node
make check      # proves no allocator symbol is linked in
```

`make verify` does not include `verifyWeb`: that one needs the module built and
a `node` to run it under, and a machine with neither should still get the rest.
**Run both.** `verifyWeb` is not only the browser's test — it is the only thing
that drives a whole disc load, so the Sim assembly, the wardrobe and the paint
are checked there and nowhere else. The defects it catches reached a browser, or
a screen, because for a while nothing ran them at all.

Changing `testAssets/` means running `scripts/makeTestDisc.sh` and updating
`testAssets/manifest.sha256`; `scripts/checkNoGameData.sh` fails otherwise, and
it consults git rather than the filesystem on purpose.

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
| `--wear=NAME` | Dresses the Sim in the catalogue entry whose name holds NAME. A preference, not a filter: parts nothing matching was offered for still wear whatever the catalogue offered them. The run names eight alternatives per part, so the next run's argument comes out of the last one's output. |

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
7. **Read the catalogue**, and dress the Sim in what it names — a face, a hair,
   and either a whole body or a top and a bottom — painting each subset with
   the material that entry names rather than the one its shape binds. Then join
   and paint it again.
8. **Index every package for animations** and play the first that stands on its
   own and targets this skeleton.

Every stage says what it saw. A run's log is meant to be self-describing enough
that this document is not needed to interpret it.

## A whole Sim

The base case, and the thing everything below is built on top of. A Sim is not
one model: it is a skeleton and three things
that skin to it, and openTS2's own base case names them — `auskel_cres`,
`amBodyNaked_cres`, `amFace_cres`, `amHairBald_cres`. All four are on this disc.

```
engine: 4 of 4 of a whole Sim's parts are on this disc by name
engine:   amBodyNaked_cres — 1173 vertices, 1768 triangles, wearing ambodynaked_nude_s1
engine:   amFace_cres      —  521 vertices,  737 triangles, wearing uuface_browbushy_brown
engine:   amHairBald_cres  —  144 vertices,  244 triangles, wearing amhairbald_skin_s1
engine: a whole Sim — 3 part(s) joined into 1838 vertices and 2749 triangles across 3 range(s)
engine: hung on auskel_cres — 125 node(s)
engine:   range 0 of amBodyNaked_cres painted with ambodynaked-nude-s1 at 1024x1024
engine:   range 1 of amFace_cres painted with amface-s1 (overriding what its shape bound) at 512x512
engine:   range 2 of amHairBald_cres painted with umhairbald-skin-s1 at 512x512
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
**`outfit` is the body slot, and `category` is not** — this document said
category was, a probe was written against it, and then outfit was ruled out too
on a sample of a hundred and fifty entries that happened to be all one kind.
Widening the sample to two thousand settled it:

```
0x01 — 1123 entr(ies), 1118 reaching a mesh — afhairpagepunk_brown        hair
0x02 —  287 entr(ies),   21 reaching a mesh, 151 painting — CASIE_tmface_s3  face
0x04 —  155 entr(ies),  155 reaching a mesh — amtopjackettshirthang_grey  top
0x08 —  343 entr(ies),  343 reaching a mesh — efbodynightgown_floralpink  whole body
0x10 —   89 entr(ies),   89 reaching a mesh — embottomnaked_CASmannequin  bottom
0x18 —    3 entr(ies) — a bottom and a body at once
```

**The face slot is 0x02, and what is in it is faces.** Every named entry in it
is a face mesh, one per age, gender and skin tone:

```
face slot — CASIE_tmface_s1, key 1 of 3 is a shape, found on this disc
face slot — CASIE_emface_s3, key 1 of 3 is a shape, found on this disc
face slot — buface_s4, tfface_alien, CASIE_puface_mannequin, …
```

`<age><gender>face_<tone>`, all reaching a shape on this disc. **This is how a
Sim gets the right face** — the engine still hardcodes `amFace_cres`, and the
catalogue can name the correct one for an age, a gender and a tone instead.

**The entries in that slot that reach no mesh are the groupings, not overlays.**
It is tempting to read "151 painting one instead" as a hundred and fifty one
brows and lips. It is not: those are the unnamed grouping entries, whose key 1
is the type-nought hole. That misreading survived several rounds here because
the count was consistent with the guess.

**So brows, eyes and lips are not among the catalogue's skin entries at all.**
Where they are is not yet known. Two leads, both from things the disc has
already said: the base face binds a material called `uuface_browbushy_brown`, so
brows exist as materials with a legible naming scheme; and better than a fifth of
the catalogue is the XML spelling this reader skips, which is where the kinds a binary sample
never showed — `facearchetype`, `facemodifier`, `meshoverlay` — would live.

The lesson to carry, since it cost three wrong answers in a row: a homogeneous
sample of a clustered catalogue says where the walk started, not what the disc
holds. Widen before concluding.

**`category` is the other one, and it is not a body part at all.** It is the
set of outfit categories a thing belongs to — everyday, formal, swimwear — and
the disc says so plainly:

```
slot 0x0000037F — 93 entr(ies), 88 reaching a mesh, such as tmhairshortspikey_black
slot 0x00000020 —  8 entr(ies),  8 reaching a mesh, such as efbodydresslongformal_celadon
```

`0x37F` is nine bits at once, and it is hair — because hair is worn with every
outfit. The single-bit values are garments available in one category each.
`outfit` is the property that says which part of a Sim a thing dresses. Both are
tallied, side by side, because assuming which was which cost three wrong answers
in a row.

The other half of that run is the more interesting one:

```
slot 0x00000000 — 96 entr(ies), 0 reaching a mesh, 47 painting one instead, such as (unnamed)
```

Ninety six of two thousand belong to no outfit category, reach
no mesh, and carry no name. Things belonging to no outfit category are things
that are not outfits — brows, eyes, lips, skin tones — and **none of them
reaches a mesh**, which is the first real evidence that the rest of a face is
paint rather than geometry. Four of them are dumped in full on every run, since
that pattern is equally what a misread property would look like.

The chain to a mesh is:

```
skin entry (0xEBCF3E27) → shapekeyidx → key list (0xAC506764) → SHPE → GMND → GMDC
```

and the engine now walks all of it — to a shape, and from a shape onto a Sim.

```
CASIE_efbodynightgown_floralpink       key 1 of 3 — a shape on this disc
afhairpagepunk_brown                   key 1 of 5 — a shape on this disc
embodypajamas_grey                     key 1 of 3 — a shape on this disc

7773 catalogue entries on this disc, taking every 3 so the sample spans the lot
read 2000, 455 of them spelled as XML rather than the binary form
followed 1884 to a shape — 0 had no key list, 59 indexed past the end of one,
2 named a shape the index does not hold, 54 named no mesh at all
```

Those counts are from the widened sample. An earlier version of this document
carried the same lines at 600 entries — 334 followed, 115 past the end, 886
XML — and every one of those numbers was a property of where the walk started.
**Quote a count with the sample it came from or not at all.**

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

**455 of the 2000 entries met are the XML spelling** of the same resource
type, which this does not read. Counted and reported rather than passed over,
because a reader silently ignoring a fifth of the catalogue looks exactly like
one that had read it all. The kinds the reference names but the binary sample never
showed — `facearchetype`, `facemodifier`, `meshoverlay` — are the obvious place
to look for the body's missing fat data.

## Wearing it

A Sim now wears what the catalogue names — a whole body, or a top and a bottom
together, plus a face and a hair. The chain resolved for a long time before
anything was ever put on the end of it; this is that step.

```
engine: the wardrobe was offered 1884 entr(ies) that reach a shape and
        dresses 3 of 3 part(s), at the tone s1
engine:   passed over 0 unnamed, 260 dressing a part this Sim has not got,
          1351 authored for another age or gender, 4 already worn,
          and 266 no better than what the part had settled on
engine:   amBodyNaked_cres — wearing ambodyhoodedsweatshirtpants_green
engine:     or any of — ambodypirate_black; ambodyunderwear_blackbriefs;
                        ambodyfirefighter_eurostyle; ambodyhiphophood_orange; …
engine:   amHairBald_cres gives way to amhairshortcenterspike_red
              — 144 vertices become 453, 453 of them weighted
engine: a dressed Sim — 3 part(s) joined into 2494 vertices across 5 range(s)
```

**A Sim wears either a whole body or a top and a bottom, never a mix and never
half.** They are the same volume of Sim described two ways — joining both puts a
pair of trousers through a pair of legs that are already there, and what shows
is decided by whichever triangle the rasterizer reaches last. So the wardrobe
settles an *arrangement* before anything is loaded:

```
engine:   a top and a bottom, which between them replace the whole body —
          so the body it was assembled with is not drawn
engine:   amBodyNaked_cres — chosen but not worn: ambodyhoodedsweatshirtpants_green
engine:   a top    — wearing amtopcowboyshirt_brownstriped
engine:   a bottom — wearing ambottomlongshorts_blueplaidtannavy
```

The pair wins when both halves turned up, because two garments chosen
independently say something one cannot. A top with no bottom beside it is
**chosen and not worn** — half a pair is a Sim in a shirt and nothing else — and
the report keeps the two words apart, because a choice that was made and not
drawn is a different state from one that was never available. Naming a
whole-body garment with `--wear` settles it the other way; without that rule the
pair wins nearly always and the flag can never be pointed at a whole body.

**A top and a bottom have no undressed form.** The assembly's hardcoded names
are a skeleton, a naked body, a face and a bald head — there is no
`amTopNaked_cres` to start from, so those two parts exist only if the catalogue
puts something there. That is why parts are indexed by **identity** and not by
the order they loaded: three of five slots have a base mesh and two do not, and
packing them would make the texture override and the range map mean different
things on different runs.

**It runs after the Sim is assembled, painted and on screen, not before.** Two
reasons, and the first is not optional: the tone a face has to match is read off
the texture the body ended up wearing, so there is nothing to choose a face for
until a body has been painted. The second is that a Sim that appears and then
dresses has two states one run can tell apart, and a Sim that was never
undressed has one.

**The choosing is in `engine/source/wardrobe.c`, on its own, because it is the
only part of this with judgement in it.** Everything else in the chain is
arithmetic the disc either agrees with or does not, and a mistake shows up as a
refusal on the first run. A wrong choice resolves perfectly and draws a Sim
wearing somebody else's body. There is no refusal to read and the disc cannot be
asked whether the answer was right, so the rule lives where
`tests/verifyWardrobe.c` can state a claim and then break it.

Four rules, and each is a way this has been or could be wrong:

**A mesh must be authored for the skeleton it hangs on.** Everything is hung on
`auskel_cres`, so an entry must be named for an adult male — `ambody`, `amface`,
`amhair`, `amtop`, `ambottom`. A child's body resolves perfectly and comes apart
on the first pose, which does not read as wrong clothes. 1578 of 1884 entries
are refused by this one rule, which is what a catalogue covering every age and
gender should look like from an adult male's point of view.

**The mark is matched anywhere in the name, not at the front.** Every CAS entry
on this disc is called `CASIE_amface_s1`, so anchoring finds nothing at all.

**A part will not wear what it already wears.** The base Sim is naked and bald,
and the catalogue names those very meshes. Taking one closes the whole chain and
changes nothing on screen, which proves the chain works and is indistinguishable
from a wardrobe that did nothing.

**The marker for that is `_nude`, and it is emphatically not `naked`.**
`amtopnaked_babybluetank` is a real garment: `topnaked` is the name of the
*mesh*, a bare torso, and the tank top is the texture painted on it. Refusing on
`naked` refuses most of the tops on the disc. `CASIE_amtopnaked_nude_s3` is the
bare one, and `_nude` is what says so.

**A slot naming two parts at once dresses neither.** `0x18` is a bottom and a
whole body together and there is no answer to which of the two it is; wearing
either half of it is worse than not wearing it. Three entries in the sample.

**The tone matters for a face and not for a garment.** A face is not worn over
skin, it *is* the skin, so one tone off is a head that does not belong to its
neck — a later entry displaces an earlier one when it matches the tone and the
earlier did not. A garment's colourway sits in exactly the same position in the
name (`ambodyswimwear_redbikini`) and means something else entirely. The tone is
matched as a whole trailing component, `_s1` and not `s1`, or `CASmannequins1`
matches.

**`--wear` is a preference and not a filter, and it was a filter first.** As a
filter it read perfectly and did the wrong thing: no hair and no face is named
after a garment, so asking for one refused both and the Sim came out dressed and
bald. Nothing is read during the walk — entries are only remembered — so
preferring costs exactly what refusing did.

### What colour it is

**A shape's material binding is one arbitrary colourway.** One mesh serves every
colour of a cowboy shirt, and the shape has to name something — so a Sim asked
for `amtopcowboyshirt_brownstriped` came out painted `amtopcowboyshirt_decogold`,
and `ambottomlongshorts_blueplaidtannavy` came out `navywhiteblack`. Which colour
*this* entry is, is in the entry:

```
numoverrides=0x00000001; override0subset=body;
override0resourcekeyidx=0x00000002; override0shape=0x00000000
```

`override<N>resourcekeyidx` indexes the same key list the shape came out of, so
resolving it costs no read at all — the list is already open. `override<N>subset`
is a **primitive's name**, which is how a material has bound here since the first
Sim, so this is the existing rule with a better source rather than a second
mechanism.

**Every property list this engine had ever printed in full was a grouping's.**
The unnamed entries carry eighteen properties and were dumped on every run for
months; a named one carries twenty-one, and the three extra are the whole
answer. A sample that never included the thing being asked about will describe
the format perfectly and leave out the part that matters — which is the
clustered-catalogue lesson again, in a different costume.

**The skin-tone stand-in now stands aside.** `simPartTextureStems` guesses a
face's texture from the body's tone because nothing better was available; where
a catalogue entry names its own material, that guess must not overrule the thing
it was standing in for.

**The report names eight alternatives per part.** Counts alone said a hundred
and fifty other garments fitted and named none of them, which left the flag with
nothing to be pointed at.

**`RENDER_PART_LIMIT` went from eight to sixteen.** A part there is a primitive,
not a body part: a firefighter's suit is two and his helmet is three, so a Sim
in a top, a bottom, a face and a hair reaches eight on the garments alone. Over
the cap a range is drawn under whatever its neighbour wears, which reads as a
garment bleeding onto a face rather than as a limit — the run says so now
either way.

Not yet chosen for: the sample carries no adult male face at the body's tone, so
the face is worn at whichever tone turned up and the run says so. A face is
still overridden to the body's tone texture regardless, so it looks right and is
not right.

### The bug that was waiting for this

**`renderSetPartTexture` is indexed by PRIMITIVE, and it was being handed a
PART.** The four hardcoded names carry exactly one primitive apiece, so for as
long as a Sim wore nothing the two were the same number and the difference could
not show. The first garment put on one had two primitives — a firefighter's suit
and the skin at his wrists — every texture after it landed one range early, and
a Sim came out with a green face.

The header had said "a position in the mesh's primitives array" all along. This
is the same failure as the component index across a merge and the same failure
as a shared name not being a shared implementation: **a number that has only
ever been tested where two meanings coincide has not been tested.** The paint
walks ranges now, each part contributes as many as it has, and a Sim beyond the
backend's eight says so rather than drawing the rest under its neighbour's skin.

## The fixture

There is a Sim on the test disc now, and none of it came from anywhere.

`scripts/makeSimFixture.py` writes `testAssets/scenegraph/sim_fixture.package`:
a skeleton of three bones, three parts weighted to it, and catalogue entries
naming replacements for them. It is boxes. It is not meant to look like
anything — it is meant to have the *structure* a Sim has, and `make verifyWeb`
drives the whole load over it headlessly, through the browser's one-read-per-step
handshake, in a couple of seconds.

Before this, the disc carried a teapot: one rigid model, no bones, no skeleton,
no catalogue entry. So the four-names lookup, the merge, the skeleton, the
wardrobe and the paint had no fixture at all — and three defects reached a
screen through that gap. Every assertion in the new block corresponds to one of
them, and each was checked by putting the defect back:

| Break | What fails |
| --- | --- |
| the merge drops a part | `joins the parts into one model`, and two more |
| the paint takes the shape's material | `paints a garment with the material its entry names` |
| the whole body is drawn under the pair | `and joins the dressed Sim again` |
| the tone rule stops displacing | `taking the face whose tone matches, not the one it met first` |

**The fixture is built to disagree with the bug.** Its body has **two
primitives and two materials**, because while every part had one a part index
and a primitive index were the same number and nothing could tell them apart. Its
catalogue offers the **wrong tone first**, so a reader that kept whatever it met
first would keep it. Its garment shapes bind a **deliberately wrong colourway**,
so taking the shape's binding cannot pass. A fixture that offered only the right
answers would agree with a reader that had no rules at all.

**Nothing is retail-derived, and the generator is the provenance.** A fixture
somebody hands you can only be taken on trust; one written by a script committed
beside it can be re-run and compared, and it reproduces byte for byte. That is
also why the generator is the file to read: it is the only place the seven
formats are written down rather than only parsed, and a disagreement with a
reader now shows up as a fixture that will not load.

**An object reference is three fields, not two.** A present byte, then a kind
byte, then the index — and the reader's early return on `present == 0` makes it
look like two from the bottom half of the function. Writing it as two made every
reference one byte short: a CRES read its first block correctly and then took
the middle of its second block for a block type. That is what authoring a format
catches that reading one does not.

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
- **455 of 2000 catalogue entries are skipped as XML.** Reading them needs an
  XML property set reader this does not have. `facearchetype`, `facemodifier` and
  `meshoverlay` never appeared in the binary sample and are presumably there.
- **59 of 2000 catalogue entries index past the end of their key list.**
  Unexplained, and worth a measurement rather than a rationalisation: it may be
  a second sidecar, a different index property, or entries whose shape index
  means nothing. It is the first thing to look at in the catalogue.
- **A reflection cube is painted as a diffuse texture.** A firefighter's visor
  wears `outdoordaytime-envcube` and comes out with the sky across it, because
  the material names it and nothing here knows what an environment cube is for.
  Every material naming one draws wrong in the same way, and it is visible.
- **A `SimSkin` material is painted as one flat texture.** A garment's texture
  covers the garment and leaves the skin around it black — the legs between a
  pair of shorts and a pair of socks, and the hands past a sleeve. The material
  type says what is missing: `SimSkin` composites a garment over the Sim's own
  skin tone, and this draws only the top layer. Every dressed Sim shows it.
  **It is the next thing to do here.**
- **Only an adult male can be dressed.** The four names the assembly starts from
  are his, so the wardrobe's rule is his too. Every other age and gender is on
  the disc and refused, counted, by name of rule.
- **No adult male face at the body's tone turned up in the sample**, so the face
  is worn at whatever tone did and repainted to the body's. It looks right for
  the wrong reason, which is the kind of thing this project has decided twice
  that it does not want.
- **Inverse kinematics are counted and skipped.** Every run says how many; the
  animation currently played carries two.
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

**A number tested only where two meanings coincide has not been tested.**
`renderSetPartTexture` takes a primitive index and was handed a part index. Both
were 0, 1, 2 for as long as every part had one primitive, which was every run
until a Sim wore its first garment — and then the textures landed one range
early and the face came out green. The header said "primitives array" the whole
time. This is the third shape of the same mistake in this document, after the
component index that meant something only inside one container and the backend
call that shared a name and not a behaviour.

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
