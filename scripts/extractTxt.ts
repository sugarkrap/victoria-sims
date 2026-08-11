import { closeSync, openSync } from "node:fs";
import { findInstallerLba, findRarOffset, findRarEntries } from "./lib/installer.ts";
import { readAt } from "./lib/iso9660.ts";

// Extracts named text/config files from the RAR archive sealed inside a
// disc's installer, so their content can be read directly.
const DEFAULT_TARGETS = [
    "TSData/Res/Lights/LightRigs/CASThumbnails.txt",
    "TSData/Res/Lights/LightRigs/SimThumbnails.txt",
    "TSData/Res/Lights/cas/casStudio.txt",
    "TSData/Res/Lights/CAS_lighting.txt",
    "TSData/Res/Lights/CAS_lights.txt",
    "TSData/Res/Lights/cas/casNeighborhoodPose.txt",
];

function main(): void {
    const isoPath = process.argv[2];
    const targets = process.argv.length > 3 ? process.argv.slice(3) : DEFAULT_TARGETS;
    if (!isoPath) {
        console.error("usage: extractTxt.ts <path-to-iso> [target-path...]");
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

        const found = findRarEntries(fd, lba, rarOffset, targets);
        for (const target of targets) {
            const entry = found.get(target);
            console.log(`\n${"=".repeat(70)}`);
            console.log(`  ${target}`);
            console.log("=".repeat(70));
            if (entry === undefined) {
                console.log("  <not found>");
                continue;
            }
            const content = readAt(fd, lba * 2048 + entry.dataOffset, entry.packedSize);
            console.log(content.toString("latin1"));
        }
    } finally {
        closeSync(fd);
    }
}

main();
