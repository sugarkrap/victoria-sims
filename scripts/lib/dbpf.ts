import { readAt } from "./iso9660.ts";

const HEADER_SIZE = 96;
const ENTRY_SIZE_WITHOUT_INSTANCE_HIGH = 20;
const ENTRY_SIZE_WITH_INSTANCE_HIGH = 24;

export interface DbpfHeader {
    majorVersion: number;
    minorVersion: number;
    entryCount: number;
    indexOffset: number;
    indexSize: number;
    entrySize: 20 | 24 | null;
}

export function readHeader(fd: number, byteOffset: number): DbpfHeader | null {
    const header = readAt(fd, byteOffset, HEADER_SIZE);
    if (header.length < HEADER_SIZE || header.toString("ascii", 0, 4) !== "DBPF") {
        return null;
    }
    const entryCount = header.readUInt32LE(0x24);
    const indexOffset = header.readUInt32LE(0x28);
    const indexSize = header.readUInt32LE(0x2c);
    const entrySize = entryCount > 0 ? indexSize / entryCount : 0;
    return {
        majorVersion: header.readUInt32LE(4),
        minorVersion: header.readUInt32LE(8),
        entryCount,
        indexOffset,
        indexSize,
        entrySize:
            entrySize === ENTRY_SIZE_WITHOUT_INSTANCE_HIGH || entrySize === ENTRY_SIZE_WITH_INSTANCE_HIGH
                ? entrySize
                : null,
    };
}

export interface DbpfEntry {
    typeIdentifier: number;
    groupIdentifier: number;
    instanceIdentifier: number;
    instanceIdentifierHigh: number;
    offsetInBytes: number;
    sizeInBytes: number;
}

export function readIndex(fd: number, byteOffset: number, header: DbpfHeader): DbpfEntry[] {
    if (header.entrySize === null) {
        return [];
    }
    const index = readAt(fd, byteOffset + header.indexOffset, header.indexSize);
    const entries: DbpfEntry[] = [];
    const hasInstanceHigh = header.entrySize === ENTRY_SIZE_WITH_INSTANCE_HIGH;
    for (let i = 0; i < header.entryCount; i += 1) {
        const position = i * header.entrySize;
        if (position + header.entrySize > index.length) {
            break;
        }
        const typeIdentifier = index.readUInt32LE(position);
        const groupIdentifier = index.readUInt32LE(position + 4);
        const instanceIdentifier = index.readUInt32LE(position + 8);
        const instanceIdentifierHigh = hasInstanceHigh ? index.readUInt32LE(position + 12) : 0;
        const offsetInBytes = index.readUInt32LE(position + (hasInstanceHigh ? 16 : 12));
        const sizeInBytes = index.readUInt32LE(position + (hasInstanceHigh ? 20 : 16));
        entries.push({
            typeIdentifier,
            groupIdentifier,
            instanceIdentifier,
            instanceIdentifierHigh,
            offsetInBytes,
            sizeInBytes,
        });
    }
    return entries;
}

export function readResourceBytes(fd: number, byteOffset: number, entry: DbpfEntry, length: number = entry.sizeInBytes): Buffer {
    return readAt(fd, byteOffset + entry.offsetInBytes, length);
}

export interface DbpfPackage {
    header: DbpfHeader;
    entries: DbpfEntry[];
}

export function readPackage(fd: number, byteOffset: number): DbpfPackage | null {
    const header = readHeader(fd, byteOffset);
    if (header === null) {
        return null;
    }
    return { header, entries: readIndex(fd, byteOffset, header) };
}

export function groupByType(entries: DbpfEntry[]): Map<number, DbpfEntry[]> {
    const types = new Map<number, DbpfEntry[]>();
    for (const entry of entries) {
        const group = types.get(entry.typeIdentifier);
        if (group === undefined) {
            types.set(entry.typeIdentifier, [entry]);
        } else {
            group.push(entry);
        }
    }
    return types;
}
