import { mkdirSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { buildFont } from "./src/truetype/font.ts";
import { buildContainer } from "./src/truetype/container.ts";

// Writes testAssets/fonts/fixture.mxf: an authored TrueType font inside the
// game's own container format.
//
// We ship no game data, so the font reader cannot be checked against a font
// off a disc. This writes one from nothing, covering every branch in the
// outline reader (see src/truetype/font.ts for which glyph covers what).
// The rest of the file — the character map, the metrics, the location table —
// is ordinary and correct because the reader has to walk it to reach any of
// that.
//
// Deterministic: run it twice and get the same bytes. Nothing here reads a
// clock.
function main(): void {
    const root = dirname(dirname(fileURLToPath(import.meta.url)));
    const directory = join(root, "testAssets", "fonts");
    mkdirSync(directory, { recursive: true });
    const path = join(directory, "fixture.mxf");

    const data = buildContainer(buildFont());
    writeFileSync(path, data);
    console.log(`wrote ${path}, ${data.length} bytes`);
}

main();
