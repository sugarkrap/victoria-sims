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

# The navigable part of an installer, and nothing else.
#
# The disc this was built for turned out to be a repack with the whole game —
# 2.7 gibibytes of it — sealed inside one of these, so the engine has to be able
# to find its way around one. What that takes is a Delphi MZP stub, an offset
# table at 0x30 with a checksum the reader uses to work out the field layout,
# and the version string the table's first offset points at.
#
# No payload: nothing here is compressed, and a fixture that pretended to be
# would be testing a decompressor that does not exist yet. What this does test
# is everything up to that point, which is the part that says whether the
# archive can be navigated at all.
python3 - "$buildRoot/TSData.exe" <<'PYTHON'
import binascii, struct, sys

TABLE_AT = 0x30
HEADER_AT = 0x100
DATA_AT = 0x180
TOTAL = 0x200

image = bytearray(TOTAL)
# Delphi's stub, which is what separates an installer from every other program
# on a disc: Microsoft's linker writes MZ, Borland's writes MZP.
image[0:4] = b"MZP\x00"

# Six fields, which is the oldest layout: how much of the file the installer
# accounts for, then four about the program it carries, then the two offsets
# every layout ends with. The reader is not told there are six — it tries each
# length until the checksum agrees, so this fixture is a real test of that.
words = [TOTAL, 0, 0, 0, HEADER_AT, DATA_AT]
table = b"rDlPtS02\x87eVx" + b"".join(struct.pack("<I", word) for word in words)
table += struct.pack("<I", binascii.crc32(table) & 0xFFFFFFFF)
image[TABLE_AT:TABLE_AT + len(table)] = table

version = b"Inno Setup Setup Data (5.5.0) (u)"
image[HEADER_AT:HEADER_AT + len(version)] = version

open(sys.argv[1], "wb").write(image)
PYTHON

mkdir -p "$(dirname "$outputImage")"
xorriso -as mkisofs -quiet -J -r -V "VICTORIA_TEST" -o "$outputImage" "$buildRoot"

echo "wrote $outputImage"
echo "remember to update testAssets/manifest.sha256"
