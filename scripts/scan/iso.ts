import { closeSync, openSync } from "node:fs";
import { readRootDirectory, collectFiles } from "../lib/iso9660.ts";
import { readPackage, groupByType } from "../lib/dbpf.ts";

// Scans a disc for every .package file reachable directly in its filesystem
// tree (not sealed inside an installer archive), then aggregates the
// resource types found across all of them — so we know what type IDs to
// look for before writing a reader against a new format.
function main(): void {
    const isoPath = process.argv[2];
    if (!isoPath) {
        console.error("usage: iso.ts <path-to-iso>");
        process.exitCode = 1;
        return;
    }

    console.log(`Opening ${isoPath} ...`);
    const fd = openSync(isoPath, "r");
    try {
        const root = readRootDirectory(fd);
        if (root === null) {
            console.error("not an ISO 9660 image");
            process.exitCode = 1;
            return;
        }

        console.log("Walking directory tree ...");
        const packages = collectFiles(fd, root.rootLba, root.rootSize, (name) => name.toLowerCase().endsWith(".package"), {
            namesAreUCS2: root.namesAreUCS2,
        });
        console.log(`  ${packages.length} .package file(s) found\n`);

        const allTypes = new Map<number, number>();
        for (const file of packages) {
            const parsed = readPackage(fd, file.lba * 2048);
            if (parsed === null) {
                continue;
            }
            for (const [typeIdentifier, entries] of groupByType(parsed.entries)) {
                allTypes.set(typeIdentifier, (allTypes.get(typeIdentifier) ?? 0) + entries.length);
            }
        }

        console.log("Resource types found across all packages:");
        console.log(`  ${"Type ID".padStart(12)}  ${"Count".padStart(8)}`);
        const sorted = [...allTypes.entries()].sort((a, b) => b[1] - a[1]);
        for (const [typeIdentifier, count] of sorted) {
            console.log(`  0x${typeIdentifier.toString(16).padStart(8, "0")}  ${String(count).padStart(8)}`);
        }
    } finally {
        closeSync(fd);
    }
}

main();
