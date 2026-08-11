import { execFileSync } from "node:child_process";
import { cpSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { buildSimFixture } from "./src/simFixture/build.ts";
import { buildFakeInstaller } from "./src/simFixture/installerImage.ts";

// Regenerates testAssets/discs/testDisc.iso.
//
// The disc carries the scenegraph fixtures that already live in
// testAssets/, laid out the way a retail disc lays them out. Using the real
// fixtures rather than empty files is the point: the test that walks this
// image goes on to open what it finds with the package reader, so the two
// halves are exercised together and against structures a retail disc
// actually has.
//
// Requires xorriso. The image is committed and pinned by hash, so
// contributors only need this when the fixture itself has to change — after
// which testAssets/manifest.sha256 has to be updated too.
function main(): void {
    const repositoryRoot = dirname(dirname(fileURLToPath(import.meta.url)));
    const outputImage = join(repositoryRoot, "testAssets", "discs", "testDisc.iso");
    const scenegraph = join(repositoryRoot, "testAssets", "scenegraph");
    const buildRoot = mkdtempSync(join(tmpdir(), "victoria-testdisc-"));

    try {
        mkdirSync(join(buildRoot, "TSData", "Res", "Sims3D"), { recursive: true });
        mkdirSync(join(buildRoot, "TSData", "Res", "Materials"), { recursive: true });
        mkdirSync(join(buildRoot, "Support"), { recursive: true });

        cpSync(join(scenegraph, "teapot_model.package"), join(buildRoot, "TSData/Res/Sims3D/teapot_model.package"));
        cpSync(join(scenegraph, "animation.package"), join(buildRoot, "TSData/Res/Sims3D/animation.package"));
        cpSync(join(scenegraph, "textures.package"), join(buildRoot, "TSData/Res/Materials/textures.package"));
        const materialDefinitionPath = join(scenegraph, "material_definition.package");
        cpSync(materialDefinitionPath, join(buildRoot, "TSData/Res/Materials/material_definition.package"));

        // A whole Sim, authored rather than taken from anywhere: a skeleton,
        // three parts that skin to it, and catalogue entries naming
        // replacements for them. The teapot is one rigid model, so without
        // this the assembly, the wardrobe and everything that indexes a
        // part by its primitives had no fixture at all — and three defects
        // reached a screen through that gap. src/simFixture/build.ts writes
        // it and is the only description of those seven formats in one
        // place.
        const simFixturePath = join(scenegraph, "sim_fixture.package");
        const { data: simFixtureData } = buildSimFixture();
        writeFileSync(simFixturePath, simFixtureData);
        cpSync(simFixturePath, join(buildRoot, "TSData/Res/Sims3D/sim_fixture.package"));

        // A file named like a package that is not one, so detection is
        // forced to read the magic rather than trust the extension.
        writeFileSync(
            join(buildRoot, "TSData/Res/NotReally.package"),
            "this is not a package at all, despite the name\n",
        );

        // An installer archive, so the "sealed inside an installer" path has
        // something to find. Retail discs put the game inside one of these.
        writeFileSync(join(buildRoot, "Support/data1.cab"), "placeholder cabinet payload\n");
        writeFileSync(join(buildRoot, "Autorun.inf"), "[autorun]\n");

        // The navigable part of a repack's payload. Nothing here is
        // compressed: a fixture that pretended to be would be testing an
        // unpacker that does not exist. What it does test is everything up
        // to that point.
        const storedPackageBytes = readFileSync(materialDefinitionPath);
        writeFileSync(join(buildRoot, "TSData.exe"), buildFakeInstaller(storedPackageBytes));

        mkdirSync(dirname(outputImage), { recursive: true });
        execFileSync("xorriso", [
            "-as", "mkisofs",
            "-quiet",
            "-J", "-r",
            "-V", "VICTORIA_TEST",
            "-o", outputImage,
            buildRoot,
        ]);

        console.log(`wrote ${outputImage}`);
        console.log("remember to update testAssets/manifest.sha256");
    } finally {
        rmSync(buildRoot, { recursive: true, force: true });
    }
}

main();
