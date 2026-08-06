#!/bin/sh
# Enforces a claim the font path makes about itself: it does no floating point.
#
# glyphRaster.h says so in as many words, and the reason matters. The floor of
# the device ladder is an ARMv5TE with no unit for it, where every float is a
# call into a software library, and rasterizing one glyph is tens of thousands
# of arithmetic operations. Fixed point there is not a stylistic preference —
# it is the difference between a font that appears and one that does not.
#
# A claim like that rots in exactly one way: somebody writes `x / 2.0f` in a
# helper, it compiles everywhere, it is correct everywhere, and it is thirty
# times slower on the one machine nobody has to hand. Grepping the sources for
# `float` would catch that and would also catch the word in a comment, so this
# does the only thing that cannot be argued with: it cross-compiles for the
# floor of the ladder and looks for calls to the soft-float library in the
# objects that come out.
#
# Needs a compiler that can target ARMv5TE. Clang can out of the box; a GNU
# cross-compiler works too. Skipped rather than failed when there is none,
# because a developer without one should still be able to run the checks — the
# build in continuous integration has one and does not skip.

set -eu

# Every module between a font on the disc and pixels on the screen. Not the
# renderers: renderSoftware projects a camera and shades a triangle, which is
# floating point on purpose and is charged for elsewhere.
MODULES="fontReader glyphRaster fontAtlas builtinFont interfaceSurface interfaceMenu"

# The names the ARM EABI gives its soft-float helpers, plus the older libgcc
# spellings. A call to any of them from a module above is the bug.
FLOAT_SYMBOLS='__aeabi_(f|d)(add|sub|mul|div|cmp|rsub|neg)|__aeabi_(i2f|i2d|ui2f|ui2d|f2iz|d2iz|f2d|d2f)|__(add|sub|mul|div|neg)[sd]f3|__float[sd]i[sd]f|__fix[sd]fsi|__(eq|ne|lt|le|gt|ge)[sd]f2'

ARM_COMPILER=${ARM_COMPILER:-}
if [ -z "$ARM_COMPILER" ]; then
    if command -v arm-linux-gnueabi-gcc >/dev/null 2>&1; then
        ARM_COMPILER="arm-linux-gnueabi-gcc"
    elif command -v clang >/dev/null 2>&1; then
        ARM_COMPILER="clang --target=armv5te-linux-gnueabi"
    else
        echo "no ARM compiler, skipping the no-floating-point check"
        exit 0
    fi
fi

SYMBOL_READER=${SYMBOL_READER:-}
if [ -z "$SYMBOL_READER" ]; then
    if command -v llvm-nm >/dev/null 2>&1; then
        SYMBOL_READER="llvm-nm"
    elif command -v nm >/dev/null 2>&1; then
        SYMBOL_READER="nm"
    else
        echo "no symbol reader, skipping the no-floating-point check"
        exit 0
    fi
fi

scratchDirectory=$(mktemp -d)
trap 'rm -rf "$scratchDirectory"' EXIT

echo "compiling the font path for ARMv5TE with no floating point unit..."
failureCount=0
for module in $MODULES; do
    objectName="$scratchDirectory/$module.o"
    $ARM_COMPILER -std=c99 -pedantic -Wall -Werror \
        -march=armv5te -mfloat-abi=soft -ffreestanding \
        -DVICTORIA_FREESTANDING_BUILTINS=1 \
        -Iengine/include -I. -c "engine/source/$module.c" -o "$objectName"

    found=$($SYMBOL_READER --undefined-only "$objectName" 2>/dev/null \
        | grep -oE "$FLOAT_SYMBOLS" || true)
    if [ -n "$found" ]; then
        echo "error: $module.c calls the soft-float library:" >&2
        echo "$found" | sort -u | sed 's/^/    /' >&2
        failureCount=$((failureCount + 1))
    else
        echo "  $module: no floating point at all"
    fi
done

if [ "$failureCount" -ne 0 ]; then
    echo "no-floating-point check FAILED" >&2
    exit 1
fi

echo "no-floating-point check passed"
