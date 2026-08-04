// Instantiates the freestanding wasm module against stub host functions and
// checks that the C engine drives the renderer the way the host page expects.
// Catches a broken import/export contract without needing a browser or a GPU.

import { readFileSync } from "node:fs";

const modulePath = "build/web/victoriaSims.wasm";
const calls = [];
let memory = null;
let failureCount = 0;

function decodeUTF8(pointer, length) {
    return new TextDecoder().decode(new Uint8Array(memory.buffer, pointer, length));
}

function check(description, condition) {
    console.log(`${condition ? "ok  " : "FAIL"}  ${description}`);
    if (!condition) {
        failureCount += 1;
    }
}

// A clock the test drives itself, so timing assertions are exact rather than
// dependent on how fast this machine happens to be.
let simulatedMilliseconds = 0;
let frameCostMilliseconds = 0;
// What the "adapter" claims it has, so detection can be checked against a
// number chosen here rather than whatever the test machine happens to report.
const reportedGraphicsKibibytes = 32 * 1024;
const shaderCompileMilliseconds = 50;

// Kept as well as printed, so a line the engine is supposed to emit can be
// asserted on rather than looked for by eye.
const loggedMessages = [];

const imports = {
    victoriaPlatform: {
        logMessage: (pointer, length) => {
            const text = decodeUTF8(pointer, length);
            loggedMessages.push(text);
            console.log(`      engine: ${text}`);
        },
        getMilliseconds: () => simulatedMilliseconds
    },
    victoriaRender: {
        configureSurface: (width, height) => calls.push({ name: "configureSurface", width, height }),
        createTrianglePipeline: (pointer, length) => {
            const shaderSource = decodeUTF8(pointer, length);
            calls.push({ name: "createTrianglePipeline", shaderSource });
            // Stands in for a slow driver compile, so the report can be checked
            // for whether that cost is actually attributed to the right zone.
            simulatedMilliseconds += shaderCompileMilliseconds;
            return 1;
        },
        setClearColor: (red, green, blue) => calls.push({ name: "setClearColor", red, green, blue }),
        setTriangleTint: (tint) => calls.push({ name: "setTriangleTint", tint }),
        // Mesh drawing. Nothing in this file gives the engine a disc, so these
        // stand ready and stay unused — which is itself worth asserting, since
        // a build with no disc must still draw its triangle.
        createMeshPipeline: (pointer, length) => {
            calls.push({ name: "createMeshPipeline", shaderSource: decodeUTF8(pointer, length) });
            return 1;
        },
        uploadMesh: (vertexPointer, vertexCount, indexPointer, indexCount) => {
            calls.push({ name: "uploadMesh", vertexCount, indexCount });
            return 1;
        },
        uploadTexture: (pixelPointer, width, height) => {
            calls.push({ name: "uploadTexture", width, height });
            return 1;
        },
        setMeshUniforms: () => calls.push({ name: "setMeshUniforms" }),
        submitFrame: () => {
            calls.push({ name: "submitFrame" });
            // Advancing the clock mid-frame is what the engine measures: it
            // reads the clock at frame start and again at frame end.
            simulatedMilliseconds += frameCostMilliseconds;
        },
        queryGraphicsMemoryKibibytes: () => {
            calls.push({ name: "queryGraphicsMemoryKibibytes" });
            return reportedGraphicsKibibytes;
        },
        warmUpPipeline: () => calls.push({ name: "warmUpPipeline" })
    }
};

const { instance } = await WebAssembly.instantiate(readFileSync(modulePath), imports);
memory = instance.exports.memory;

const requiredExports = [
    "memory",
    "victoriaWebInitialize",
    "victoriaWebResize",
    "victoriaWebRenderFrame",
    "victoriaWebShutdown",
    "victoriaWebGetBudgetTotalBytes",
    "victoriaWebGetBudgetUsedBytes",
    "victoriaWebGetProfilerReportPointer",
    "victoriaWebGetProfilerReportLength",
    "victoriaWebGetFrameMicroseconds",
    "victoriaWebGetAverageFrameMicroseconds",
    "victoriaWebGetWorstFrameMicroseconds",
    "victoriaWebGetFrameIntervalMicroseconds",
    "victoriaWebGetGraphicsMemoryLimitBytes",
    "victoriaWebGetGraphicsMemoryUsedBytes"
];
for (const exportName of requiredExports) {
    check(`exports ${exportName}`, exportName in instance.exports);
}

