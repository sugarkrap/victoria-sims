export function u8(value: number): Buffer {
    return Buffer.from([value & 0xff]);
}

export function i8(value: number): Buffer {
    const b = Buffer.alloc(1);
    b.writeInt8(value | 0);
    return b;
}

export function u16(value: number): Buffer {
    const b = Buffer.alloc(2);
    b.writeUInt16BE(value & 0xffff);
    return b;
}

export function i16(value: number): Buffer {
    const b = Buffer.alloc(2);
    b.writeInt16BE(value | 0);
    return b;
}

export function u32(value: number): Buffer {
    const b = Buffer.alloc(4);
    b.writeUInt32BE(value >>> 0);
    return b;
}

export function ascii(text: string): Buffer {
    return Buffer.from(text, "ascii");
}
