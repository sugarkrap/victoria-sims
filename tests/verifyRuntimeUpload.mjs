// Runs the page's own uploadMesh against a device that records what it was
// asked to do.
//
// The module test next door stubs uploadMesh out, so the real one had no
// coverage at all — and a mesh whose index count made an odd number of words
// went straight to the browser and threw there. WebGPU wants both the buffer
// size and the size of every write to be a multiple of four bytes. The teapot's
// 18960 indices are 37920 bytes, which is a multiple of four by luck, so a
// version that rounded the buffer but not the write passed everything here and
// failed on the first retail mesh: 737 triangles, 2211 indices, 4422 bytes.
//
// The real file is loaded rather than reimplemented. A copy of the logic would
// have agreed with itself and told us nothing.

import { readFileSync } from "node:fs";
import { createContext, runInContext } from "node:vm";

let failureCount = 0;

function check(description, condition) {
    console.log(`${condition ? "ok  " : "FAIL"}  ${description}`);
    if (!condition) {
        failureCount += 1;
    }
}

const writes = [];
const buffers = [];

// Enough of a browser for the file to load, and no more. Anything it reaches
// for that is not here is something this test should be told about.
function makeRecordingDevice() {
    return {
        createBuffer(descriptor) {
            const buffer = { size: descriptor.size, usage: descriptor.usage };
            buffers.push(buffer);
            return buffer;
        },
        createBindGroup: () => ({}),
        queue: {
            writeBuffer(buffer, offset, source) {
                writes.push({ buffer, offset, byteLength: source.byteLength, bytes: Uint8Array.from(source) });
            }
        }
    };
}

const context = createContext({
    console: { log: () => {}, error: () => {} },
    document: {
        readyState: "complete",
        getElementById: () => null,
        addEventListener: () => {}
    },
    navigator: {},
    window: {},
    requestAnimationFrame: () => 0,
    performance: { now: () => 0 },
    TextDecoder,
    Uint8Array,
    Uint16Array,
    Float32Array,
    GPUBufferUsage: { VERTEX: 1, INDEX: 2, UNIFORM: 4, COPY_DST: 8 },
    GPUTextureUsage: { RENDER_ATTACHMENT: 16 }
});

// A trailing expression, because the file's top level declarations are const and
// so live in the script's own scope rather than on the context object. The
// completion value is the only way out that does not involve editing the file
// for the benefit of its test.
const runtime = runInContext(
    `${readFileSync("platform/web/victoriaRuntime.js", "utf8")}\n;({ importObject, runtimeState });`,
    context, { filename: "victoriaRuntime.js" });

const uploadMesh = runtime.importObject.victoriaRender.uploadMesh;
check("the page exposes uploadMesh to the module", typeof uploadMesh === "function");
if (typeof uploadMesh !== "function") {
    process.exit(1);
}

// A mesh laid out the way the engine lays one out: vertices first, then indices.
const VERTEX_COUNT = 521;
const INDEX_COUNT = 2211; // 737 triangles, as read off a retail face
const VERTEX_POINTER = 1024;
const INDEX_POINTER = VERTEX_POINTER + VERTEX_COUNT * 24;

const linearMemory = new ArrayBuffer(INDEX_POINTER + INDEX_COUNT * 2 + 2);
const indexView = new Uint16Array(linearMemory, INDEX_POINTER, INDEX_COUNT);
for (let index = 0; index < INDEX_COUNT; index += 1) {
    indexView[index] = index % VERTEX_COUNT;
}
// The two bytes past the end are somebody else's. Filled with a value that could
// not come from the indices, so reading them long is visible rather than
// plausible.
new Uint8Array(linearMemory).fill(0xEE, INDEX_POINTER + INDEX_COUNT * 2);

runtime.runtimeState.instance = { exports: { memory: { buffer: linearMemory } } };
runtime.runtimeState.device = makeRecordingDevice();

console.log("-- a mesh whose indices are not a whole number of words --");
check("uploads it", uploadMesh(VERTEX_POINTER, VERTEX_COUNT, INDEX_POINTER, INDEX_COUNT) === 1);
check("the index count is genuinely odd for this test", (INDEX_COUNT * 2) % 4 !== 0);

for (const write of writes) {
    check(`writes ${write.byteLength} bytes, a multiple of four`, write.byteLength % 4 === 0);
}
for (const buffer of buffers) {
    check(`allocates ${buffer.size} bytes, a multiple of four`, buffer.size % 4 === 0);
}

console.log("\n-- the padding is padding, not data --");
{
    const indexWrite = writes.find((write) => write.byteLength === 4424);
    check("the index write is the padded length", indexWrite !== undefined);
    if (indexWrite) {
        // Every real index survives the copy...
        let intact = true;
        for (let index = 0; index < INDEX_COUNT; index += 1) {
            const low = indexWrite.bytes[index * 2];
            const high = indexWrite.bytes[index * 2 + 1];
            if ((low | (high << 8)) !== index % VERTEX_COUNT) {
                intact = false;
            }
        }
        check("every index arrives unchanged", intact);
        // ...and the two bytes added are this engine's zeroes, not whatever
        // happened to follow the mesh in the arena.
        check("the padding is zero rather than the neighbour's bytes",
              indexWrite.bytes[4422] === 0 && indexWrite.bytes[4423] === 0);
    }
}

console.log("\n-- and an even one is left alone --");
{
    writes.length = 0;
    buffers.length = 0;
    uploadMesh(VERTEX_POINTER, VERTEX_COUNT, INDEX_POINTER, 2210);
    const indexWrite = writes.find((write) => write.byteLength === 4420);
    check("a mesh already on a word boundary is not padded", indexWrite !== undefined);
}

if (failureCount === 0) {
    console.log("\nall runtime upload checks passed");
    process.exit(0);
}
console.log(`\n${failureCount} runtime upload check(s) failed`);
process.exit(1);
