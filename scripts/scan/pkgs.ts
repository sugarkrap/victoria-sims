import { closeSync, openSync } from "node:fs";
import { findInstallerLba, findRarOffset, findRarEntries } from "../lib/installer.ts";
import { readPackage, groupByType, readResourceBytes } from "../lib/dbpf.ts";

// Scans several named packages sealed inside a disc's installer, reporting
// the resource types each one carries — a quick survey across multiple
// packages at once, rather than one at a time like catBin.ts.
const DEFAULT_TARGETS = ["TSData/Res/Catalog/Skins/Skins.package", "TSData/Res/GlobalLots/CAS!.package"];

function hex32(value: number): string {
    return `0x${value.toString(16).padStart(8, "0")}`;
}

function scanPackage(fd: number, packageOffset: number, label: string): void {
    console.log(`\n=== ${label} ===`);
    const parsed = readPackage(fd, packageOffset);
    if (parsed === null) {
        console.log("  NOT a DBPF package");
        return;
    }
    console.log(`  ${parsed.header.entryCount} entries, entry_size=${parsed.header.entrySize}`);

    const types = groupByType(parsed.entries);
    const sorted = [...types.entries()].sort((a, b) => b[1].length - a[1].length);
    for (const [typeIdentifier, entries] of sorted) {
        const first = entries[0]!;
        const peek = first.sizeInBytes > 0 ? readResourceBytes(fd, packageOffset, first, Math.min(first.sizeInBytes, 12)) : Buffer.alloc(0);
        const magic = peek.length > 0 ? peek.subarray(0, 8).toString("hex") : "????";
        console.log(`  ${hex32(typeIdentifier)}  ${String(entries.length).padStart(5)} entries  e.g. sz=${first.sizeInBytes}  magic=${magic}`);
    }
}

function main(): void {
    const isoPath = process.argv[2];
    const targets = process.argv.length > 3 ? process.argv.slice(3) : DEFAULT_TARGETS;
    if (!isoPath) {
        console.error("usage: pkgs.ts <path-to-iso> [package-path-inside-installer...]");
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
        const rarOffset = findRarOffset(fd, lba);
        if (rarOffset === null) {
            console.error("no RAR archive found inside TSDATA.EXE");
            process.exitCode = 1;
            return;
        }
        console.log(`TSDATA.EXE LBA=${lba}, RAR at 0x${rarOffset.toString(16)}`);

        const found = findRarEntries(fd, lba, rarOffset, targets);
        for (const target of targets) {
            const entry = found.get(target);
            if (entry === undefined) {
                console.log(`\n=== ${target} NOT FOUND ===`);
                continue;
            }
            scanPackage(fd, lba * 2048 + entry.dataOffset, target);
        }
    } finally {
        closeSync(fd);
    }
}

main();
