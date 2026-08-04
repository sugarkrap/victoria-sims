// Instantiates the freestanding wasm module against stub host functions and
// checks that the C engine drives the renderer the way the host page expects.
// Catches a broken import/export contract without needing a browser or a GPU.

import { readFileSync } from "node:fs";

const modulePath = "build/web/victoriaSims.wasm";
const calls = [];
let memory = null;
let failureCount = 0;

function decodeUtf8(pointer, length) {
    return new TextDecoder().decode(new Uint8Array(memory.buffer, pointer, length));
}

function check(description, condition) {
    console.log(`${condition ? "ok  " : "FAIL"}  ${description}`);
    if (!condition) {
        failureCount += 1;
    }
}

const imports = {
    victoriaPlatform: {
        logMessage: (pointer, length) => console.log(`      engine: ${decodeUtf8(pointer, length)}`)
    },
    victoriaRender: {
        configureSurface: (width, height) => calls.push({ name: "configureSurface", width, height }),
        createTrianglePipeline: (pointer, length) => {
            const shaderSource = decodeUtf8(pointer, length);
            calls.push({ name: "createTrianglePipeline", shaderSource });
            return 1;
        },
        setClearColor: (red, green, blue) => calls.push({ name: "setClearColor", red, green, blue }),
        setTriangleTint: (tint) => calls.push({ name: "setTriangleTint", tint }),
        submitFrame: () => calls.push({ name: "submitFrame" })
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
    "victoriaWebGetBudgetUsedBytes"
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
for (let frameIndex = 0; frameIndex < frameCount; frameIndex += 1) {
    instance.exports.victoriaWebRenderFrame(frameIndex / 60);
}

check("submits one frame per render call",
      calls.filter((call) => call.name === "submitFrame").length === frameCount);

const tints = calls.filter((call) => call.name === "setTriangleTint").map((call) => call.tint);
check("tint stays inside its intended range", tints.every((tint) => tint >= 0.29 && tint <= 1.01));
check("tint actually animates", new Set(tints.map((tint) => tint.toFixed(4))).size > 1);

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
