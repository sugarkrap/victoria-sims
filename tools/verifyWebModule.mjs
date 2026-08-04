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

const imports = {
    victoriaPlatform: {
        logMessage: (pointer, length) => console.log(`      engine: ${decodeUTF8(pointer, length)}`),
        getMilliseconds: () => simulatedMilliseconds
    },
    victoriaRender: {
        configureSurface: (width, height) => calls.push({ name: "configureSurface", width, height }),
        createTrianglePipeline: (pointer, length) => {
            const shaderSource = decodeUTF8(pointer, length);
            calls.push({ name: "createTrianglePipeline", shaderSource });
            return 1;
        },
        setClearColor: (red, green, blue) => calls.push({ name: "setClearColor", red, green, blue }),
        setTriangleTint: (tint) => calls.push({ name: "setTriangleTint", tint }),
        submitFrame: () => {
            calls.push({ name: "submitFrame" });
            // Advancing the clock mid-frame is what the engine measures: it
            // reads the clock at frame start and again at frame end.
            simulatedMilliseconds += frameCostMilliseconds;
        }
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
    "victoriaWebGetFrameIntervalMicroseconds"
];
for (const exportName of requiredExports) {
    check(`exports ${exportName}`, exportName in instance.exports);
}

const initializeResult = instance.exports.victoriaWebInitialize(1024, 576);
check("initializes successfully", initializeResult === 1);

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
const normalFrameMilliseconds = 4;
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

const lastMicroseconds = instance.exports.victoriaWebGetFrameMicroseconds();
const worstMicroseconds = instance.exports.victoriaWebGetWorstFrameMicroseconds();
const averageMicroseconds = instance.exports.victoriaWebGetAverageFrameMicroseconds();
const expectedAverage =
    ((frameCount - 1) * normalFrameMilliseconds + spikeFrameMilliseconds) * 1000 / frameCount;

check(`profiler times the last frame at ${normalFrameMilliseconds} ms`,
      lastMicroseconds === normalFrameMilliseconds * 1000);
check(`profiler catches the ${spikeFrameMilliseconds} ms spike as the worst frame`,
      worstMicroseconds === spikeFrameMilliseconds * 1000);
check(`profiler averages to ${expectedAverage} us`, averageMicroseconds === expectedAverage);

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

console.log("\n--- profiler report ---");
console.log(reportText.trimEnd());
console.log("--- end report ---\n");

calls.length = 0;
instance.exports.victoriaWebResize(800, 600);
const resizeCall = calls.find((call) => call.name === "configureSurface");
check("reconfigures the surface on resize",
      Boolean(resizeCall) && resizeCall.width === 800 && resizeCall.height === 600);

instance.exports.victoriaWebShutdown();

if (failureCount > 0) {
    console.error(`\n${failureCount} check(s) failed`);
    process.exit(1);
}
console.log("\nall web module checks passed");
