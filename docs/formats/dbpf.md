# DBPF package format

The container every Sims 2 resource lives in. What follows is the header and
index only — enough to enumerate a package and locate a resource. Individual
resource formats are separate documents.

Everything marked **verified** was read back out of the fixtures in
`testAssets/scenegraph/` by an actual parse. Everything marked *unverified* is
from the wider reverse-engineering record and has not been exercised here yet;
treat it as a lead, not a fact.

## Header

96 bytes, little-endian throughout. **Verified.**

| Offset | Size | Field |
| --- | --- | --- |
| 0x00 | 4 | Magic, `DBPF` |
| 0x04 | 4 | Major version |
| 0x08 | 4 | Minor version |
| 0x0C | 12 | Unused |
| 0x18 | 4 | Date created *(unverified)* |
| 0x1C | 4 | Date modified *(unverified)* |
| 0x20 | 4 | Index major version |
| 0x24 | 4 | Index entry count |
| 0x28 | 4 | Index offset |
| 0x2C | 4 | Index size in bytes |
| 0x30 | 4 | Hole entry count *(unverified)* |
| 0x34 | 4 | Hole offset *(unverified)* |
| 0x38 | 4 | Hole size *(unverified)* |
| 0x3C | 4 | Index minor version |
| 0x40 | 32 | Unused |

The fixtures are DBPF 1.1, except `Effects.package` upstream which is 1.0. Both
occur in practice, so a reader must handle the two index layouts below rather
than assuming.

## Index

`indexEntryCount` entries at `indexOffset`. Entry size follows the index minor
version — and the safest way to determine it is `indexSize / indexEntryCount`,
which is what the parser used here did, because it does not depend on
interpreting the version field correctly.

**Verified**, 24-byte entry (index minor version 2):

| Offset | Size | Field |
| --- | --- | --- |
| 0x00 | 4 | Type ID |
| 0x04 | 4 | Group ID |
| 0x08 | 4 | Instance ID |
| 0x0C | 4 | Instance ID high |
| 0x10 | 4 | Offset of the resource in the file |
| 0x14 | 4 | Size of the resource in bytes |

*Unverified*, 20-byte entry (index minor version 1): the same without the
instance-high field.

A resource is identified by the type, group and instance triple — the "TGI".
The instance-high word is part of that identity where present, so a reader that
ignores it will collide entries that the game considers distinct.

## Type identifiers

**Verified** against the fixtures — each of these was found in the package
named, with the resource count shown:

| Type ID | Name | Meaning | Seen in |
| --- | --- | --- | --- |
| `0xE519C933` | CRES | Resource node; ties a model to the skeleton | `teapot_model.package` ×1 |
| `0xFC6EB1F7` | SHPE | Shape; binds material to geometry | `teapot_model.package` ×1 |
| `0x7BA3838C` | GMND | Geometric node; settings and morph names | `teapot_model.package` ×1 |
| `0xAC4F8687` | GMDC | Geometric data container; the actual buffers | `teapot_model.package` ×1 |
| `0x49596978` | TXMT | Material definition | `material_definition.package` ×1 |
| `0x1C4A276C` | TXTR | Texture image | `textures.package` ×4 |
| `0xED534136` | LIFO | Level info, the large mip of a texture | `textures.package` ×1 |
| `0xFB00791E` | ANIM | Animation resource | `animation.package` ×3 |
| `0xE86B1EEF` | DIR | Compression directory | upstream `Effects.package` ×1 |

The load order the renderer has to walk is
CRES → SHPE → GMND → GMDC, with TXMT and TXTR reached from the SHPE's material
references.

*Unverified*, present in upstream's other fixtures but not yet decoded here:
`0xEA5118B0` (effects), `0xFA1C39F7` (lot objects), `0x0BF999E7` (lot),
`0xAC506764` (3IDR), `0x42484156` (BHAV), `0x53545223` (STR#),
`0xAC598EAC`, `0xEBCF3E27`, `0xABD0DC63`, `0x6D619378`, `0x6F626A74`,
`0x4F626A4D`, `0x584F424A`.

## Compression

*Unverified here.* Entries listed in the DIR resource (`0xE86B1EEF`) are
QFS/RefPack compressed and carry their uncompressed size there. A package
without a DIR resource has no compressed entries. None of the scenegraph
fixtures contain a DIR, which is why the reader can be brought up against them
before compression support exists.

## The reader

`engine/source/packageReader.c` implements the above. Two things about it are
deliberate:

* **It performs no file I/O.** It is handed bytes that already exist. That is
  what lets it be identical on WebAssembly, which has no filesystem, and lets a
  test point it at a buffer with no platform layer underneath.
* **A bad index is rejected outright, not partially believed.** A resource
  claiming to extend past the end of the file makes the whole index
  untrustworthy, so the open fails rather than skipping that entry.

Entry size is derived by dividing the index size by the entry count, as above,
rather than by reading the version field.

`tests/verifyPackageReader.c` checks it against the real fixtures in
`testAssets/`, asserting exact resource counts per type — including the full
CRES/SHPE/GMND/GMDC chain in `teapot_model.package` — plus the rejection paths.

## Reading notes for this project

The package reader allocates nothing. An index has a known entry count before
any entry is read, so the caller sizes an arena region from
`indexEntryCount × sizeof(entry)` and the reader fills it. A package whose
index does not fit the space provided is rejected, not truncated.
