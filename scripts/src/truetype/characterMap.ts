import { u16, u32 } from "../binary/bigEndian.ts";

// Format 4, the one nearly every font uses. Segments must be sorted and the
// last one must end at 0xFFFF, which is the rule readers most often meet only
// by accident.
export function buildCharacterMap(mapping: ReadonlyMap<number, number>): Buffer {
    const codePoints = [...mapping.keys()].sort((a, b) => a - b);
    const segments: Array<[number, number]> = [];
    for (const codePoint of codePoints) {
        const last = segments[segments.length - 1];
        if (last && codePoint === last[1] + 1 && mapping.get(codePoint) === mapping.get(last[1])! + 1) {
            segments[segments.length - 1] = [last[0], codePoint];
        } else {
            segments.push([codePoint, codePoint]);
        }
    }
    segments.push([0xffff, 0xffff]);

    const count = segments.length;
    let searchRange = 2;
    while (searchRange * 2 <= count * 2) {
        searchRange *= 2;
    }
    let entrySelector = 0;
    while (1 << (entrySelector + 1) <= Math.floor(searchRange / 2)) {
        entrySelector += 1;
    }

    const parts: Buffer[] = [u16(4), u16(0), u16(0), u16(count * 2), u16(searchRange), u16(entrySelector)];
    parts.push(u16(count * 2 - searchRange));
    for (const [, end] of segments) {
        parts.push(u16(end));
    }
    parts.push(u16(0));
    for (const [start] of segments) {
        parts.push(u16(start));
    }
    for (const [start, end] of segments) {
        const glyph = mapping.get(start) ?? 0;
        const delta = start !== 0xffff ? (glyph - start) & 0xffff : 1;
        parts.push(u16(delta));
    }
    for (let i = 0; i < segments.length; i += 1) {
        parts.push(u16(0));
    }
    let body = Buffer.concat(parts);
    body = Buffer.concat([body.subarray(0, 2), u16(body.length), body.subarray(4)]);

    const table = Buffer.concat([u16(0), u16(1), u16(3), u16(1), u32(12)]);
    return Buffer.concat([table, body]);
}
