#!/bin/sh
# Regenerates testAssets/discs/testDisc.iso.
#
# The disc carries the scenegraph fixtures that already live in testAssets/,
# laid out the way a retail disc lays them out. Using the real fixtures rather
# than empty files is the point: the test that walks this image goes on to open
# what it finds with the package reader, so the two halves are exercised
# together and against structures a retail disc actually has.
#
# Requires xorriso. The image is committed and pinned by hash, so contributors
# only need this when the fixture itself has to change — after which
# testAssets/manifest.sha256 has to be updated too.

set -eu

repositoryRoot="$(cd "$(dirname "$0")/.." && pwd)"
outputImage="$repositoryRoot/testAssets/discs/testDisc.iso"
scenegraph="$repositoryRoot/testAssets/scenegraph"
buildRoot="$(mktemp -d)"
trap 'rm -rf "$buildRoot"' EXIT

mkdir -p "$buildRoot/TSData/Res/Sims3D" "$buildRoot/TSData/Res/Materials" "$buildRoot/Support"

cp "$scenegraph/teapot_model.package" "$buildRoot/TSData/Res/Sims3D/teapot_model.package"
cp "$scenegraph/animation.package" "$buildRoot/TSData/Res/Sims3D/animation.package"
cp "$scenegraph/textures.package" "$buildRoot/TSData/Res/Materials/textures.package"
cp "$scenegraph/material_definition.package" "$buildRoot/TSData/Res/Materials/material_definition.package"

# A file named like a package that is not one, so detection is forced to read
# the magic rather than trust the extension.
printf 'this is not a package at all, despite the name\n' \
    > "$buildRoot/TSData/Res/NotReally.package"

# An installer archive, so the "sealed inside an installer" path has something
# to find. Retail discs put the game inside one of these.
printf 'placeholder cabinet payload\n' > "$buildRoot/Support/data1.cab"
printf '[autorun]\n' > "$buildRoot/Autorun.inf"

# The navigable part of a repack's payload.
#
# The disc this was built for keeps its whole game — 2.7 gibibytes of it — in a
# file that is a program with an archive appended past the end of it, so the
# engine has to be able to find its way from one to the other. Nothing here is
# compressed: a fixture that pretended to be would be testing an unpacker that
# does not exist. What it does test is everything up to that point.
python3 - "$buildRoot/TSData.exe" <<'PYTHON'
import struct, sys

# A real program at the front, and an archive appended past the end of it.
#
# That is the shape the disc's own TSData.exe has: three sections ending at
# 0xCE00, then 2.7 gibibytes beginning "Rar!". The front holds nothing that
# identifies an archive, which is why the engine reads the section table to find
# out where the front stops.
#
# Two entries, one stored and one packed, because the difference between them
# decides everything downstream. A stored entry is a range of the file and can
# go straight to the package reader; a packed one cannot.
HEADER_AT = 0x80          # where the DOS stub points
SECTION_TABLE_AT = HEADER_AT + 4 + 20 + 224
PROGRAM_ENDS_AT = 0x200   # the one section's bytes end here, and the archive begins
TOTAL = 0x2B3

image = bytearray(TOTAL)
# Delphi's stub, which is what separates an installer from every other program
# on a disc: Microsoft's linker writes MZ, Borland's writes MZP.
image[0:4] = b"MZP\x00"
image[0x3C:0x40] = struct.pack("<I", HEADER_AT)
image[HEADER_AT:HEADER_AT + 4] = b"PE\x00\x00"
# Two bytes of machine, then the section count; twelve bytes of symbol table
# fields, then the optional header's size.
image[HEADER_AT + 4 + 2:HEADER_AT + 4 + 4] = struct.pack("<H", 1)
image[HEADER_AT + 4 + 16:HEADER_AT + 4 + 18] = struct.pack("<H", 224)
# That section covers the program and nothing after it.
image[SECTION_TABLE_AT + 16:SECTION_TABLE_AT + 20] = struct.pack("<I", PROGRAM_ENDS_AT)
image[SECTION_TABLE_AT + 20:SECTION_TABLE_AT + 24] = struct.pack("<I", 0)

# Rar!, fourth generation. The fifth writes an eighth byte and is a different
# format; the reader refuses that one by name rather than misreading it.
at = PROGRAM_ENDS_AT
image[at:at + 7] = b"Rar!\x1a\x07\x00"
at += 7

# The archive header block: a length and nothing this reader needs.
image[at + 2] = 0x73
image[at + 5:at + 7] = struct.pack("<H", 13)
at += 13

def fileHeader(at, name, packed, unpacked, method):
    """One file header block, followed by its data. Returns where the next
    block starts."""
    name = name.encode()
    headerSize = 32 + len(name)
    image[at + 2] = 0x74
    image[at + 3:at + 5] = struct.pack("<H", 0x8000)      # carries data
    image[at + 5:at + 7] = struct.pack("<H", headerSize)
    image[at + 7:at + 11] = struct.pack("<I", packed)
    image[at + 11:at + 15] = struct.pack("<I", unpacked)
    image[at + 24] = 20                                    # version needed
    image[at + 25] = method                                # 0x30 stored
    image[at + 26:at + 28] = struct.pack("<H", len(name))
    image[at + 32:at + 32 + len(name)] = name
    return at + headerSize + packed, at + headerSize

# Stored, and its data really is a package: the whole reason to care which of
# the two an entry is.
at, dataAt = fileHeader(at, "TSData/Res/Sims3D/Sims01.package", 16, 16, 0x30)
image[dataAt:dataAt + 4] = b"DBPF"

# Packed, whose sizes differ — which is the other thing that says so.
at, dataAt = fileHeader(at, "TSData/Res/Sims3D/Sims02.package", 8, 32, 0x35)

# The end block a real archive closes with. Without it the walk runs off the
# last entry into whatever follows and reports it as damage, which is what the
# first version of this fixture did.
image[at + 2] = 0x7B
image[at + 5:at + 7] = struct.pack("<H", 7)

open(sys.argv[1], "wb").write(image)
PYTHON

mkdir -p "$(dirname "$outputImage")"
xorriso -as mkisofs -quiet -J -r -V "VICTORIA_TEST" -o "$outputImage" "$buildRoot"

echo "wrote $outputImage"
echo "remember to update testAssets/manifest.sha256"
