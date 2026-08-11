import { closeSync, openSync } from "node:fs";
import { findInstallerLba, findRarOffset, walkRarEntries, type RarEntry } from "./lib/installer.ts";
import { readHeader, readIndex, readResourceBytes } from "./lib/dbpf.ts";
import { decompress } from "./lib/refpack.ts";

// Analyzes every catalog entry from a disc's BodyShopThumbnails package:
//   - the distribution of src_grp values
//   - a cross-reference of catalog src_insts against the binary/XML
//     SKIN_ENTRY instances in Skins.package
//
// Both packages are sealed inside the disc's installer archive, and the
// catalog index itself is RefPack-compressed, so this exercises the same
// installer, DBPF and RefPack machinery the engine's own readers use.

const BINARY_SKIN = 0xebcf3e27;
const XML_SKIN = 0x0c1fe246;
const RESOURCE_KEY_LIST = 0xac506764;
const JPEG_TYPE = 0x856ddbac;
const CATALOG_INDEX_TYPE = 0x43494745;

function hex32(value: number): string {
    return `0x${value.toString(16).padStart(8, "0")}`;
}

function findEntryContaining(
    fd: number,
    lba: number,
    rarOffset: number,
    nameIncludes: readonly string[],
    nameExcludes: readonly string[] = [],
): RarEntry | null {
    let found: RarEntry | null = null;
    walkRarEntries(fd, lba, rarOffset, (entry) => {
        if (
            nameIncludes.some((needle) => entry.name.includes(needle)) &&
            !nameExcludes.some((needle) => entry.name.includes(needle))
        ) {
            found = entry;
            return true;
        }
        return false;
    });
    return found;
}

interface FirstSeen {
    index: number;
    srcType: number;
    srcInstance: number;
    thumbInstance: number;
}

