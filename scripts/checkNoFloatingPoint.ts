import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { CheckReport, commandExists, run, runMain } from "./lib/checkReport.ts";

// Enforces a claim the font path makes about itself: it does no floating
// point.
//
// glyphRaster.h says so in as many words, and the reason matters. The floor
// of the device ladder is an ARMv5TE with no unit for it, where every float
// is a call into a software library, and rasterizing one glyph is tens of
// thousands of arithmetic operations. Fixed point there is not a stylistic
// preference — it is the difference between a font that appears and one that
// does not.
//
// A claim like that rots in exactly one way: somebody writes `x / 2.0f` in a
// helper, it compiles everywhere, it is correct everywhere, and it is thirty
// times slower on the one machine nobody has to hand. Grepping the sources
// for `float` would catch that and would also catch the word in a comment,
// so this does the only thing that cannot be argued with: it cross-compiles
// for the floor of the ladder and looks for calls to the soft-float library
// in the objects that come out.
//
// Needs a compiler that can target ARMv5TE. Clang can out of the box; a GNU
// cross-compiler works too. Skipped rather than failed when there is none,
// because a developer without one should still be able to run the checks —
// the build in continuous integration has one and does not skip.

// Every module between a font on the disc and pixels on the screen. Not the
// renderers: renderSoftware projects a camera and shades a triangle, which
// is floating point on purpose and is charged for elsewhere.
const MODULES = ["fontReader", "glyphRaster", "fontAtlas", "builtinFont", "interfaceSurface", "interfaceMenu"];

// The names the ARM EABI gives its soft-float helpers, plus the older libgcc
// spellings. A call to any of them from a module above is the bug.
const FLOAT_SYMBOLS = new RegExp(
    "__aeabi_(f|d)(add|sub|mul|div|cmp|rsub|neg)" +
        "|__aeabi_(i2f|i2d|ui2f|ui2d|f2iz|d2iz|f2d|d2f)" +
        "|__(add|sub|mul|div|neg)[sd]f3" +
        "|__float[sd]i[sd]f" +
        "|__fix[sd]fsi" +
        "|__(eq|ne|lt|le|gt|ge)[sd]f2",
    "g",
);

function pickArmCompiler(): string[] | null {
    const override = process.env["ARM_COMPILER"];
    if (override) {
        return override.split(" ");
    }
    if (commandExists("arm-linux-gnueabi-gcc")) {
        return ["arm-linux-gnueabi-gcc"];
    }
    if (commandExists("clang")) {
        return ["clang", "--target=armv5te-linux-gnueabi"];
    }
    return null;
}

function pickSymbolReader(): string | null {
    const override = process.env["SYMBOL_READER"];
    if (override) {
        return override;
    }
    if (commandExists("llvm-nm")) {
        return "llvm-nm";
    }
    if (commandExists("nm")) {
        return "nm";
    }
    return null;
}

function main(): void {
    const report = new CheckReport();
    const armCompiler = pickArmCompiler();
    if (armCompiler === null) {
        console.log("no ARM compiler, skipping the no-floating-point check");
        return;
    }
    const symbolReader = pickSymbolReader();
    if (symbolReader === null) {
        console.log("no symbol reader, skipping the no-floating-point check");
        return;
    }

    const scratchDirectory = mkdtempSync(join(tmpdir(), "victoria-nofloat-"));
    try {
        console.log("compiling the font path for ARMv5TE with no floating point unit...");
        for (const module of MODULES) {
            const objectPath = join(scratchDirectory, `${module}.o`);
            const [compilerCommand, ...compilerArgs] = armCompiler as [string, ...string[]];
            run(compilerCommand, [
                ...compilerArgs,
                "-std=c99",
                "-pedantic",
                "-Wall",
                "-Werror",
                "-march=armv5te",
                "-mfloat-abi=soft",
                "-ffreestanding",
                "-DVICTORIA_FREESTANDING_BUILTINS=1",
                "-Iengine/include",
                "-I.",
                "-c",
                `engine/source/${module}.c`,
                "-o",
                objectPath,
            ]);

            const symbols = run(symbolReader, ["--undefined-only", objectPath]);
            const found = new Set((symbols.match(FLOAT_SYMBOLS) ?? []).sort());
            if (found.size > 0) {
                report.fail(`${module}.c calls the soft-float library:`);
                for (const symbol of found) {
                    console.error(`    ${symbol}`);
                }
            } else {
                report.ok(`${module}: no floating point at all`);
            }
        }
    } finally {
        rmSync(scratchDirectory, { recursive: true, force: true });
    }

    report.finish("no-floating-point");
}

runMain(main);