// Zero asks the engine to detect rather than override.
const initializeResult = instance.exports.victoriaWebInitialize(1024, 576, 0);
check("initializes successfully", initializeResult === 1);

check("adopts the graphics memory size the backend reported",
      instance.exports.victoriaWebGetGraphicsMemoryLimitBytes() === reportedGraphicsKibibytes * 1024);
check("charges the uniform buffer against the graphics ceiling",
      instance.exports.victoriaWebGetGraphicsMemoryUsedBytes() === 16);
check("warms the pipeline up during initialisation",
      calls.some((call) => call.name === "warmUpPipeline"));
check("warm-up happens after pipeline creation",
      calls.findIndex((call) => call.name === "warmUpPipeline") >
      calls.findIndex((call) => call.name === "createTrianglePipeline"));

const budgetBytes = instance.exports.victoriaWebGetBudgetTotalBytes();
check("reserves a 128 MiB arena", budgetBytes === 128 * 1024 * 1024);
check("linear memory covers the arena", memory.buffer.byteLength >= budgetBytes);

const shaderCall = calls.find((call) => call.name === "createTrianglePipeline");
check("creates a pipeline", Boolean(shaderCall));
check("passes valid WGSL",
      Boolean(shaderCall) &&
      shaderCall.shaderSource.includes("@vertex") &&
      shaderCall.shaderSource.includes("@fragment") &&
      shaderCall.shaderSource.includes("fn vertexMain") &&
      shaderCall.shaderSource.includes("fn fragmentMain"));

calls.length = 0;
const frameCount = 32;
// Each frame is made to take a known 4 ms, with one deliberate 20 ms spike, so
// the profiler's last/average/worst can be checked against exact numbers.
const normalFrameMilliseconds = 8;
const spikeFrameMilliseconds = 20;
const spikeFrameIndex = 10;

for (let frameIndex = 0; frameIndex < frameCount; frameIndex += 1) {
    frameCostMilliseconds = frameIndex === spikeFrameIndex ? spikeFrameMilliseconds : normalFrameMilliseconds;
    instance.exports.victoriaWebRenderFrame(frameIndex / 60);
}

check("submits one frame per render call",
      calls.filter((call) => call.name === "submitFrame").length === frameCount);

const tints = calls.filter((call) => call.name === "setTriangleTint").map((call) => call.tint);
check("tint stays inside its intended range", tints.every((tint) => tint >= 0.29 && tint <= 1.01));
check("tint actually animates", new Set(tints.map((tint) => tint.toFixed(4))).size > 1);

// A build that was never given a disc must still draw. The mesh imports exist
// from the moment the module is linked, and a backend that reached for them
// unprompted would take the page from "no disc yet" to a blank canvas.
check("draws the triangle when no disc was given",
      calls.some((call) => call.name === "setTriangleTint"));
check("and does not build a mesh pipeline it was never given a mesh for",
      !calls.some((call) => call.name === "createMeshPipeline"));
check("nor uploads one", !calls.some((call) => call.name === "uploadMesh"));

const lastMicroseconds = instance.exports.victoriaWebGetFrameMicroseconds();
const worstMicroseconds = instance.exports.victoriaWebGetWorstFrameMicroseconds();
const averageMicroseconds = instance.exports.victoriaWebGetAverageFrameMicroseconds();
// Initialisation is bracketed as a profiler frame of its own, so shader
// compilation counts as frame one and participates in these statistics.
const measuredFrameCount = frameCount + 1;
const totalMilliseconds =
    shaderCompileMilliseconds + (frameCount - 1) * normalFrameMilliseconds + spikeFrameMilliseconds;
const expectedAverage = Math.floor((totalMilliseconds * 1000) / measuredFrameCount);
const expectedWorst = Math.max(shaderCompileMilliseconds, spikeFrameMilliseconds) * 1000;

check(`profiler times the last frame at ${normalFrameMilliseconds} ms`,
      lastMicroseconds === normalFrameMilliseconds * 1000);
check(`profiler reports the ${expectedWorst / 1000} ms startup frame as the worst`,
      worstMicroseconds === expectedWorst);
check(`profiler averages to ${expectedAverage} us over ${measuredFrameCount} frames`,
      averageMicroseconds === expectedAverage);

const reportPointer = instance.exports.victoriaWebGetProfilerReportPointer();
const reportLength = instance.exports.victoriaWebGetProfilerReportLength();
const reportText = decodeUTF8(reportPointer, reportLength);

