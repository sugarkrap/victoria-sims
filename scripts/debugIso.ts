import { closeSync, openSync } from "node:fs";
import { readAt, SECTOR_SIZE, listDirectory } from "./lib/iso9660.ts";

// Dumps every volume descriptor on a disc image, and the top-level directory
// of its primary (non-Joliet) tree — the shape a reader sees before it knows
// to look for Joliet's long names. Useful for a first look at an unfamiliar
// disc's layout.
const DESCRIPTOR_NAMES: Record<number, string> = { 0: "boot", 1: "primary", 2: "supplementary", 255: "terminator" };

function main(): void {
    const isoPath = process.argv[2];
    if (!isoPath) {
        console.error("usage: debugIso.ts <path-to-iso>");
        process.exitCode = 1;
        return;
    }

    const fd = openSync(isoPath, "r");
    try {
        for (let sector = 16; sector < 32; sector += 1) {
            const descriptor = readAt(fd, sector * SECTOR_SIZE, SECTOR_SIZE);
            if (descriptor.length < 6 || descriptor.toString("ascii", 1, 6) !== "CD001") {
                break;
            }
            const type = descriptor[0]!;
            console.log(`Sector ${sector}: type=${type} (${DESCRIPTOR_NAMES[type] ?? "?"})`);

            if (type === 1) {
                const rootLba = descriptor.readUInt32LE(158);
                const rootSize = descriptor.readUInt32LE(166);
                console.log(`  Root dir: lba=${rootLba}, size=${rootSize}`);

                const entries = listDirectory(fd, rootLba, rootSize).slice(0, 50);
                for (const entry of entries) {
                    console.log(
                        `  ${entry.isDirectory ? "DIR " : "FILE"} ${entry.name.padEnd(40)}  lba=${entry.lba}  size=${entry.size}`,
                    );
                }
            }
        }
    } finally {
        closeSync(fd);
    }
}

main();
