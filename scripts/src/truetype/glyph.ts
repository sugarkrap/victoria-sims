import { u8, i8, u16, i16 } from "../binary/bigEndian.ts";

export type GlyphPoint = readonly [x: number, y: number, onCurve: 0 | 1];
export type Contour = readonly GlyphPoint[];

export function contour(points: readonly GlyphPoint[]): Contour {
    return points;
}

// Packs contours into a simple glyph. The two coordinate encodings are both
// exercised because a reader can get one right and the other wrong.
export function simpleGlyph(contours: readonly Contour[], useShortDeltas: boolean): Buffer {
    const flat = contours.flat();
    const xs = flat.map((p) => p[0]);
    const ys = flat.map((p) => p[1]);

    const parts: Buffer[] = [
        i16(contours.length),
        i16(Math.min(...xs)),
        i16(Math.min(...ys)),
        i16(Math.max(...xs)),
        i16(Math.max(...ys)),
    ];
    let end = -1;
    for (const c of contours) {
        end += c.length;
        parts.push(u16(end));
    }
    parts.push(u16(0)); // no hinting program

    const flags: number[] = [];
    const xBytes: Buffer[] = [];
    const yBytes: Buffer[] = [];
    let previousX = 0;
    let previousY = 0;
    for (const [x, y, onCurve] of flat) {
        let flag = onCurve ? 0x01 : 0x00;
        const deltaX = x - previousX;
        const deltaY = y - previousY;
        previousX = x;
        previousY = y;

        if (useShortDeltas && deltaX >= -255 && deltaX <= 255) {
            flag |= 0x02;
            if (deltaX >= 0) {
                flag |= 0x10;
            }
            xBytes.push(u8(Math.abs(deltaX)));
        } else {
            xBytes.push(i16(deltaX));
        }

        if (useShortDeltas && deltaY >= -255 && deltaY <= 255) {
            flag |= 0x04;
            if (deltaY >= 0) {
                flag |= 0x20;
            }
            yBytes.push(u8(Math.abs(deltaY)));
        } else {
            yBytes.push(i16(deltaY));
        }
        flags.push(flag);
    }

    // Run-length the flags where they repeat, which is the encoding a reader
    // is most likely to skip and then read every coordinate at the wrong
    // offset.
    const packed: Buffer[] = [];
    let index = 0;
    while (index < flags.length) {
        let run = 1;
        while (index + run < flags.length && flags[index + run] === flags[index] && run < 255) {
            run += 1;
        }
        if (run > 1) {
            // The count is how many EXTRA times the flag repeats, not the
            // run length.
            packed.push(u8(flags[index]! | 0x08), u8(run - 1));
        } else {
            packed.push(u8(flags[index]!));
        }
        index += run;
    }

    return Buffer.concat([...parts, ...packed, ...xBytes, ...yBytes]);
}

export function compositeGlyph(
    componentGlyph: number,
    offsetX: number,
    offsetY: number,
    scale: number | null,
    bounds: readonly [number, number, number, number],
    argumentsAreWords: boolean,
): Buffer {
    let flags = 0x0002; // the arguments are an offset, not a pair of point numbers
    if (argumentsAreWords) {
        flags |= 0x0001;
    }
    if (scale !== null) {
        flags |= 0x0008;
    }

    const parts: Buffer[] = [
        i16(-1),
        i16(bounds[0]),
        i16(bounds[1]),
        i16(bounds[2]),
        i16(bounds[3]),
        u16(flags),
        u16(componentGlyph),
    ];
    if (argumentsAreWords) {
        parts.push(i16(offsetX), i16(offsetY));
    } else {
        parts.push(i8(offsetX), i8(offsetY));
    }
    if (scale !== null) {
        parts.push(i16(scale));
    }
    return Buffer.concat(parts);
}

// Every glyph starts on an even byte, which the short location format
// requires and the long one does not care about.
export function padded(data: Buffer): Buffer {
    return data.length % 2 === 0 ? data : Buffer.concat([data, Buffer.alloc(1)]);
}
