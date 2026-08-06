#!/usr/bin/env python3
"""Writes testAssets/fonts/fixture.mxf: an authored TrueType font inside the
game's own container format.

We ship no game data, so the font reader cannot be checked against a font off a
disc. This writes one from nothing — six glyphs, chosen so that each is the
smallest thing that would catch a specific way of reading the format wrongly:

  0  .notdef   two contours, the inner wound against the outer, so a rasterizer
                that ignores winding fills the hole and a reader that loses the
                second contour draws a solid box
  1  space     no outline at all, which the format stores as two equal entries
                in the location table rather than as a glyph
  2  A         a square with coordinates as full sixteen-bit deltas
  3  B         a triangle with short deltas, repeated flags and one off-curve
                point, which is the packing the other glyph does not use
  4  C         a composite: glyph 2 translated, arguments as words
  5  D         a composite: glyph 2 at half scale, arguments as bytes

Between them they cover every branch in the outline reader. The rest of the
file — the character map, the metrics, the location table — is ordinary and
correct because the reader has to walk it to reach any of the above.

The container is the shape the game uses: the magic MXFN, four bytes whose
meaning is not known, the mask byte in the clear, and then the whole font with
every byte exclusive-ored against that mask, padded to a round length. The four
unknown bytes are written here as the payload length, which is a guess and is
never read back — the reader finds the font by looking for the TrueType version
under a single-byte mask, which is why it can read this file and the game's
without being told anything about either.

Deterministic: run it twice and get the same bytes. Nothing here reads a clock.
"""

import os
import struct

MASK = 0x9D
UNITS_PER_EM = 1024
PADDED_LENGTH = 4096


def contour(points):
    """One closed contour. Each point is (x, y, onCurve)."""
    return list(points)


def simpleGlyph(contours, useShortDeltas):
    """Packs contours into a simple glyph. The two coordinate encodings are both
    exercised because a reader can get one right and the other wrong."""
    flat = [point for c in contours for point in c]
    xs = [p[0] for p in flat]
    ys = [p[1] for p in flat]

    out = struct.pack(">hhhhh", len(contours), min(xs), min(ys), max(xs), max(ys))
    end = -1
    for c in contours:
        end += len(c)
        out += struct.pack(">H", end)
    out += struct.pack(">H", 0)  # no hinting program

    flags = bytearray()
    xBytes = bytearray()
    yBytes = bytearray()
    previousX = 0
    previousY = 0
    for (x, y, onCurve) in flat:
        flag = 0x01 if onCurve else 0x00
        deltaX = x - previousX
        deltaY = y - previousY
        previousX, previousY = x, y

        if useShortDeltas and -255 <= deltaX <= 255:
            flag |= 0x02
            if deltaX >= 0:
                flag |= 0x10
            xBytes += struct.pack(">B", abs(deltaX))
        else:
            xBytes += struct.pack(">h", deltaX)

        if useShortDeltas and -255 <= deltaY <= 255:
            flag |= 0x04
            if deltaY >= 0:
                flag |= 0x20
            yBytes += struct.pack(">B", abs(deltaY))
        else:
            yBytes += struct.pack(">h", deltaY)
        flags.append(flag)

    # Run-length the flags where they repeat, which is the encoding a reader is
    # most likely to skip and then read every coordinate at the wrong offset.
    packed = bytearray()
    index = 0
    while index < len(flags):
        run = 1
        while index + run < len(flags) and flags[index + run] == flags[index] and run < 255:
            run += 1
        if run > 1:
            # The count is how many EXTRA times the flag repeats, not the run
            # length. Off by one here and every coordinate after it is read at
            # the wrong offset.
            packed += struct.pack(">BB", flags[index] | 0x08, run - 1)
        else:
            packed.append(flags[index])
        index += run

    return out + bytes(packed) + bytes(xBytes) + bytes(yBytes)


def compositeGlyph(componentGlyph, offsetX, offsetY, scale, bounds, argumentsAreWords):
    flags = 0x0002  # the arguments are an offset and not a pair of point numbers
    if argumentsAreWords:
        flags |= 0x0001
    if scale is not None:
        flags |= 0x0008

    out = struct.pack(">hhhhh", -1, bounds[0], bounds[1], bounds[2], bounds[3])
    out += struct.pack(">HH", flags, componentGlyph)
    if argumentsAreWords:
        out += struct.pack(">hh", offsetX, offsetY)
    else:
        out += struct.pack(">bb", offsetX, offsetY)
    if scale is not None:
        out += struct.pack(">h", scale)
    return out


def padded(data):
    """Every glyph starts on an even byte, which the short location format
    requires and the long one does not care about."""
    return data + (b"\0" * (len(data) % 2))


def buildGlyphs():
    ring = simpleGlyph(
        [
            contour([(0, 0, 1), (600, 0, 1), (600, 600, 1), (0, 600, 1)]),
            # Wound the other way round, so non-zero winding leaves a hole.
            contour([(100, 100, 1), (100, 500, 1), (500, 500, 1), (500, 100, 1)]),
        ],
        useShortDeltas=False,
    )
    space = b""
    square = simpleGlyph(
        [contour([(100, 100, 1), (500, 100, 1), (500, 500, 1), (100, 500, 1)])],
        useShortDeltas=False,
    )
    triangle = simpleGlyph(
        [contour([(60, 60, 1), (260, 60, 1), (300, 300, 0), (60, 460, 1)])],
        useShortDeltas=True,
    )
    translated = compositeGlyph(2, 200, 0, None, (300, 100, 700, 500), argumentsAreWords=True)
    halved = compositeGlyph(2, 0, 0, 0x2000, (50, 50, 250, 250), argumentsAreWords=False)
    return [padded(g) for g in (ring, space, square, triangle, translated, halved)]


