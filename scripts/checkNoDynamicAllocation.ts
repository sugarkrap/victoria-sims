import { mkdtempSync, readdirSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, basename } from "node:path";
import { CheckReport, commandExists, run, runMain } from "./lib/checkReport.ts";

// Enforces the project's hardest rule: our own code never allocates at run
// time. Checked two ways, because either one alone is easy to fool.
//
//   1. No allocator names appear in the sources we own.
//   2. No object file we compile carries an undefined reference to one.
//
// Platform libraries (X11, Mesa, the browser) allocate internally and are
// out of our reach; that is a known limit, not an exemption for engine code.

const SOURCE_DIRECTORIES = ["engine", "platform", "render", "tests", "utils"];
const ALLOCATOR_NAMES = ["malloc", "calloc", "realloc", "free", "aligned_alloc", "posix_memalign", "strdup", "mmap", "brk", "sbrk"];

// Matched as calls rather than bare words, so prose like "cheap but not
// free" does not trip the check. Anything that slips past this is still
// caught by the symbol check below, which is the real backstop.
const FORBIDDEN_PATTERN = new RegExp(`\\b(${ALLOCATOR_NAMES.join("|")})\\s*\\(`);

function findSources(directory: string): string[] {
    let entries;
    try {
        entries = readdirSync(directory, { recursive: true, withFileTypes: true });
    } catch {
        return [];
    }
    return entries
        .filter((entry) => entry.isFile() && (entry.name.endsWith(".c") || entry.name.endsWith(".h")))
        .map((entry) => join(entry.parentPath, entry.name));
}

function checkSources(report: CheckReport): void {
    console.log("checking sources for allocator use...");
    let anyMatch = false;
    for (const directory of SOURCE_DIRECTORIES) {
        for (const path of findSources(directory)) {
            const lines = readFileSync(path, "utf8").split("\n");
            lines.forEach((line, index) => {
                if (FORBIDDEN_PATTERN.test(line)) {
                    console.error(`${path}:${index + 1}:${line}`);
                    anyMatch = true;
                }
            });
        }
    }
    if (anyMatch) {
        report.fail("allocator reference found under a checked directory");
    } else {
        report.ok("sources clean");
    }
}

function checkCompiledObjects(report: CheckReport): void {
    if (!commandExists("nm")) {
        report.ok("nm unavailable, skipping object symbol check");
        return;
    }
    console.log("checking compiled objects for allocator symbols...");

    const hostCompiler = process.env["HOST_COMPILER"] ?? "clang";
    const scratchDirectory = mkdtempSync(join(tmpdir(), "victoria-nodyn-"));
    try {
        const sourceGroups = [
            "engine/source",
            "render/openGLES2",
            "render/software",
            "platform/linux",
        ];
        const objects: string[] = [];
        for (const directory of sourceGroups) {
            for (const path of findSources(directory).filter((p) => p.endsWith(".c"))) {
                const objectPath = join(scratchDirectory, `${basename(path, ".c")}.o`);
                run(hostCompiler, ["-std=c99", "-I.", "-Iengine/include", "-c", path, "-o", objectPath]);
                objects.push(objectPath);
            }
        }
        const stringsObject = join(scratchDirectory, "strings.o");
        run(hostCompiler, ["-std=c99", "-I.", "-Iengine/include", "-c", "utils/strings.c", "-o", stringsObject]);
        objects.push(stringsObject);

        const nmOutput = run("nm", ["--undefined-only", ...objects]);
        const found = nmOutput
            .split("\n")
            .filter((line) => ALLOCATOR_NAMES.some((name) => line.trim().endsWith(` ${name}`)));

        if (found.length > 0) {
            found.forEach((line) => console.error(line));
            report.fail("compiled object references an allocator");
        } else {
            report.ok("objects clean");
        }
    } finally {
        rmSync(scratchDirectory, { recursive: true, force: true });
    }
}

function main(): void {
    const report = new CheckReport();
    checkSources(report);
    checkCompiledObjects(report);
    report.finish("no-dynamic-allocation");
}

runMain(main);
