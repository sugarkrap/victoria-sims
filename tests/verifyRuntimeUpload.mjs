
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
const destroyed = [];

function makeRecordingDevice() {
    return {
        createBuffer(descriptor) {
            const buffer = { size: descriptor.size, usage: descriptor.usage };
            buffer.destroy = () => destroyed.push(buffer);
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
            texture.destroy = () => destroyed.push(texture);
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
    window: {
        addEventListener: () => {},
        removeEventListener: () => {},
        devicePixelRatio: 1,
        location: { search: "" },
        innerWidth: 1280,
        innerHeight: 720
    },
    requestAnimationFrame: () => 0,
    performance: { now: () => 0 },
    TextDecoder,
    Uint8Array,
    Uint16Array,
    Float32Array,
    GPUBufferUsage: { VERTEX: 1, INDEX: 2, UNIFORM: 4, COPY_DST: 8 },
    GPUTextureUsage: { RENDER_ATTACHMENT: 16, TEXTURE_BINDING: 32, COPY_DST: 64 }
});

const runtime = runInContext(
    `${readFileSync("platform/web/victoriaRuntime.js", "utf8")}\n;({ importObject, runtimeState });`,
    context, { filename: "victoriaRuntime.js" });

const uploadMesh = runtime.importObject.victoriaRender.uploadMesh;
check("the page exposes uploadMesh to the module", typeof uploadMesh === "function");
if (typeof uploadMesh !== "function") {
    process.exit(1);
}

const VERTEX_COUNT = 521;
const INDEX_COUNT = 2211;
const VERTEX_POINTER = 1024;
const INDEX_POINTER = VERTEX_POINTER + VERTEX_COUNT * 24;

const linearMemory = new ArrayBuffer(INDEX_POINTER + INDEX_COUNT * 2 + 2);
const indexView = new Uint16Array(linearMemory, INDEX_POINTER, INDEX_COUNT);
for (let index = 0; index < INDEX_COUNT; index += 1) {
    indexView[index] = index % VERTEX_COUNT;
}
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
        let intact = true;
        for (let index = 0; index < INDEX_COUNT; index += 1) {
            const low = indexWrite.bytes[index * 2];
            const high = indexWrite.bytes[index * 2 + 1];
            if ((low | (high << 8)) !== index % VERTEX_COUNT) {
                intact = false;
            }
        }
        check("every index arrives unchanged", intact);
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
    const createMeshPipeline = runtime.importObject.victoriaRender.createMeshPipeline;
    const uploadTexture = runtime.importObject.victoriaRender.uploadTexture;

    check("the page exposes uploadTexture", typeof uploadTexture === "function");

    const shaderMemory = new ArrayBuffer(4096);
    runtime.runtimeState.instance = { exports: { memory: { buffer: shaderMemory } } };
    runtime.runtimeState.memory = { buffer: shaderMemory };
    runtime.runtimeState.device = makeRecordingDevice();
    runtime.runtimeState.canvasFormat = "bgra8unorm";
    runtime.runtimeState.meshTexture = null;

    check("the mesh pipeline builds", createMeshPipeline(0, 0) === 1);
    check("with a single white pixel standing in for a texture",
          textures.length === 1 && textures[0].size.width === 1 && textures[0].size.height === 1);
    check("and the vertex layout carries a texture coordinate",
          runtime.runtimeState.meshPipeline.descriptor.vertex.buffers[0].arrayStride === 32);
    check("as a third attribute",
          runtime.runtimeState.meshPipeline.descriptor.vertex.buffers[0].attributes.length === 3);

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

console.log("\n-- re-sending the vertices of a mesh that is already here --");
{
    const createMeshPipeline = runtime.importObject.victoriaRender.createMeshPipeline;
    const uploadMesh = runtime.importObject.victoriaRender.uploadMesh;
    const setMeshPart = runtime.importObject.victoriaRender.setMeshPart;
    const uploadPartTexture = runtime.importObject.victoriaRender.uploadPartTexture;
    const updateMeshVertices = runtime.importObject.victoriaRender.updateMeshVertices;

    check("the page exposes updateMeshVertices", typeof updateMeshVertices === "function");

    const COUNT = 64;
    const memory = new ArrayBuffer(COUNT * 32 + 4096);

    runtime.runtimeState.instance = { exports: { memory: { buffer: memory } } };
    runtime.runtimeState.memory = { buffer: memory };
    runtime.runtimeState.device = makeRecordingDevice();
    runtime.runtimeState.canvasFormat = "bgra8unorm";
    runtime.runtimeState.meshTexture = null;
    runtime.runtimeState.meshVertexBuffer = null;
    runtime.runtimeState.meshIndexBuffer = null;
    createMeshPipeline(0, 0);

    const INDICES = COUNT * 32;
    uploadMesh(0, COUNT, INDICES, 6);
    setMeshPart(0, 0, 3);
    setMeshPart(1, 3, 3);
    uploadPartTexture(0, INDICES + 64, 2, 2);
    uploadPartTexture(1, INDICES + 128, 2, 2);

    const partTexture = runtime.runtimeState.meshPartTextures[0];
    const vertexBuffer = runtime.runtimeState.meshVertexBuffer;
    check("the model arrived with two parts", runtime.runtimeState.meshParts.length === 2);
    check("each wearing its own skin",
          Boolean(partTexture) && Boolean(runtime.runtimeState.meshPartTextures[1]));

    new Uint8Array(memory).fill(0x5A, 0, COUNT * 32);
    writes.length = 0;
    buffers.length = 0;
    destroyed.length = 0;

    check("the update is taken", updateMeshVertices(0, COUNT) === 1);
    check("the ranges survive it", runtime.runtimeState.meshParts.length === 2);
    check("and so do the skins",
          runtime.runtimeState.meshPartTextures[0] === partTexture &&
          runtime.runtimeState.meshPartBindGroups.length === 2);
    check("nothing was destroyed to make room", destroyed.length === 0);
    check("no new buffer was allocated for it", buffers.length === 0);
    check("the posed vertices went into the buffer already there",
          writes.length === 1 && writes[0].buffer === vertexBuffer &&
          writes[0].byteLength === COUNT * 32 && writes[0].bytes[0] === 0x5A);

    check("a different vertex count is refused rather than resized",
          updateMeshVertices(0, COUNT - 1) === 0);
    check("and so is one reaching past the end of memory",
          updateMeshVertices(memory.byteLength - 16, COUNT) === 0);

    uploadMesh(0, COUNT, INDICES, 6);
    check("a new mesh still clears the ranges", runtime.runtimeState.meshParts.length === 0);
    check("and gives back what the last one held",
          destroyed.includes(vertexBuffer) && destroyed.includes(partTexture));
}

if (failureCount === 0) {
    console.log("\nall runtime upload checks passed");
    process.exit(0);
}
console.log(`\n${failureCount} runtime upload check(s) failed`);
process.exit(1);