check("profiler report is non-empty", reportLength > 0);
check("report names the frame counter", reportText.includes("frame "));
check("report separates work from interval", reportText.includes("work ") && reportText.includes("interval "));
check("report includes the memory budget", reportText.includes("128 MiB budget"));
check("report lists the renderDrawFrame zone", reportText.includes("renderDrawFrame"));
check("report lists the engineRenderFrame zone", reportText.includes("engineRenderFrame"));
check("report warns about no overflows", !reportText.includes("overflow"));
check("report includes the graphics memory ledger",
      reportText.includes("graphics memory") && reportText.includes("(detected)"));
check("report attributes shader compilation",
      reportText.includes("renderCompileShaders"));
// Startup is its own profiler frame, so by steady state the compile shows as
// the zone's worst rather than its most recent value.
const compileLine = reportText.split("\n").find((line) => line.includes("renderCompileShaders")) ?? "";
check(`report attributes the ${shaderCompileMilliseconds} ms compile to renderCompileShaders`,
      compileLine.includes(`${shaderCompileMilliseconds}.000`));
check("report shows a non-zero frame interval once frames are flowing",
      /interval\s+\d+\.\d{3} ms\s+average\s+\d+\.\d{3} ms/.test(reportText) &&
      !/interval 0\.000 ms\s+average 0\.000 ms/.test(reportText));

console.log("\n--- profiler report ---");
console.log(reportText.trimEnd());
console.log("--- end report ---\n");

calls.length = 0;
instance.exports.victoriaWebResize(800, 600);
const resizeCall = calls.find((call) => call.name === "configureSurface");
check("reconfigures the surface on resize",
      Boolean(resizeCall) && resizeCall.width === 800 && resizeCall.height === 600);

instance.exports.victoriaWebShutdown();

// A ceiling that never refuses anything is decoration. Instantiated fresh so
// the budget starts clean, then given less room than the renderer needs.
const refusalCalls = [];
const refusalImports = {
    victoriaPlatform: {
        logMessage: (pointer, length) => refusalCalls.push({ name: "logMessage" }),
        getMilliseconds: () => 0
    },
    victoriaRender: {
        configureSurface: () => refusalCalls.push({ name: "configureSurface" }),
        createTrianglePipeline: () => { refusalCalls.push({ name: "createTrianglePipeline" }); return 1; },
        setClearColor: () => {},
        setTriangleTint: () => {},
        createMeshPipeline: () => 1,
        uploadMesh: () => 1,
        uploadTexture: () => 1,
        setMeshUniforms: () => {},
        submitFrame: () => {},
        queryGraphicsMemoryKibibytes: () => reportedGraphicsKibibytes,
        warmUpPipeline: () => refusalCalls.push({ name: "warmUpPipeline" })
    }
};
const refusalModule = await WebAssembly.instantiate(readFileSync(modulePath), refusalImports);
// 8 bytes, against a 16-byte uniform buffer.
const refusedInitialize = refusalModule.instance.exports.victoriaWebInitialize(1024, 576, 8);

check("initialisation fails when the graphics ceiling is too low", refusedInitialize === 0);
check("nothing is charged after a refusal",
      refusalModule.instance.exports.victoriaWebGetGraphicsMemoryUsedBytes() === 0);
check("no pipeline is built once the ceiling has refused",
      !refusalCalls.some((call) => call.name === "createTrianglePipeline"));


