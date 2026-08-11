import { readSync } from "node:fs";

export const SECTOR_SIZE = 2048;

export function readAt(fd: number, position: number, length: number): Buffer {
    const buffer = Buffer.alloc(length);
    const bytesRead = readSync(fd, buffer, 0, length, position);
    return bytesRead === length ? buffer : buffer.subarray(0, bytesRead);
}

export function readSector(fd: number, lba: number, offsetInSector: number, length: number): Buffer {
    return readAt(fd, lba * SECTOR_SIZE + offsetInSector, length);
}

function escapesAreJoliet(descriptor: Buffer): boolean {
    const escapes = descriptor.subarray(88, 91);
    if (escapes[0] !== 0x25 || escapes[1] !== 0x2f) {
        return false;
    }
    return escapes[2] === 0x40 || escapes[2] === 0x43 || escapes[2] === 0x45;
}

export interface VolumeRoot {
    rootLba: number;
    rootSize: number;
    namesAreUCS2: boolean;
}

export interface VolumeDescriptors {
    primaryRoot: VolumeRoot | null;
    jolietRoot: VolumeRoot | null;
}

// Scans every volume descriptor and returns both roots when present: the
// plain primary tree, which is always 8.3-uppercase and never absent, and
// the Joliet supplementary tree, which carries names past 8.3 but is an
// optional extension a disc may not have.
export function readVolumeDescriptors(fd: number): VolumeDescriptors {
    let primaryRoot: VolumeRoot | null = null;
    let jolietRoot: VolumeRoot | null = null;

    for (let sector = 16; sector <= 80; sector += 1) {
        const descriptor = readAt(fd, sector * SECTOR_SIZE, SECTOR_SIZE);
        if (descriptor.length < 6 || descriptor.toString("ascii", 1, 6) !== "CD001") {
            break;
        }
        if (descriptor[0] === 1 && primaryRoot === null) {
            primaryRoot = {
                rootLba: descriptor.readUInt32LE(158),
                rootSize: descriptor.readUInt32LE(166),
                namesAreUCS2: false,
            };
        } else if (descriptor[0] === 2 && escapesAreJoliet(descriptor)) {
            jolietRoot = {
                rootLba: descriptor.readUInt32LE(158),
                rootSize: descriptor.readUInt32LE(166),
                namesAreUCS2: true,
            };
        } else if (descriptor[0] === 255) {
            break;
        }
    }
    return { primaryRoot, jolietRoot };
}

// Mirrors engine/source/discReader.c: prefer the Joliet root when present,
// since that is the one carrying names past 8.3 — the plain primary tree
// truncates the same disc's files to "MATERIAL.PAC" and the like. Use this
// for browsing; for locating one specific, always-8.3-safe file such as a
// disc's installer executable, read the primary tree directly instead, since
// that name is guaranteed present and guaranteed uppercase whether or not
// this disc carries Joliet at all.
export function readRootDirectory(fd: number): VolumeRoot | null {
    const { primaryRoot, jolietRoot } = readVolumeDescriptors(fd);
    return jolietRoot ?? primaryRoot;
}

function decodeName(rawName: Buffer, isDirectory: boolean, namesAreUCS2: boolean): string {
    if (namesAreUCS2) {
        let text = rawName.swap16().toString("utf16le");
        if (!isDirectory) {
            text = text.split(";")[0]!;
        }
        return text;
    }
    let text = rawName.toString("ascii");
    if (!isDirectory) {
        text = text.split(";")[0]!;
    }
    return text;
}

export interface DirectoryEntry {
    recordLength: number;
    isDirectory: boolean;
    lba: number;
    size: number;
    name: string;
}

function parseDirectoryRecord(data: Buffer, position: number, namesAreUCS2: boolean): DirectoryEntry | null {
    const recordLength = data[position]!;
    if (recordLength === 0) {
        return null;
    }
    const flags = data[position + 25]!;
    const isDirectory = (flags & 2) !== 0;
    const nameLength = data[position + 32]!;
    const rawName = data.subarray(position + 33, position + 33 + nameLength);
    const isSelfOrParent = nameLength === 1 && (rawName[0] === 0x00 || rawName[0] === 0x01);
    const name = isSelfOrParent
        ? rawName[0] === 0x00 ? "." : ".."
        : decodeName(rawName, isDirectory, namesAreUCS2);
    return {
        recordLength,
        isDirectory,
        lba: data.readUInt32LE(position + 2),
        size: data.readUInt32LE(position + 10),
        name,
    };
}

export interface DirectoryListOptions {
    namesAreUCS2?: boolean;
}

export function listDirectory(fd: number, lba: number, size: number, options: DirectoryListOptions = {}): DirectoryEntry[] {
    const { namesAreUCS2 = false } = options;
    const data = readAt(fd, lba * SECTOR_SIZE, size);
    const entries: DirectoryEntry[] = [];
    let position = 0;
    while (position < data.length) {
        const record = parseDirectoryRecord(data, position, namesAreUCS2);
        if (record === null) {
            position = (Math.floor(position / SECTOR_SIZE) + 1) * SECTOR_SIZE;
            continue;
        }
        if (record.name !== "." && record.name !== "..") {
            entries.push(record);
        }
        position += record.recordLength;
    }
    return entries;
}

export function findFileInDirectory(
    fd: number,
    lba: number,
    size: number,
    targetName: string,
    options?: DirectoryListOptions,
): DirectoryEntry | null {
    for (const entry of listDirectory(fd, lba, size, options)) {
        if (!entry.isDirectory && entry.name === targetName) {
            return entry;
        }
    }
    return null;
}

export interface CollectFilesOptions extends DirectoryListOptions {
    maxDepth?: number;
}

export function collectFiles(
    fd: number,
    lba: number,
    size: number,
    predicate: (name: string) => boolean,
    options: CollectFilesOptions = {},
): DirectoryEntry[] {
    const { maxDepth = 8, ...listOptions } = options;
    const found: DirectoryEntry[] = [];
    const visit = (dirLba: number, dirSize: number, depth: number): void => {
        for (const entry of listDirectory(fd, dirLba, dirSize, listOptions)) {
            if (entry.isDirectory) {
                if (depth < maxDepth) {
                    visit(entry.lba, entry.size, depth + 1);
                }
            } else if (predicate(entry.name)) {
                found.push(entry);
            }
        }
    };
    visit(lba, size, 0);
    return found;
}
