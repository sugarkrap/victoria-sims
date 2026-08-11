const CRC24_POLYNOMIAL = 0x864cfb;
const CRC24_INITIAL = 0xb704ce;
const CRC32_POLYNOMIAL = 0x04c11db7;
const CRC32_INITIAL = 0xffffffff;

function toLowerAsciiBytes(name: string): Buffer {
    return Buffer.from(name.toLowerCase(), "ascii");
}

export function crc24(name: string): number {
    let remainder = CRC24_INITIAL;
    for (const byte of toLowerAsciiBytes(name)) {
        remainder ^= byte << 16;
        for (let bit = 0; bit < 8; bit += 1) {
            remainder = (remainder << 1) >>> 0;
            if ((remainder & 0x01000000) !== 0) {
                remainder ^= CRC24_POLYNOMIAL;
            }
        }
    }
    return remainder & 0x00ffffff;
}

export function crc32Mpeg2(name: string): number {
    let remainder = CRC32_INITIAL;
    for (const byte of toLowerAsciiBytes(name)) {
        remainder = (remainder ^ (byte << 24)) >>> 0;
        for (let bit = 0; bit < 8; bit += 1) {
            if ((remainder & 0x80000000) !== 0) {
                remainder = ((remainder << 1) ^ CRC32_POLYNOMIAL) >>> 0;
            } else {
                remainder = (remainder << 1) >>> 0;
            }
        }
    }
    return remainder >>> 0;
}

export function instanceOf(name: string): number {
    return (crc24(name) | 0xff000000) >>> 0;
}

export function instanceHighOf(name: string): number {
    return crc32Mpeg2(name);
}

export function groupOf(name: string): number {
    return (crc24(name) | 0x7f000000) >>> 0;
}
