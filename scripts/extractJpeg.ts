import { closeSync, openSync, writeFileSync } from "node:fs";
import { findInstallerLba, findRarOffset, findRarEntry } from "./lib/installer.ts";
import { readPackage, readResourceBytes } from "./lib/dbpf.ts";

// Extracts every JPEG-typed resource (0x856DDBAC) from a package sealed
// inside a disc's installer archive, so its actual content can be inspected
// directly.
const JPEG_TYPE = 0x856ddbac;

function main(): void {
    const isoPath = process.argv[2];
    const target = process.argv[3] ?? "TSData/Res/GlobalLots/CAS!.package";
    if (!isoPath) {
        console.error("usage: extractJpeg.ts <path-to-iso> [package-path-inside-installer]");
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
        const entry = findRarEntry(fd, lba, rarOffset, target);
        if (entry === null) {
            console.error(`${target} not found in the archive`);
            process.exitCode = 1;
            return;
        }

        const packageOffset = lba * 2048 + entry.dataOffset;
        const parsed = readPackage(fd, packageOffset);
        if (parsed === null) {
            console.error(`${target} is not a DBPF package`);
            process.exitCode = 1;
            return;
        }

        let index = 0;
        for (const resourceEntry of parsed.entries) {
            if (resourceEntry.typeIdentifier !== JPEG_TYPE) {
                continue;
            }
            console.log(
                `Entry ${index}: grp=0x${resourceEntry.groupIdentifier.toString(16).padStart(8, "0")} ` +
                    `inst=0x${resourceEntry.instanceIdentifier.toString(16).padStart(8, "0")} sz=${resourceEntry.sizeInBytes}`,
            );
            const data = readResourceBytes(fd, packageOffset, resourceEntry);
            const outname = `cas_jpeg_${index}.jpg`;
            if (data[0] === 0xff && data[1] === 0xd8) {
                writeFileSync(outname, data);
                console.log(`  -> saved pure JPEG ${resourceEntry.sizeInBytes}B to ${outname}`);
            } else if (data.length >= 6 && data[4] === 0xff && data[5] === 0xd8) {
                writeFileSync(outname, data.subarray(4));
                console.log(`  -> saved JPEG (offset 4) to ${outname}`);
            } else {
                console.log(`  -> magic=${data.subarray(0, 8).toString("hex")}`);
            }
            index += 1;
        }
        if (index === 0) {
            console.log("no JPEG-typed resources found");
        }
    } finally {
        closeSync(fd);
    }
}

main();
