import { execFileSync, spawnSync, type ExecFileSyncOptions } from "node:child_process";

export class CheckReport {
    failureCount = 0;

    fail(message: string): void {
        console.error(`error: ${message}`);
        this.failureCount += 1;
    }

    ok(message: string): void {
        console.log(`  ${message}`);
    }

    finish(name: string): void {
        if (this.failureCount !== 0) {
            console.error(`${name} check FAILED`);
            process.exitCode = 1;
            return;
        }
        console.log(`${name} check passed`);
    }
}

export function commandExists(command: string): boolean {
    const result = spawnSync(command, ["--version"], { stdio: "ignore" });
    return result.error === undefined;
}

// A compiler or symbol reader failing is not a bug in the check — it is the
// thing the check is reporting on. Let its own error message reach the
// terminal directly, then stop, rather than burying it under a stack trace.
export function run(command: string, args: string[], options: ExecFileSyncOptions = {}): string {
    return execFileSync(command, args, {
        stdio: ["ignore", "pipe", "inherit"],
        encoding: "utf8",
        ...options,
    }) as string;
}

// Runs `main`, and on failure prints just the message rather than a stack
// trace — this is a command line check, not a program with a bug to debug.
export function runMain(main: () => void): void {
    try {
        main();
    } catch (error) {
        const message = error instanceof Error ? error.message : String(error);
        console.error(`error: ${message}`);
        process.exitCode = 1;
    }
}
