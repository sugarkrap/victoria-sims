const HEADER_SIZE = 9;
const SIGNATURE_OFFSET = 4;
const DECOMPRESSED_SIZE_OFFSET = 6;

export function looksLikeRefPack(bytes: Buffer): boolean {
    return bytes.length >= HEADER_SIZE && bytes[SIGNATURE_OFFSET] === 0x10 && bytes[SIGNATURE_OFFSET + 1] === 0xfb;
}

export function getDecompressedSize(bytes: Buffer): number {
    if (!looksLikeRefPack(bytes)) {
        return 0;
    }
    return (
        (bytes[DECOMPRESSED_SIZE_OFFSET]! << 16) |
        (bytes[DECOMPRESSED_SIZE_OFFSET + 1]! << 8) |
        bytes[DECOMPRESSED_SIZE_OFFSET + 2]!
    );
}

export function decompress(source: Buffer): Buffer {
    if (!looksLikeRefPack(source)) {
        throw new Error("not a RefPack stream");
    }
    const expectedSize = getDecompressedSize(source);
    const destination = Buffer.alloc(expectedSize);
    let readPosition = HEADER_SIZE;
    let writePosition = 0;

    while (readPosition < source.length) {
        const control = source[readPosition]!;
        let literalCount: number;
        let copyCount = 0;
        let distance = 0;
        let isLast = false;

        if (control < 0x80) {
            literalCount = control & 0x03;
            copyCount = ((control & 0x1c) >> 2) + 3;
            distance = (((control & 0x60) << 3) | source[readPosition + 1]!) + 1;
            readPosition += 2;
        } else if (control < 0xc0) {
            literalCount = (source[readPosition + 1]! & 0xc0) >> 6;
            copyCount = (control & 0x3f) + 4;
            distance = (((source[readPosition + 1]! & 0x3f) << 8) | source[readPosition + 2]!) + 1;
            readPosition += 3;
        } else if (control < 0xe0) {
            literalCount = control & 0x03;
            copyCount = (((control & 0x0c) << 6) | source[readPosition + 3]!) + 5;
            distance =
                (((control & 0x10) << 12) | (source[readPosition + 1]! << 8) | source[readPosition + 2]!) + 1;
            readPosition += 4;
        } else if (control < 0xfc) {
            literalCount = ((control & 0x1f) << 2) + 4;
            readPosition += 1;
        } else {
            literalCount = control & 0x03;
            readPosition += 1;
            isLast = true;
        }

        source.copy(destination, writePosition, readPosition, readPosition + literalCount);
        readPosition += literalCount;
        writePosition += literalCount;

        if (copyCount === 0) {
            if (isLast) {
                break;
            }
            continue;
        }

        if (distance > writePosition) {
            throw new Error("a back reference points before the start of the output");
        }
        for (let index = 0; index < copyCount; index += 1) {
            destination[writePosition + index] = destination[writePosition + index - distance];
        }
        writePosition += copyCount;
    }

    return destination.subarray(0, writePosition);
}
