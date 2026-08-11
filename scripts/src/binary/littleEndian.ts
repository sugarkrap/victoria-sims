export function u8(value: number): Buffer {
    return Buffer.from([value & 0xff]);
}

export function u16(value: number): Buffer {
    const b = Buffer.alloc(2);
    b.writeUInt16LE(value & 0xffff);
    return b;
}

export function u32(value: number): Buffer {
    const b = Buffer.alloc(4);
    b.writeUInt32LE(value >>> 0);
    return b;
}

export function f32(value: number): Buffer {
    const b = Buffer.alloc(4);
    b.writeFloatLE(value);
    return b;
}

export function ascii(text: string): Buffer {
    return Buffer.from(text, "ascii");
}

// A scenegraph string: one to five length bytes with a continuation bit.
//
// NOT the same as a property set's string, which is a flat four-byte length.
// Either rule applied to the other's stream yields a plausible length, which
// is exactly why both are spelled out here rather than shared.
export function sgString(text: string): Buffer {
    const data = ascii(text);
    let length = data.length;
    const out: Buffer[] = [];
    for (;;) {
        const byte = length & 0x7f;
        length >>= 7;
        if (length) {
            out.push(u8(byte | 0x80));
        } else {
            out.push(u8(byte));
            break;
        }
    }
    return Buffer.concat([...out, data]);
}

// A property set string: a flat four-byte length and then the bytes.
export function cpfString(text: string): Buffer {
    const data = ascii(text);
    return Buffer.concat([u32(data.length), data]);
}

// A geometry container string: one length byte. Short names only.
export function shortString(text: string): Buffer {
    const data = ascii(text);
    if (data.length >= 256) {
        throw new Error(`name too long for a short string: ${text}`);
    }
    return Buffer.concat([u8(data.length), data]);
}
