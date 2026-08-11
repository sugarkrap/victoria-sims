import { createHash } from "node:crypto";
import { readFileSync, statSync } from "node:fs";
import { CheckReport, run, runMain } from "./lib/checkReport.ts";

// Keeps retail game data out of the repository.
//
// The rule is not "no binary data files" — that was too blunt, and it cost
// this project a set of perfectly clean, purpose-built test fixtures. The
// rule is that data of the kind a retail install produces may only live in
// testAssets/, and every file there is pinned by hash. So:
//
//   1. No game-data-shaped file tracked anywhere outside testAssets/.
//   2. Every manifest entry is tracked by git and matches its recorded hash.
//   3. Nothing is tracked under testAssets/ that the manifest does not list.
//
// Rules 2 and 3 both consult git rather than the filesystem, deliberately.
// An earlier version hashed whatever was on disk while listing what git knew
// about, and so cheerfully validated four fixtures that .gitignore had
// silently excluded from the commit.

const DATA_EXTENSIONS = /\.(package|dds|tga|xa|far|iff|bmp|jpg|jpeg|mp3|wav|iso|img|bin|cue|nrg|mdf)$/i;
const FIXTURE_DIRECTORY = "testAssets";
const MANIFEST = `${FIXTURE_DIRECTORY}/manifest.sha256`;

function gitFiles(pathspec?: string): string[] {
    const args = pathspec ? ["ls-files", pathspec] : ["ls-files"];
    return run("git", args).split("\n").filter(Boolean);
}

function checkForStrayGameData(report: CheckReport): void {
    console.log(`checking for game data outside ${FIXTURE_DIRECTORY}/...`);
    const strays = gitFiles().filter(
        (path) => DATA_EXTENSIONS.test(path) && !path.startsWith(`${FIXTURE_DIRECTORY}/`),
    );
    if (strays.length > 0) {
        strays.forEach((path) => console.error(path));
        report.fail(`game data must live under ${FIXTURE_DIRECTORY}/ and be listed in the manifest`);
    } else {
        report.ok("none");
    }
}

interface ManifestEntry {
    hash: string;
    path: string;
}

function parseManifest(): ManifestEntry[] {
    const text = readFileSync(MANIFEST, "utf8");
    return text
        .split("\n")
        .filter(Boolean)
        .map((line): ManifestEntry => {
            const match = line.match(/^(\S+)\s+\*?(.+)$/);
            if (match === null) {
                throw new Error(`malformed manifest line: ${line}`);
            }
            const [, hash, path] = match;
            return { hash: hash!, path: `${FIXTURE_DIRECTORY}/${path}` };
        });
}

function main(): void {
    const report = new CheckReport();
    checkForStrayGameData(report);

    try {
        statSync(MANIFEST);
    } catch {
        console.error(`error: missing ${MANIFEST}`);
        process.exitCode = 1;
        return;
    }

    const manifest = parseManifest();
    const listed = new Set(manifest.map((entry) => entry.path));
    const tracked = new Set(
        gitFiles(FIXTURE_DIRECTORY).filter(
            (path) => path !== `${FIXTURE_DIRECTORY}/manifest.sha256` && path !== `${FIXTURE_DIRECTORY}/README.md`,
        ),
    );

    console.log("checking every manifest entry is committed...");
    const missing = [...listed].filter((path) => !tracked.has(path)).sort();
    if (missing.length === 0) {
        report.ok("all listed fixtures are tracked");
    } else {
        missing.forEach((path) => console.error(path));
        report.fail("manifest lists a file git is not tracking (is it caught by .gitignore?)");
    }

    console.log(`checking for unlisted files in ${FIXTURE_DIRECTORY}/...`);
    const unlisted = [...tracked].filter((path) => !listed.has(path)).sort();
    if (unlisted.length === 0) {
        report.ok("none");
    } else {
        unlisted.forEach((path) => console.error(path));
        report.fail(`file tracked under ${FIXTURE_DIRECTORY}/ but absent from the manifest`);
    }

    console.log(`verifying fixture contents against ${MANIFEST}...`);
    const mismatches = manifest.filter(({ hash, path }) => {
        let actual: string;
        try {
            actual = createHash("sha256").update(readFileSync(path)).digest("hex");
        } catch {
            return true;
        }
        return actual !== hash;
    });
    if (mismatches.length === 0) {
        report.ok("all fixtures match");
    } else {
        mismatches.forEach(({ path }) => console.error(path));
        report.fail("a fixture does not match its recorded hash");
    }

    report.finish("game data");
}

runMain(main);
