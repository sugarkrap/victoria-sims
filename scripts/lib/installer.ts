import { readAt, readVolumeDescriptors, findFileInDirectory } from "./iso9660.ts";

const RAR_SIGNATURE = Buffer.from([0x52, 0x61, 0x72, 0x21, 0x1a, 0x07, 0x00]);
const SEARCH_CHUNK = 65529;
const SEARCH_LIMIT = 64 * 1024 * 1024;

const BLOCK_TYPE_FILE = 0x74;
const BLOCK_TYPE_END = 0x7b;

// Looked up on the primary tree specifically, not Joliet: an installer's own
// name is always within 8.3 (it has to run on the DOS/Windows loader that
// reads a disc with no Joliet support at all), so the primary tree — which
// every disc has, unlike Joliet — names it correctly without truncation.
export function findInstallerLba(fd: number, exeName = "TSDATA.EXE"): number | null {
    const { primaryRoot } = readVolumeDescriptors(fd);
    if (primaryRoot === null) {
        return null;
    }
    const entry = findFileInDirectory(fd, primaryRoot.rootLba, primaryRoot.rootSize, exeName, {
        namesAreUCS2: false,
    });
    return entry === null ? null : entry.lba;
}

export function findRarOffset(fd: number, lba: number): number | null {
    for (let base = 0; base < SEARCH_LIMIT; base += SEARCH_CHUNK) {
        const chunk = readAt(fd, lba * 2048 + base, 65536);
        const index = chunk.indexOf(RAR_SIGNATURE);
        if (index !== -1) {
            return base + index;
        }
        if (chunk.length < 65536) {
            break;
        }
    }
    return null;
}

interface RarSkipBlock {
    isFile: false;
    nextPosition: number;
}

interface RarFileBlock {
    isFile: true;
    name: string;
    dataOffset: number;
    packedSize: number;
    unpackedSize: number;
    nextPosition: number;
}

type RarBlock = RarSkipBlock | RarFileBlock;

function readFileBlockHeader(fd: number, lba: number, position: number): RarBlock | null {
    const header = readAt(fd, lba * 2048 + position, 40);
    if (header.length < 7) {
        return null;
    }
    const blockType = header[2]!;
    const blockFlags = header.readUInt16LE(3);
    const blockSize = header.readUInt16LE(5);
    if (blockSize === 0 || blockType === BLOCK_TYPE_END) {
        return null;
    }
    if (blockType !== BLOCK_TYPE_FILE) {
        const hasData = (blockFlags & 0x8000) !== 0;
        const dataLength = hasData && header.length >= 11 ? header.readUInt32LE(7) : 0;
        return { isFile: false, nextPosition: position + blockSize + dataLength };
    }

    let packedSize = header.readUInt32LE(7);
    let unpackedSize = header.readUInt32LE(11);
    const nameSize = header.readUInt16LE(26);
    let nameOffset = 32;
    if ((blockFlags & 0x0100) !== 0) {
        const extra = readAt(fd, lba * 2048 + position + 32, 8);
        packedSize += extra.readUInt32LE(0) * 2 ** 32;
        unpackedSize += extra.readUInt32LE(4) * 2 ** 32;
        nameOffset = 40;
    }
    const nameBytes = readAt(fd, lba * 2048 + position + nameOffset, nameSize);
    const name = nameBytes.toString("ascii").replaceAll("\\", "/");
    const dataOffset = position + blockSize;

    return {
        isFile: true,
        name,
        dataOffset,
        packedSize,
        unpackedSize,
        nextPosition: dataOffset + packedSize,
    };
}

export interface RarEntry {
    name: string;
    dataOffset: number;
    packedSize: number;
    unpackedSize: number;
}

// Walks every file entry in the archive, calling `visit` with each one until
// it returns true or the archive ends. For matching by anything other than
// an exact name — findRarEntry(s) below cover that case more cheaply.
export function walkRarEntries(fd: number, lba: number, rarOffset: number, visit: (entry: RarEntry) => boolean | void): void {
    let position = rarOffset;
    for (;;) {
        const block = readFileBlockHeader(fd, lba, position);
        if (block === null) {
            return;
        }
        if (block.isFile) {
            const entry: RarEntry = {
                name: block.name,
                dataOffset: block.dataOffset,
                packedSize: block.packedSize,
                unpackedSize: block.unpackedSize,
            };
            if (visit(entry) === true) {
                return;
            }
        }
        position = block.nextPosition;
    }
}

export function findRarEntries(fd: number, lba: number, rarOffset: number, targetNames: Iterable<string>): Map<string, RarEntry> {
    const remaining = new Set(targetNames);
    const results = new Map<string, RarEntry>();
    let position = rarOffset;
    while (remaining.size > 0) {
        const block = readFileBlockHeader(fd, lba, position);
        if (block === null) {
            break;
        }
        if (block.isFile && remaining.has(block.name)) {
            results.set(block.name, {
                name: block.name,
                dataOffset: block.dataOffset,
                packedSize: block.packedSize,
                unpackedSize: block.unpackedSize,
            });
            remaining.delete(block.name);
        }
        position = block.nextPosition;
    }
    return results;
}

export function findRarEntry(fd: number, lba: number, rarOffset: number, targetName: string): RarEntry | null {
    const results = findRarEntries(fd, lba, rarOffset, [targetName]);
    return results.get(targetName) ?? null;
}

export interface InstallerResource extends RarEntry {
    lba: number;
    rarOffset: number;
}

export function readInstallerResource(fd: number, targetName: string, exeName = "TSDATA.EXE"): InstallerResource | null {
    const lba = findInstallerLba(fd, exeName);
    if (lba === null) {
        return null;
    }
    const rarOffset = findRarOffset(fd, lba);
    if (rarOffset === null) {
        return null;
    }
    const entry = findRarEntry(fd, lba, rarOffset, targetName);
    if (entry === null) {
        return null;
    }
    return { lba, rarOffset, ...entry };
}
