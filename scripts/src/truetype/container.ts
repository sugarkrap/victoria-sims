import { u8, ascii } from "../binary/bigEndian.ts";

const MASK = 0x9d;
const PADDED_LENGTH = 4096;

// MXFN, four bytes whose meaning is unknown, the mask in the clear, then the
// font under the mask, padded with what looks like a repeated byte and is
// nought wearing the same disguise.
export function buildContainer(font: Buffer): Buffer {
    const lengthField = Buffer.alloc(4);
    lengthField.writeUInt32LE(font.length);
    const header = Buffer.concat([ascii("MXFN"), lengthField, u8(MASK)]);
    const masked = Buffer.from(font.map((b) => b ^ MASK));
    const room = PADDED_LENGTH - header.length - masked.length;
    if (room < 0) {
        throw new Error("the font outgrew the padded length");
    }
    return Buffer.concat([header, masked, Buffer.alloc(room, MASK)]);
}
