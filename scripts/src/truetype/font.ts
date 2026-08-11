import { u16, i16, u32, ascii } from "../binary/bigEndian.ts";
import { contour, simpleGlyph, compositeGlyph, padded } from "./glyph.ts";
import { buildCharacterMap } from "./characterMap.ts";

const UNITS_PER_EM = 1024;

// Six glyphs, chosen so that each is the smallest thing that would catch a
// specific way of reading the format wrongly:
//
//   0  .notdef   two contours, the inner wound against the outer, so a
//                 rasterizer that ignores winding fills the hole and a
//                 reader that loses the second contour draws a solid box
//   1  space     no outline at all, which the format stores as two equal
//                 entries in the location table rather than as a glyph
//   2  A         a square with coordinates as full sixteen-bit deltas
//   3  B         a triangle with short deltas, repeated flags and one
//                 off-curve point, which is the packing the other glyph
//                 does not use
//   4  C         a composite: glyph 2 translated, arguments as words
//   5  D         a composite: glyph 2 at half scale, arguments as bytes
function buildGlyphs(): Buffer[] {
    const ring = simpleGlyph(
        [
            contour([
                [0, 0, 1],
                [600, 0, 1],
                [600, 600, 1],
                [0, 600, 1],
            ]),
            // Wound the other way round, so non-zero winding leaves a hole.
            contour([
                [100, 100, 1],
                [100, 500, 1],
                [500, 500, 1],
                [500, 100, 1],
            ]),
        ],
        false,
    );
    const space = Buffer.alloc(0);
    const square = simpleGlyph(
        [
            contour([
                [100, 100, 1],
                [500, 100, 1],
                [500, 500, 1],
                [100, 500, 1],
            ]),
        ],
        false,
    );
    const triangle = simpleGlyph(
        [
            contour([
                [60, 60, 1],
                [260, 60, 1],
                [300, 300, 0],
                [60, 460, 1],
            ]),
        ],
        true,
    );
    const translated = compositeGlyph(2, 200, 0, null, [300, 100, 700, 500], true);
    const halved = compositeGlyph(2, 0, 0, 0x2000, [50, 50, 250, 250], false);
    return [ring, space, square, triangle, translated, halved].map(padded);
}

export function buildFont(): Buffer {
    const glyphs = buildGlyphs();
    const glyphData = Buffer.concat(glyphs);

    const locations = [0];
    for (const g of glyphs) {
        locations.push(locations[locations.length - 1]! + g.length);
    }

    const advances = [700, 400, 600, 400, 800, 300];
    const bearings = [0, 0, 100, 60, 300, 50];

    let head = Buffer.concat([
        u32(0x00010000),
        u32(0x00010000),
        u32(0),
        u32(0x5f0f3cf5),
        u16(0x0000),
        u16(UNITS_PER_EM),
    ]);
    head = Buffer.concat([head, Buffer.alloc(16)]); // created, modified
    head = Buffer.concat([head, i16(0), i16(0), i16(700), i16(600)]); // bounding box
    head = Buffer.concat([head, u16(0), u16(8), i16(2), i16(1), i16(0)]); // macStyle .. glyphDataFormat, long locations

    let maxp = Buffer.concat([u32(0x00010000), u16(glyphs.length)]);
    maxp = Buffer.concat([maxp, u16(8), u16(2)]); // maxPoints, maxContours
    maxp = Buffer.concat([maxp, u16(8), u16(2)]); // the same for composites
    maxp = Buffer.concat([maxp, u16(2), u16(0), u16(0), u16(0), u16(0), u16(0), u16(0), u16(2), u16(2)]);

    let hhea = Buffer.concat([u32(0x00010000), i16(800), i16(-200), i16(90)]);
    hhea = Buffer.concat([hhea, u16(Math.max(...advances)), i16(0), i16(0), i16(700)]);
    hhea = Buffer.concat([hhea, i16(1), i16(0), i16(0)]);
    hhea = Buffer.concat([hhea, i16(0), i16(0), i16(0), i16(0)]);
    hhea = Buffer.concat([hhea, i16(0), u16(advances.length)]);

    const hmtx = Buffer.concat(advances.map((a, i) => Buffer.concat([u16(a), i16(bearings[i]!)])));
    const loca = Buffer.concat(locations.map((o) => u32(o)));
    const cmap = buildCharacterMap(
        new Map([
            [0x20, 1],
            [0x41, 2],
            [0x42, 3],
            [0x43, 4],
            [0x44, 5],
        ]),
    );

    const tables = new Map<string, Buffer>([
        ["cmap", cmap],
        ["glyf", glyphData],
        ["head", head],
        ["hhea", hhea],
        ["hmtx", hmtx],
        ["loca", loca],
        ["maxp", maxp],
    ]);

    const names = [...tables.keys()].sort();
    const count = names.length;
    let searchRange = 16;
    let entrySelector = 0;
    while (searchRange * 2 <= count * 16) {
        searchRange *= 2;
        entrySelector += 1;
    }

    const directory = Buffer.concat([
        u32(0x00010000),
        u16(count),
        u16(searchRange),
        u16(entrySelector),
        u16(count * 16 - searchRange),
    ]);
    let offset = 12 + count * 16;
    let records = Buffer.alloc(0);
    let body = Buffer.alloc(0);
    for (const name of names) {
        const table = tables.get(name)!;
        const data = Buffer.concat([table, Buffer.alloc((4 - (table.length % 4)) % 4)]);
        records = Buffer.concat([records, ascii(name), u32(0), u32(offset), u32(table.length)]);
        body = Buffer.concat([body, data]);
        offset += data.length;
    }
    return Buffer.concat([directory, records, body]);
}