// ---------------------------------------------------------------------------
// Reading a disc the way the page does.
//
// The browser cannot answer a read on the spot, so the module asks for a range,
// says PENDING, and waits to be asked again. That protocol is the whole reason
// the engine's reads have a PENDING at all, and it is not exercised by anything
// else here — a build with no disc never reaches it.
//
// Driving it from Node rather than a browser is deliberate. What is being
// tested is the handshake, not WebGPU, and a headless run can assert on every
// step of it.
// ---------------------------------------------------------------------------
calls.length = 0;
{
    const image = readFileSync("testAssets/discs/testDisc.iso");
    const opened = instance.exports.victoriaWebOpenDisc(image.length);
    let status = 0;
    let steps = 0;
    let rangesFetched = 0;
    let bytesFetched = 0;

    check("opens a disc", opened === 1);

    for (steps = 0; steps < 200000; steps += 1) {
        status = instance.exports.victoriaWebStepDiscLoad();
        if (status !== 1) {
            break;
        }
        const length = instance.exports.victoriaWebGetWantedLength();
        if (length > 0) {
            const offset = instance.exports.victoriaWebGetWantedOffset();
            const pointer = instance.exports.victoriaWebGetDeliveryPointer();
            new Uint8Array(instance.exports.memory.buffer, pointer, length)
                .set(image.subarray(offset, offset + length));
            instance.exports.victoriaWebDeliver();
            rangesFetched += 1;
            bytesFetched += length;
        }
    }

    check("reaches a drawable state", status === 2);
    check("asked the page for ranges rather than the image", rangesFetched > 0);
    check(`read less than the image (${bytesFetched} of ${image.length} bytes)`,
          bytesFetched < image.length);
    check("built a mesh pipeline once it had geometry",
          calls.some((call) => call.name === "createMeshPipeline"));

    const upload = calls.find((call) => call.name === "uploadMesh");
    check("uploaded the teapot", Boolean(upload));
    if (upload) {
        // The same numbers the native geometry test asserts, arrived at through
        // wasm, the disc reader and the PENDING handshake.
        check("with 13248 vertices", upload.vertexCount === 13248);
        check("and 18960 indices", upload.indexCount === 18960);
    }
    // What the disc holds besides packages, and what those files actually are.
    // The fixture carries a file named like an installer cabinet that is not
    // one, so the probe has something to be wrong about: it reports what the
    // bytes say, not what the extension claims.
    check("counts packages against everything else",
          loggedMessages.some((text) =>
              text.includes("5 package(s)") && text.includes("3 other file(s)")));
    check("names the largest non-package file first",
          loggedMessages.some((text) => text.includes("512 bytes  TSData.exe")));

    // The fixture's TSData.exe carries the first sixty-four bytes of an Inno
    // Setup installer, copied from the shape a real repack has: a Delphi MZP
    // stub, and the loader mark at 0x30 that says what the program really is.
    // Reading only the front would call this "a Windows program", which is what
    // every installer on every disc looks like and tells nobody anything.
    const installer = loggedMessages.find((text) => text.includes("TSData.exe —"));
    check("probes the installer for a signature", Boolean(installer));
    if (installer) {
        check("naming Delphi's stub, which is what marks it out",
              installer.includes("Delphi-built"));
        // Printed whether or not they mean anything to the reader: on the disc
        // this grew for, 0x30 being zeros is what said the table was elsewhere.
        check("and printing the front and 0x30 both",
              installer.includes("4D 5A 50 00") && installer.includes("and at 0x30"));
    }

    // Having named it, the load goes on to open it: the table's own checksum
    // establishes the field layout, and its last two offsets are what anything
    // reading further would need. Nothing is decompressed — this is navigation,
    // and a reader that claimed to open an archive before it could find its way
    // around one would be claiming nothing.
    // Not at 0x30 — the real installer keeps zeros there, so the fixture does
    // too, and the table has to be found by looking for it.
    check("says the table is not where the old loaders keep it",
          loggedMessages.some((text) => text.includes("keeps no table at 0x30")));
    check("and finds it by searching",
          loggedMessages.some((text) => text.includes("found an offset table at 0x00000140")));
    const offsetTable = loggedMessages.find((text) => text.includes("table revision"));
    check("reads it", Boolean(offsetTable));
    if (offsetTable) {
        // Six fields, discovered rather than assumed, and the two offsets taken
        // off the end where every layout keeps them.
        check("working out the layout from the checksum", offsetTable.includes("6 fields"));
        check("with the offsets it ends with",
              offsetTable.includes("header at 0x00000100") && offsetTable.includes("data at 0x00000180"));
        check("and the size it claims matching the file",
              offsetTable.includes("512 bytes of 512 bytes"));
    }
    check("and reads the version that built it",
          loggedMessages.some((text) => text.includes("Inno Setup Setup Data (5.5.0) (u)")));

    const probe = loggedMessages.find((text) => text.includes("data1.cab —"));
    check("probes a file it cannot name", Boolean(probe));
    if (probe) {
        // "placehol", which is neither a cabinet nor anything else known.
        check("reporting the bytes it read", probe.includes("70 6C 61 63 65 68 6F 6C"));
        check("and refusing to name a format it does not recognise",
              probe.includes("unrecognised"));
    }

    console.log(`  ${steps} steps, ${rangesFetched} ranges, ${bytesFetched} bytes`);
}

if (failureCount > 0) {
    console.error(`\n${failureCount} check(s) failed`);
    process.exit(1);
}
console.log("\nall web module checks passed");