function main(): void {
    const isoPath = process.argv[2];
    if (!isoPath) {
        console.error("usage: analyzeCatalog.ts <path-to-iso>");
        process.exitCode = 1;
        return;
    }

    const fd = openSync(isoPath, "r");
    try {
        const lba = findInstallerLba(fd);
        if (lba === null) {
            console.error("TSDATA.EXE not found");
            process.exitCode = 1;
            return;
        }
        console.log(`TSDATA.EXE LBA=${lba}`);

        const rarOffset = findRarOffset(fd, lba);
        if (rarOffset === null) {
            console.error("RAR archive not found");
            process.exitCode = 1;
            return;
        }
        console.log(`RAR offset=${hex32(rarOffset)}`);

        const bst = findEntryContaining(fd, lba, rarOffset, ["BodyShopThumbnails"]);
        const skins = findEntryContaining(fd, lba, rarOffset, ["Skins.package"], ["Locale"]);
        if (bst === null || skins === null) {
            console.error(`BodyShopThumbnails ${bst ? "found" : "NOT FOUND"}, Skins.package ${skins ? "found" : "NOT FOUND"}`);
            process.exitCode = 1;
            return;
        }
        console.log(`Found BodyShopThumbnails at offset=${hex32(bst.dataOffset)} size=${Math.floor(bst.packedSize / 1024)}KB`);
        console.log(`Found Skins.package at offset=${hex32(skins.dataOffset)} size=${Math.floor(skins.packedSize / 1024)}KB`);

        console.log(`\nReading Skins.package (${Math.floor(skins.packedSize / 1024)}KB)...`);
        const skinsOffset = lba * 2048 + skins.dataOffset;
        const skinsHeader = readHeader(fd, skinsOffset);
        if (skinsHeader === null) {
            console.error("Not DBPF!");
            process.exitCode = 1;
            return;
        }
        const skinsEntries = readIndex(fd, skinsOffset, skinsHeader);
        console.log(`  ${skinsHeader.entryCount} entries`);

        const binInstances = new Set<number>();
        const xmlInstances = new Set<number>();
        const rklInstances = new Set<number>();
        for (const entry of skinsEntries) {
            if (entry.typeIdentifier === BINARY_SKIN) {
                binInstances.add(entry.instanceIdentifier);
            } else if (entry.typeIdentifier === XML_SKIN) {
                xmlInstances.add(entry.instanceIdentifier);
            } else if (entry.typeIdentifier === RESOURCE_KEY_LIST) {
                rklInstances.add(entry.instanceIdentifier);
            }
        }
        console.log(`  binary SKIN_ENTRY: ${binInstances.size}, XML: ${xmlInstances.size}, RKL: ${rklInstances.size}`);

        const bstOffset = lba * 2048 + bst.dataOffset;
        const bstHeader = readHeader(fd, bstOffset);
        if (bstHeader === null) {
            console.error("BodyShopThumbnails is not DBPF!");
            process.exitCode = 1;
            return;
        }
        const bstEntries = readIndex(fd, bstOffset, bstHeader);
        console.log(`\nBodyShopThumbnails: ${bstHeader.entryCount} entries, index at offset ${bstHeader.indexOffset}`);

        const jpegInstances = new Set<number>();
        let catalogEntry: (typeof bstEntries)[number] | null = null;
        for (const entry of bstEntries) {
            if (entry.typeIdentifier === JPEG_TYPE) {
                jpegInstances.add(entry.instanceIdentifier);
            } else if (entry.typeIdentifier === CATALOG_INDEX_TYPE) {
                catalogEntry = entry;
            }
        }
        console.log(`  ${jpegInstances.size} JPEG thumbnails, catalog at offset ${catalogEntry?.offsetInBytes} size=${catalogEntry?.sizeInBytes}`);
        if (catalogEntry === null) {
            console.error("no catalog index found in BodyShopThumbnails");
            process.exitCode = 1;
            return;
        }

        const raw = readResourceBytes(fd, bstOffset, catalogEntry);
        const decompressed = decompress(raw);
        console.log(`  Catalog: compressed=${catalogEntry.sizeInBytes} -> decompressed=${decompressed.length}`);

        const version = decompressed.readUInt32LE(0);
        const count = decompressed.readUInt32LE(4);
        const entrySize = count ? Math.floor((decompressed.length - 8) / count) : 0;
        console.log(`  ver=${version} count=${count} entry_size=${entrySize}`);

        const srcGroupCounts = new Map<number, number>();
        const firstSeenByGroup = new Map<number, FirstSeen>();
        let matchedBinary = 0;
        let matchedXml = 0;
        let matchedRkl = 0;
        let inBst = 0;

        for (let i = 0; i < count; i += 1) {
            const base = 8 + i * entrySize;
            if (base + entrySize > decompressed.length) {
                break;
            }
            const srcType = decompressed.readUInt32LE(base + 4);
            const srcGroup = decompressed.readUInt32LE(base + 8);
            const srcInstance = decompressed.readUInt32LE(base + 12);
            const thumbInstance = decompressed.readUInt32LE(base + 28);

            srcGroupCounts.set(srcGroup, (srcGroupCounts.get(srcGroup) ?? 0) + 1);
            if (binInstances.has(srcInstance)) matchedBinary += 1;
            if (xmlInstances.has(srcInstance)) matchedXml += 1;
            if (rklInstances.has(srcInstance)) matchedRkl += 1;
            if (jpegInstances.has(thumbInstance)) inBst += 1;

            if (!firstSeenByGroup.has(srcGroup)) {
                firstSeenByGroup.set(srcGroup, { index: i, srcType, srcInstance, thumbInstance });
            }
        }

        console.log(`\nsrc_grp distribution (${srcGroupCounts.size} unique groups):`);
        const sortedGroups = [...srcGroupCounts.entries()].sort((a, b) => b[1] - a[1]);
        for (const [group, groupCount] of sortedGroups) {
            const first = firstSeenByGroup.get(group)!;
            console.log(
                `  ${hex32(group)}: ${String(groupCount).padStart(5)} entries  (first at idx ${first.index}: src_type=${hex32(first.srcType)} src_inst=${hex32(first.srcInstance)})`,
            );
        }

        console.log("\nCross-reference with Skins.package:");
        console.log(`  src_inst in binary SKIN_ENTRY: ${matchedBinary} / ${count}`);
        console.log(`  src_inst in XML SKIN_ENTRY:    ${matchedXml} / ${count}`);
        console.log(`  src_inst in RKL:               ${matchedRkl} / ${count}`);
        console.log(`  thumb_inst in BST:             ${inBst} / ${count}`);

        console.log("\nFirst 3 src_insts from catalog:");
        for (let i = 0; i < Math.min(3, count); i += 1) {
            const base = 8 + i * entrySize;
            const srcGroup = decompressed.readUInt32LE(base + 8);
            const srcInstance = decompressed.readUInt32LE(base + 12);
            const thumbInstance = decompressed.readUInt32LE(base + 28);
            console.log(`  Entry ${i}: src_grp=${hex32(srcGroup)} src_inst=${hex32(srcInstance)} thumb_inst=${hex32(thumbInstance)}`);
            console.log(`    -> src_inst in bin_insts: ${binInstances.has(srcInstance)}, xml_insts: ${xmlInstances.has(srcInstance)}`);
        }
    } finally {
        closeSync(fd);
    }
}

main();
