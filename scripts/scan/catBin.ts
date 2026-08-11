import { closeSync, openSync } from "node:fs";
import { findInstallerLba, findRarOffset, findRarEntry } from "../lib/installer.ts";
import { readPackage, groupByType, readResourceBytes } from "../lib/dbpf.ts";

// Reads a catalogue bin package sealed inside a disc's installer and reports
// what resource types it contains, plus the first few entries of each type —
// a first pass over an unfamiliar package before writing a reader against it.
const DEFAULT_TARGET = "TSData/Res/Catalog/Bins/globalcatbin.bundle.package";
const MAX_ENTRIES_PER_TYPE = 3;

function hex32(value: number): string {
    return `0x${value.toString(16).padStart(8, "0")}`;
}

function main(): void {
    const isoPath = process.argv[2];
    const target = process.argv[3] ?? DEFAULT_TARGET;
    if (!isoPath) {
        console.error("usage: catBin.ts <path-to-iso> [package-path-inside-installer]");
        process.exitCode = 1;
        return;
    }

    const fd = openSync(isoPath, "r");
    try {
        console.log("Finding TSDATA.EXE ...");
        const lba = findInstallerLba(fd);
        if (lba === null) {
            console.error("ERROR: TSDATA.EXE not found");
            process.exitCode = 1;
            return;
        }
        console.log(`  LBA=${lba}`);

        console.log("Finding RAR offset ...");
        const rarOffset = findRarOffset(fd, lba);
        if (rarOffset === null) {
            console.error("ERROR: RAR not found");
            process.exitCode = 1;
            return;
        }
        console.log(`  RAR at offset 0x${rarOffset.toString(16)}`);

        console.log(`\nLocating ${target} ...`);
        const entry = findRarEntry(fd, lba, rarOffset, target);
        if (entry === null) {
            console.error("ERROR: entry not found");
            process.exitCode = 1;
            return;
        }
        console.log(
            `  offset=0x${entry.dataOffset.toString(16)}, packed=${Math.floor(entry.packedSize / 1024)}KB, unpacked=${Math.floor(entry.unpackedSize / 1024)}KB`,
        );

        console.log("\nScanning DBPF ...");
        const packageOffset = lba * 2048 + entry.dataOffset;
        const parsed = readPackage(fd, packageOffset);
        if (parsed === null) {
            console.log("Not a DBPF package!");
            process.exitCode = 1;
            return;
        }
        console.log(
            `  DBPF version ${parsed.header.majorVersion}.x, ${parsed.header.entryCount} entries, ` +
                `index at 0x${parsed.header.indexOffset.toString(16)} (${parsed.header.indexSize} bytes), entry size=${parsed.header.entrySize}`,
        );

        const types = groupByType(parsed.entries);
        console.log("\n  Resource types:");
        const sorted = [...types.entries()].sort((a, b) => b[1].length - a[1].length);
        for (const [typeIdentifier, entries] of sorted) {
            console.log(`    ${hex32(typeIdentifier)}  ${String(entries.length).padStart(5)} entries`);
            for (const resourceEntry of entries.slice(0, MAX_ENTRIES_PER_TYPE)) {
                const peek = readResourceBytes(fd, packageOffset, resourceEntry, Math.min(resourceEntry.sizeInBytes, 16));
                const magic = peek.length > 0 ? peek.subarray(0, 4).toString("hex") : "????";
                console.log(
                    `      grp=${hex32(resourceEntry.groupIdentifier)} inst=${resourceEntry.instanceIdentifierHigh.toString(16).padStart(8, "0")}${resourceEntry.instanceIdentifier.toString(16).padStart(8, "0")} ` +
                        `off=0x${resourceEntry.offsetInBytes.toString(16)} sz=${resourceEntry.sizeInBytes}  magic=${magic}`,
                );
            }
        }
    } finally {
        closeSync(fd);
    }
}

main();
