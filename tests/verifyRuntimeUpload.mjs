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
const textures = [];
const textureWrites = [];

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
        createSampler: () => ({ kind: "sampler" }),
        createShaderModule: () => ({ kind: "shader" }),
        createRenderPipeline: (descriptor) => ({
            descriptor,
            getBindGroupLayout: () => ({ kind: "layout" })
        }),
        createTexture(descriptor) {
            const texture = { ...descriptor, createView: () => ({ kind: "view" }) };
            textures.push(texture);
            return texture;
        },
        queue: {
            writeBuffer(buffer, offset, source) {
                writes.push({ buffer, offset, byteLength: source.byteLength, bytes: Uint8Array.from(source) });
            },
            writeTexture(destination, source, layout, size) {
                textureWrites.push({
                    bytesPerRow: layout.bytesPerRow,
                    rowsPerImage: layout.rowsPerImage,
                    size,
                    byteLength: source.byteLength,
                    bytes: Uint8Array.from(source)
                });
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
    GPUTextureUsage: { RENDER_ATTACHMENT: 16, TEXTURE_BINDING: 32, COPY_DST: 64 }
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

console.log("\n-- uploading a texture whose rows are not a multiple of 256 --");
{
    // WebGPU requires each row of a texture write to begin on a 256 byte
    // boundary. A 3 pixel wide image is 12 bytes a row, so the write has to be
    // staged into padded rows rather than passed through — and passing the
    // module's memory straight in would be accepted by the type system and
    // rejected by the browser.
    const createMeshPipeline = runtime.importObject.victoriaRender.createMeshPipeline;
    const uploadTexture = runtime.importObject.victoriaRender.uploadTexture;

    check("the page exposes uploadTexture", typeof uploadTexture === "function");

    const shaderMemory = new ArrayBuffer(4096);
    runtime.runtimeState.instance = { exports: { memory: { buffer: shaderMemory } } };
    // readUTF8 reads through runtimeState.memory rather than the instance, so
    // the shader source the pipeline is built from comes from here.
    runtime.runtimeState.memory = { buffer: shaderMemory };
    runtime.runtimeState.device = makeRecordingDevice();
    runtime.runtimeState.canvasFormat = "bgra8unorm";
    runtime.runtimeState.meshTexture = null;

    check("the mesh pipeline builds", createMeshPipeline(0, 0) === 1);
    // Built before any image arrives, so the bind group is complete and a mesh
    // with no texture is lit exactly as it was before textures existed.
    check("with a single white pixel standing in for a texture",
          textures.length === 1 && textures[0].size.width === 1 && textures[0].size.height === 1);
    check("and the vertex layout carries a texture coordinate",
          runtime.runtimeState.meshPipeline.descriptor.vertex.buffers[0].arrayStride === 32);
    check("as a third attribute",
          runtime.runtimeState.meshPipeline.descriptor.vertex.buffers[0].attributes.length === 3);

    // Three pixels across, four rows: rows of 12 bytes padded to 256.
    const PIXELS = 3 * 4;
    const pixelMemory = new ArrayBuffer(1024);
    new Uint8Array(pixelMemory).fill(0x7F, 0, PIXELS * 4);
    runtime.runtimeState.instance = { exports: { memory: { buffer: pixelMemory } } };
    runtime.runtimeState.memory = { buffer: pixelMemory };

    textureWrites.length = 0;
    check("the texture uploads", uploadTexture(0, 3, 4) === 1);
    const write = textureWrites[0];
    check("a row starts on a 256 byte boundary", write && write.bytesPerRow % 256 === 0);
    check("and the staging buffer is a whole number of those rows",
          write && write.byteLength === write.bytesPerRow * 4);
    // The first twelve bytes of each row are the image; the rest is padding
    // that the write's own layout tells the device to skip.
    check("each row begins with its own pixels",
          write && write.bytes[0] === 0x7F && write.bytes[11] === 0x7F &&
          write.bytes[write.bytesPerRow] === 0x7F);
    check("and the padding after them is not image data",
          write && write.bytes[12] === 0);

    check("a texture reaching past the end of memory is refused",
          uploadTexture(1000, 64, 64) === 0);
}

console.log("\n-- uploading a texture before there is a mesh to put it on --");
{
    // An image binds to the layout the mesh's pipeline defines, so there is
    // nothing to bind it to until a mesh has been uploaded. The engine sets the
    // mesh first; this is what happens if it ever does not, and for as long as
    // no disc yielded a texture the answer was an exception from a null
    // pipeline rather than a refusal.
    const uploadTexture = runtime.importObject.victoriaRender.uploadTexture;
    const pixelMemory = new ArrayBuffer(1024);

    runtime.runtimeState.instance = { exports: { memory: { buffer: pixelMemory } } };
    runtime.runtimeState.memory = { buffer: pixelMemory };
    runtime.runtimeState.device = makeRecordingDevice();
    runtime.runtimeState.meshPipeline = null;
    runtime.runtimeState.meshTexture = null;

    let threw = false;
    let answer = 1;
    try {
        answer = uploadTexture(0, 2, 2);
    } catch (error) {
        threw = true;
    }
    check("it does not throw", !threw);
    check("and says it could not", answer === 0);
}

if (failureCount === 0) {
    console.log("\nall runtime upload checks passed");
    process.exit(0);
}
console.log(`\n${failureCount} runtime upload check(s) failed`);
process.exit(1);
