import { mkdirSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { buildSimFixture } from "./src/simFixture/build.ts";

function main(): void {
    const root = dirname(dirname(fileURLToPath(import.meta.url)));
    const output = join(root, "testAssets", "scenegraph", "sim_fixture.package");

    const { resources, data } = buildSimFixture();
    mkdirSync(dirname(output), { recursive: true });
    writeFileSync(output, data);
    process.stderr.write(`wrote ${output} — ${resources.length} resources, ${data.length} bytes\n`);
}

main();