def buildCharacterMap(mapping):
    """Format 4, the one nearly every font uses. Segments must be sorted and the
    last one must end at 0xFFFF, which is the rule readers most often meet only
    by accident."""
    segments = []
    for codePoint in sorted(mapping):
        if segments and codePoint == segments[-1][1] + 1 and \
                mapping[codePoint] == mapping[segments[-1][1]] + 1:
            segments[-1] = (segments[-1][0], codePoint)
        else:
            segments.append((codePoint, codePoint))
    segments.append((0xFFFF, 0xFFFF))

    count = len(segments)
    searchRange = 2
    while searchRange * 2 <= count * 2:
        searchRange *= 2
    entrySelector = 0
    while (1 << (entrySelector + 1)) <= searchRange // 2:
        entrySelector += 1

    body = struct.pack(">HHHHHH", 4, 0, 0, count * 2, searchRange, entrySelector)
    body += struct.pack(">H", count * 2 - searchRange)
    body += b"".join(struct.pack(">H", s[1]) for s in segments)
    body += struct.pack(">H", 0)
    body += b"".join(struct.pack(">H", s[0]) for s in segments)
    for start, end in segments:
        glyph = mapping.get(start, 0)
        delta = (glyph - start) & 0xFFFF if start != 0xFFFF else 1
        body += struct.pack(">H", delta)
    body += b"".join(struct.pack(">H", 0) for _ in segments)
    body = body[:2] + struct.pack(">H", len(body)) + body[4:]

    table = struct.pack(">HH", 0, 1)              # version, one subtable
    table += struct.pack(">HHI", 3, 1, 12)        # Windows, Unicode, at offset 12
    return table + body


def buildFont():
    glyphs = buildGlyphs()
    glyphData = b"".join(glyphs)

    locations = [0]
    for g in glyphs:
        locations.append(locations[-1] + len(g))

    advances = [700, 400, 600, 400, 800, 300]
    bearings = [0, 0, 100, 60, 300, 50]

    head = struct.pack(
        ">IIIIHH", 0x00010000, 0x00010000, 0, 0x5F0F3CF5, 0x0000, UNITS_PER_EM
    )
    head += struct.pack(">qq", 0, 0)              # created, modified
    head += struct.pack(">hhhh", 0, 0, 700, 600)  # bounding box
    head += struct.pack(">HHhhh", 0, 8, 2, 1, 0)  # macStyle .. glyphDataFormat, long locations

    maxp = struct.pack(">IH", 0x00010000, len(glyphs))
    maxp += struct.pack(">HH", 8, 2)              # maxPoints, maxContours
    maxp += struct.pack(">HH", 8, 2)              # the same for composites
    maxp += struct.pack(">HHHHHHHHH", 2, 0, 0, 0, 0, 0, 0, 2, 2)

    hhea = struct.pack(">Ihhh", 0x00010000, 800, -200, 90)
    hhea += struct.pack(">Hhhh", max(advances), 0, 0, 700)
    hhea += struct.pack(">hhh", 1, 0, 0)
    hhea += struct.pack(">hhhh", 0, 0, 0, 0)
    hhea += struct.pack(">hH", 0, len(advances))

    hmtx = b"".join(struct.pack(">Hh", a, b) for a, b in zip(advances, bearings))
    loca = b"".join(struct.pack(">I", o) for o in locations)
    cmap = buildCharacterMap({0x20: 1, 0x41: 2, 0x42: 3, 0x43: 4, 0x44: 5})

    tables = {
        b"cmap": cmap,
        b"glyf": glyphData,
        b"head": head,
        b"hhea": hhea,
        b"hmtx": hmtx,
        b"loca": loca,
        b"maxp": maxp,
    }

    names = sorted(tables)
    count = len(names)
    searchRange = 16
    entrySelector = 0
    while searchRange * 2 <= count * 16:
        searchRange *= 2
        entrySelector += 1

    directory = struct.pack(">IHHHH", 0x00010000, count, searchRange, entrySelector,
                            count * 16 - searchRange)
    offset = 12 + count * 16
    records = b""
    body = b""
    for name in names:
        data = tables[name] + (b"\0" * ((4 - len(tables[name]) % 4) % 4))
        records += name + struct.pack(">III", 0, offset, len(tables[name]))
        body += data
        offset += len(data)
    return directory + records + body


def buildContainer(font):
    """MXFN, four bytes whose meaning is unknown, the mask in the clear, then the
    font under the mask, padded with what looks like a repeated byte and is
    nought wearing the same disguise."""
    header = b"MXFN" + struct.pack("<I", len(font)) + bytes([MASK])
    masked = bytes(b ^ MASK for b in font)
    room = PADDED_LENGTH - len(header) - len(masked)
    if room < 0:
        raise SystemExit("the font outgrew the padded length")
    return header + masked + (bytes([MASK]) * room)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    directory = os.path.join(root, "testAssets", "fonts")
    os.makedirs(directory, exist_ok=True)
    path = os.path.join(directory, "fixture.mxf")

    data = buildContainer(buildFont())
    with open(path, "wb") as handle:
        handle.write(data)
    print("wrote %s, %d bytes" % (path, len(data)))


if __name__ == "__main__":
    main()
