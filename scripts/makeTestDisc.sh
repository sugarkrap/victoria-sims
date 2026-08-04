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

# The head of an installer, and nothing else: a Delphi MZP stub with the loader
# mark at 0x30 where Inno Setup writes it. The disc this was built for turned
# out to be a repack with the whole game sealed in one of these, and identifying
# it correctly is the difference between "this disc has no art on it" and "the
# art is behind an archive". Sixty-four bytes, because that is all the probe
# reads — a real installer's payload is not the part being tested.
printf 'MZP\000\002\000\000\000\004\000\017\000\377\377\000\000' \
    > "$buildRoot/TSData.exe"
printf '\270\000\000\000\000\000\000\000@\000\032\000\000\000\000\000' \
    >> "$buildRoot/TSData.exe"
printf '\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000' \
    >> "$buildRoot/TSData.exe"
printf 'rDlPtS02\207eVx\000\000\000\000' >> "$buildRoot/TSData.exe"

mkdir -p "$(dirname "$outputImage")"
xorriso -as mkisofs -quiet -J -r -V "VICTORIA_TEST" -o "$outputImage" "$buildRoot"

echo "wrote $outputImage"
echo "remember to update testAssets/manifest.sha256"
