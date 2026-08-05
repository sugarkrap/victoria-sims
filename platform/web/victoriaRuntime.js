// Host side of the WebAssembly build. It owns the WebGPU objects and executes
// commands issued from C; it makes no rendering decisions of its own.

const runtimeState = {
    instance: null,
    memory: null,
    canvas: null,
    device: null,
    context: null,
    canvasFormat: null,
    pipeline: null,
    uniformBuffer: null,
    meshPipeline: null,
    meshUniformBuffer: null,
    meshBindGroup: null,
    meshVertexBuffer: null,
    // The vertex buffer's size, so an update can refuse a mesh of a different
    // shape rather than writing past the end of what was uploaded.
    meshVertexBytes: 0,
    meshIndexBuffer: null,
    meshTexture: null,
    meshSampler: null,
    meshIndexCount: 0,
    // One index range and one bind group per part, so a model can be painted a
    // part at a time. WebGPU binds a texture through a group rather than to a
    // slot, so a part with its own texture needs a group of its own — there is
    // no equivalent of rebinding one texture name between draw calls.
    meshParts: [],
    meshPartTextures: [],
    meshPartBindGroups: [],
    depthTexture: null,
    bindGroup: null,
    clearColor: { r: 0, g: 0, b: 0, a: 1 },
    startTimestamp: 0,
    lastOverlayTimestamp: 0,
    frameMicrosecondHistory: []
};

// Matches the engine's own report refresh interval; sampling faster only costs
// work without telling anyone anything new.
const OVERLAY_INTERVAL_MILLISECONDS = 250;
const SPARKLINE_SAMPLE_COUNT = 120;

// Creates the device texture and copies the pixels in. WebGPU wants each row of
// a write to start on a 256 byte boundary, so a row that is not a multiple of
// that has to be padded — which is why this stages rather than writing the
// module's memory straight through.
function setMeshTexture(pixels, width, height) {
    const bytesPerRow = (width * 4 + 255) & ~255;
    const staging = new Uint8Array(bytesPerRow * height);

    for (let row = 0; row < height; row += 1) {
        staging.set(pixels.subarray(row * width * 4, (row + 1) * width * 4), row * bytesPerRow);
    }

    runtimeState.meshTexture = runtimeState.device.createTexture({
        size: { width, height },
        format: "rgba8unorm",
        usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST
    });
    runtimeState.device.queue.writeTexture(
        { texture: runtimeState.meshTexture }, staging,
        { bytesPerRow, rowsPerImage: height }, { width, height });
}

// A texture binds to the layout the mesh's pipeline defines, so there is
// nothing to bind it to until a mesh has been uploaded. That ordering held by
// accident for as long as no texture was ever found; the first disc that
// yielded one arrived here with a null pipeline. The engine now sets the mesh
// first, and this refuses rather than throwing if it ever does not.
function rebuildMeshBindGroup() {
    if (!runtimeState.meshPipeline || !runtimeState.meshTexture) {
        return false;
    }
    runtimeState.meshBindGroup = runtimeState.device.createBindGroup({
        layout: runtimeState.meshPipeline.getBindGroupLayout(0),
        entries: [
            { binding: 0, resource: { buffer: runtimeState.meshUniformBuffer } },
            { binding: 1, resource: runtimeState.meshSampler },
            { binding: 2, resource: runtimeState.meshTexture.createView() }
        ]
    });
    return true;
}

// A part's own bind group, over that part's own texture. Shares the mesh's
// uniform buffer and sampler, because only the image differs between parts.
function rebuildPartBindGroup(partIndex) {
    const texture = runtimeState.meshPartTextures[partIndex];

    if (!runtimeState.meshPipeline || !texture) {
        return false;
    }
    runtimeState.meshPartBindGroups[partIndex] = runtimeState.device.createBindGroup({
        layout: runtimeState.meshPipeline.getBindGroupLayout(0),
        entries: [
            { binding: 0, resource: { buffer: runtimeState.meshUniformBuffer } },
            { binding: 1, resource: runtimeState.meshSampler },
            { binding: 2, resource: texture.createView() }
        ]
    });
    return true;
}

// The depth attachment has to match the canvas, and the canvas resizes with
// the window. Kept until the size changes rather than made every frame, since
// allocating a full screen texture per frame is exactly the sort of thing that
// looks fine at sixty frames a second on a desktop and is not.
function ensureDepthTexture() {
    const width = runtimeState.canvas.width;
    const height = runtimeState.canvas.height;

    if (runtimeState.depthTexture &&
        runtimeState.depthTexture.width === width &&
        runtimeState.depthTexture.height === height) {
        return runtimeState.depthTexture;
    }
    if (runtimeState.depthTexture) {
        runtimeState.depthTexture.destroy();
    }
    runtimeState.depthTexture = runtimeState.device.createTexture({
        size: { width, height },
        format: "depth24plus",
        usage: GPUTextureUsage.RENDER_ATTACHMENT
    });
    return runtimeState.depthTexture;
}

function readUTF8(pointer, length) {
    const bytes = new Uint8Array(runtimeState.memory.buffer, pointer, length);
    return new TextDecoder("utf-8").decode(bytes);
}

function reportStatus(text, isError) {
    const element = document.getElementById("statusLine");
    if (element) {
        element.textContent = text;
        element.classList.toggle("statusError", Boolean(isError));
    }
    if (isError) {
        console.error(text);
    }
}

const importObject = {
    victoriaPlatform: {
        logMessage(pointer, length) {
            console.log(readUTF8(pointer, length));
        },

        getMilliseconds() {
            return performance.now();
        }
    },
    victoriaRender: {
        configureSurface(widthInPixels, heightInPixels) {
            runtimeState.canvas.width = widthInPixels;
            runtimeState.canvas.height = heightInPixels;
            runtimeState.context.configure({
                device: runtimeState.device,
                format: runtimeState.canvasFormat,
                alphaMode: "opaque"
            });
        },

        createTrianglePipeline(shaderPointer, shaderLength) {
            const shaderModule = runtimeState.device.createShaderModule({
                code: readUTF8(shaderPointer, shaderLength)
            });

            runtimeState.pipeline = runtimeState.device.createRenderPipeline({
                layout: "auto",
                vertex: { module: shaderModule, entryPoint: "vertexMain" },
                fragment: {
                    module: shaderModule,
                    entryPoint: "fragmentMain",
                    targets: [{ format: runtimeState.canvasFormat }]
                },
                primitive: { topology: "triangle-list" }
            });

            runtimeState.uniformBuffer = runtimeState.device.createBuffer({
                size: 16,
                usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST
            });

            runtimeState.bindGroup = runtimeState.device.createBindGroup({
                layout: runtimeState.pipeline.getBindGroupLayout(0),
                entries: [{ binding: 0, resource: { buffer: runtimeState.uniformBuffer } }]
            });

            return 1;
        },

        setClearColor(red, green, blue) {
            runtimeState.clearColor = { r: red, g: green, b: blue, a: 1 };
        },

        setTriangleTint(tint) {
            runtimeState.device.queue.writeBuffer(
                runtimeState.uniformBuffer, 0, new Float32Array([tint, 0, 0, 0]));
        },

        // Builds the pipeline a mesh is drawn with. Separate from the triangle's
        // because it takes vertex buffers and tests depth, and because the
        // triangle has to keep working on a build with no disc.
        createMeshPipeline(shaderPointer, shaderLength) {
            const shaderSource = readUTF8(shaderPointer, shaderLength);
            const shaderModule = runtimeState.device.createShaderModule({ code: shaderSource });

            runtimeState.meshPipeline = runtimeState.device.createRenderPipeline({
                layout: "auto",
                vertex: {
                    module: shaderModule,
                    entryPoint: "vertexMain",
                    buffers: [{
                        // Position, normal, texture coordinate: 3 + 3 + 2 floats.
                        arrayStride: 32,
                        attributes: [
                            { shaderLocation: 0, offset: 0, format: "float32x3" },
                            { shaderLocation: 1, offset: 12, format: "float32x3" },
                            { shaderLocation: 2, offset: 24, format: "float32x2" }
                        ]
                    }]
                },
                fragment: {
                    module: shaderModule,
                    entryPoint: "fragmentMain",
                    targets: [{ format: runtimeState.canvasFormat }]
                },
                primitive: { topology: "triangle-list" },
                depthStencil: {
                    format: "depth24plus",
                    depthWriteEnabled: true,
                    depthCompare: "less-equal"
                }
            });

            // Sixteen floats of matrix and four of light direction.
            runtimeState.meshUniformBuffer = runtimeState.device.createBuffer({
                size: 80,
                usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST
            });

            runtimeState.meshSampler = runtimeState.device.createSampler({
                magFilter: "linear",
                minFilter: "linear",
                addressModeU: "repeat",
                addressModeV: "repeat"
            });
            // A single white pixel, so the bind group is complete before any
            // texture arrives and a mesh with no image is lit exactly as it was
            // before textures existed. Without it the shader could not sample
            // unconditionally, and the pipeline would need two variants.
            if (!runtimeState.meshTexture) {
                setMeshTexture(new Uint8Array([255, 255, 255, 255]), 1, 1);
            }
            rebuildMeshBindGroup();
            return 1;
        },

        // Replaces the image the mesh is painted with. The bind group holds the
        // old texture by reference, so it has to be built again — cheap, and
        // done here rather than per frame.
        // Which range of indices belongs to which part. Sent after the mesh,
        // and cleared with it, because a range means nothing against a mesh it
        // did not come from.
        setMeshPart(partIndex, firstIndex, indexCount) {
            runtimeState.meshParts[partIndex] = { firstIndex, indexCount };
        },

        uploadPartTexture(partIndex, pixelPointer, width, height) {
            const memory = runtimeState.instance.exports.memory.buffer;
            const byteCount = width * height * 4;

            if (byteCount <= 0 || pixelPointer + byteCount > memory.byteLength) {
                return 0;
            }
            const pixels = new Uint8Array(memory, pixelPointer, byteCount);
            const bytesPerRow = (width * 4 + 255) & ~255;
            const staging = new Uint8Array(bytesPerRow * height);

            for (let row = 0; row < height; row += 1) {
                staging.set(pixels.subarray(row * width * 4, (row + 1) * width * 4),
                            row * bytesPerRow);
            }
            const texture = runtimeState.device.createTexture({
                size: { width, height },
                format: "rgba8unorm",
                usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST
            });
            runtimeState.device.queue.writeTexture(
                { texture }, staging, { bytesPerRow, rowsPerImage: height }, { width, height });

            if (runtimeState.meshPartTextures[partIndex]) {
                runtimeState.meshPartTextures[partIndex].destroy();
            }
            runtimeState.meshPartTextures[partIndex] = texture;
            return rebuildPartBindGroup(partIndex) ? 1 : 0;
        },

        uploadTexture(pixelPointer, width, height) {
            const memory = runtimeState.instance.exports.memory.buffer;
            const byteCount = width * height * 4;

            if (byteCount <= 0 || pixelPointer + byteCount > memory.byteLength) {
                return 0;
            }
            setMeshTexture(new Uint8Array(memory, pixelPointer, byteCount), width, height);
            return rebuildMeshBindGroup() ? 1 : 0;
        },

        // Copies the mesh out of linear memory into buffers the device owns.
        // The copy happens here, synchronously, which is what lets the module
        // reuse its staging space the moment this returns.
        uploadMesh(vertexPointer, vertexCount, indexPointer, indexCount) {
            const memory = runtimeState.instance.exports.memory.buffer;
            const vertexBytes = vertexCount * 32;
            // Both the buffer and the write have to be a multiple of four bytes,
            // and an odd number of sixteen-bit indices is neither. A mesh of 737
            // triangles is 4422 bytes of indices; the teapot's 6320 happen to
            // land on a multiple of four, which is why rounding only the buffer
            // size looked correct for as long as the teapot was the only model.
            const indexBytes = (indexCount * 2 + 3) & ~3;

            // Whatever the last mesh took, given back. A GPU buffer dropped on
            // the floor is not collected on any schedule worth relying on.
            if (runtimeState.meshVertexBuffer) {
                runtimeState.meshVertexBuffer.destroy();
            }
            if (runtimeState.meshIndexBuffer) {
                runtimeState.meshIndexBuffer.destroy();
            }
            runtimeState.meshVertexBuffer = runtimeState.device.createBuffer({
                size: vertexBytes,
                usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST
            });
            runtimeState.meshVertexBytes = vertexBytes;
            runtimeState.device.queue.writeBuffer(
                runtimeState.meshVertexBuffer, 0,
                new Uint8Array(memory, vertexPointer, vertexBytes));

            // Copied into a padded array of its own rather than read long out of
            // the module's memory: the two extra bytes are somebody else's, and
            // reading them would work right up until the mesh sat at the end of
            // the arena.
            const indexStaging = new Uint8Array(indexBytes);
            indexStaging.set(new Uint8Array(memory, indexPointer, indexCount * 2));

            runtimeState.meshIndexBuffer = runtimeState.device.createBuffer({
                size: indexBytes,
                usage: GPUBufferUsage.INDEX | GPUBufferUsage.COPY_DST
            });
            runtimeState.device.queue.writeBuffer(runtimeState.meshIndexBuffer, 0, indexStaging);

            runtimeState.meshIndexCount = indexCount;
            // The ranges and their textures belonged to the mesh just replaced.
            for (const texture of runtimeState.meshPartTextures) {
                if (texture) {
                    texture.destroy();
                }
            }
            runtimeState.meshParts = [];
            runtimeState.meshPartTextures = [];
            runtimeState.meshPartBindGroups = [];
            return 1;
        },

        // New positions for the mesh already here. Writes over the buffer rather
        // than making one, which is the whole point of it: uploadMesh clears the
        // part ranges and destroys the part textures, and an animation calling
        // that once a frame left the Sim wearing one skin over its whole body
        // from the first frame it moved.
        //
        // Refuses a different vertex count instead of resizing. A mesh of
        // another shape is another model, and building one is uploadMesh's job.
        updateMeshVertices(vertexPointer, vertexCount) {
            const memory = runtimeState.instance.exports.memory.buffer;
            const vertexBytes = vertexCount * 32;

            if (!runtimeState.meshVertexBuffer || vertexBytes !== runtimeState.meshVertexBytes ||
                vertexPointer + vertexBytes > memory.byteLength) {
                return 0;
            }
            runtimeState.device.queue.writeBuffer(
                runtimeState.meshVertexBuffer, 0,
                new Uint8Array(memory, vertexPointer, vertexBytes));
            return 1;
        },

        setMeshUniforms(valuePointer) {
            const memory = runtimeState.instance.exports.memory.buffer;
            runtimeState.device.queue.writeBuffer(
                runtimeState.meshUniformBuffer, 0, new Uint8Array(memory, valuePointer, 80));
        },

        submitFrame() {
            const drawingMesh = runtimeState.meshIndexCount > 0;
            const encoder = runtimeState.device.createCommandEncoder();
            const descriptor = {
                colorAttachments: [{
                    view: runtimeState.context.getCurrentTexture().createView(),
                    clearValue: runtimeState.clearColor,
                    loadOp: "clear",
                    storeOp: "store"
                }]
            };

            if (drawingMesh) {
                descriptor.depthStencilAttachment = {
                    view: ensureDepthTexture().createView(),
                    depthClearValue: 1,
                    depthLoadOp: "clear",
                    depthStoreOp: "store"
                };
            }

            const pass = encoder.beginRenderPass(descriptor);
            if (drawingMesh) {
                pass.setPipeline(runtimeState.meshPipeline);
                pass.setVertexBuffer(0, runtimeState.meshVertexBuffer);
                pass.setIndexBuffer(runtimeState.meshIndexBuffer, "uint16");
                if (runtimeState.meshParts.length > 0) {
                    // A part at a time, each with its own skin. A Sim drawn in
                    // one call wears one texture, and with a face's texture on
                    // its body the arms come out banded with an eyebrow.
                    for (let part = 0; part < runtimeState.meshParts.length; part += 1) {
                        const range = runtimeState.meshParts[part];

                        if (!range || range.indexCount === 0) {
                            continue;
                        }
                        // A part never given a texture falls back to the
                        // model's, so a single-texture model is unaffected.
                        pass.setBindGroup(0, runtimeState.meshPartBindGroups[part] ||
                                             runtimeState.meshBindGroup);
                        pass.drawIndexed(range.indexCount, 1, range.firstIndex, 0, 0);
                    }
                } else {
                    pass.setBindGroup(0, runtimeState.meshBindGroup);
                    pass.drawIndexed(runtimeState.meshIndexCount, 1, 0, 0, 0);
                }
            } else {
                pass.setPipeline(runtimeState.pipeline);
                pass.setBindGroup(0, runtimeState.bindGroup);
                pass.draw(3, 1, 0, 0);
            }
            pass.end();
            runtimeState.device.queue.submit([encoder.finish()]);
        },

        // WebGPU deliberately does not expose total video memory. maxBufferSize
        // is the closest thing an adapter will admit to, and it tracks the
        // device class well enough to be a better starting point than a fixed
        // guess. Reported as a hint, not a measurement.
        queryGraphicsMemoryKibibytes() {
            const maximumBufferBytes = runtimeState.device?.limits?.maxBufferSize;
            if (!maximumBufferBytes || !Number.isFinite(maximumBufferBytes)) {
                return 0;
            }
            return Math.floor(maximumBufferBytes / 1024);
        },

        // Draws once into an off-screen target so pipeline compilation and
        // first-use specialisation happen now instead of during frame one.
        warmUpPipeline() {
            const warmUpTexture = runtimeState.device.createTexture({
                size: { width: 1, height: 1 },
                format: runtimeState.canvasFormat,
                usage: GPUTextureUsage.RENDER_ATTACHMENT
            });

            const encoder = runtimeState.device.createCommandEncoder();
            const pass = encoder.beginRenderPass({
                colorAttachments: [{
                    view: warmUpTexture.createView(),
                    clearValue: { r: 0, g: 0, b: 0, a: 1 },
                    loadOp: "clear",
                    storeOp: "store"
                }]
            });
            pass.setPipeline(runtimeState.pipeline);
            pass.setBindGroup(0, runtimeState.bindGroup);
            pass.draw(3, 1, 0, 0);
            pass.end();
            runtimeState.device.queue.submit([encoder.finish()]);
            warmUpTexture.destroy();
        }
    }
};

function resizeToDisplaySize() {
    const pixelRatio = window.devicePixelRatio || 1;
    const width = Math.max(1, Math.floor(runtimeState.canvas.clientWidth * pixelRatio));
    const height = Math.max(1, Math.floor(runtimeState.canvas.clientHeight * pixelRatio));

    if (runtimeState.canvas.width !== width || runtimeState.canvas.height !== height) {
        runtimeState.instance.exports.victoriaWebResize(width, height);
    }
}

// Reads the report the engine already formatted, rather than formatting one
// here: the same text appears on the terminal in the Linux build.
function updateProfilerOverlay() {
    const exports = runtimeState.instance.exports;
    const pointer = exports.victoriaWebGetProfilerReportPointer();
    const length = exports.victoriaWebGetProfilerReportLength();

    const reportElement = document.getElementById("profilerReport");
    if (reportElement && length > 0) {
        reportElement.textContent = readUTF8(pointer, length);
    }

    drawFrameSparkline();
}

function drawFrameSparkline() {
    const canvas = document.getElementById("profilerSparkline");
    if (!canvas) {
        return;
    }

    const pixelRatio = window.devicePixelRatio || 1;
    const width = Math.max(1, Math.floor(canvas.clientWidth * pixelRatio));
    const height = Math.max(1, Math.floor(canvas.clientHeight * pixelRatio));
    if (canvas.width !== width || canvas.height !== height) {
        canvas.width = width;
        canvas.height = height;
    }

    const context = canvas.getContext("2d");
    const samples = runtimeState.frameMicrosecondHistory;
    context.clearRect(0, 0, width, height);

    if (samples.length < 2) {
        return;
    }

    // Scale against the 60 Hz budget so the line's height means something
    // absolute, not just relative to whatever the worst recent frame was.
    const budgetMicroseconds = 16667;
    const peak = Math.max(budgetMicroseconds, ...samples);
    const stepX = width / (SPARKLINE_SAMPLE_COUNT - 1);
    // Inset so a line sitting at either extreme is not clipped in half.
    const inset = 2 * pixelRatio;
    const plotHeight = height - (inset * 2);
    const plotY = (microseconds) => height - inset - (microseconds / peak) * plotHeight;

    const budgetY = plotY(budgetMicroseconds);
    context.strokeStyle = "#3a4152";
    context.lineWidth = pixelRatio;
    context.beginPath();
    context.moveTo(0, budgetY);
    context.lineTo(width, budgetY);
    context.stroke();

    context.strokeStyle = "#6fd3ff";
    context.lineWidth = 1.5 * pixelRatio;
    context.beginPath();
    samples.forEach((microseconds, index) => {
        const x = index * stepX;
        const y = plotY(microseconds);
        if (index === 0) {
            context.moveTo(x, y);
        } else {
            context.lineTo(x, y);
        }
    });
    context.stroke();
}

function renderLoop(timestamp) {
    if (runtimeState.startTimestamp === 0) {
        runtimeState.startTimestamp = timestamp;
    }
    resizeToDisplaySize();
    runtimeState.instance.exports.victoriaWebRenderFrame((timestamp - runtimeState.startTimestamp) / 1000);

    runtimeState.frameMicrosecondHistory.push(
        runtimeState.instance.exports.victoriaWebGetFrameIntervalMicroseconds());
    if (runtimeState.frameMicrosecondHistory.length > SPARKLINE_SAMPLE_COUNT) {
        runtimeState.frameMicrosecondHistory.shift();
    }

    if (timestamp - runtimeState.lastOverlayTimestamp >= OVERLAY_INTERVAL_MILLISECONDS) {
        runtimeState.lastOverlayTimestamp = timestamp;
        updateProfilerOverlay();
    }

    requestAnimationFrame(renderLoop);
}

async function start() {
    runtimeState.canvas = document.getElementById("victoriaCanvas");

    if (!navigator.gpu) {
        reportStatus("WebGPU is not available in this browser.", true);
        return;
    }

    const adapter = await navigator.gpu.requestAdapter();
    if (!adapter) {
        reportStatus("No WebGPU adapter available.", true);
        return;
    }

    runtimeState.device = await adapter.requestDevice();
    runtimeState.context = runtimeState.canvas.getContext("webgpu");
    runtimeState.canvasFormat = navigator.gpu.getPreferredCanvasFormat();

    const response = await fetch("victoriaSims.wasm");
    const wasm = await WebAssembly.instantiate(await response.arrayBuffer(), importObject);

    runtimeState.instance = wasm.instance;
    runtimeState.memory = wasm.instance.exports.memory;

    const pixelRatio = window.devicePixelRatio || 1;
    const initialWidth = Math.max(1, Math.floor(runtimeState.canvas.clientWidth * pixelRatio));
    const initialHeight = Math.max(1, Math.floor(runtimeState.canvas.clientHeight * pixelRatio));

    // ?graphicsMemoryMebibytes=8 simulates a small-memory device, which is the
    // only practical way to exercise the ceiling from a desktop browser.
    const requestedMebibytes = Number(
        new URLSearchParams(window.location.search).get("graphicsMemoryMebibytes"));
    const overrideBytes = Number.isFinite(requestedMebibytes) && requestedMebibytes > 0
        ? Math.floor(requestedMebibytes * 1024 * 1024)
        : 0;

    if (!runtimeState.instance.exports.victoriaWebInitialize(
            initialWidth, initialHeight, overrideBytes)) {
        reportStatus("Engine failed to initialize.", true);
        return;
    }

    const budgetMebibytes = runtimeState.instance.exports.victoriaWebGetBudgetTotalBytes() / (1024 * 1024);
    const linearMebibytes = runtimeState.memory.buffer.byteLength / (1024 * 1024);
    const graphicsMebibytes =
        runtimeState.instance.exports.victoriaWebGetGraphicsMemoryLimitBytes() / (1024 * 1024);
    reportStatus(
        `WebGPU running. Arena ${budgetMebibytes} MiB, wasm linear memory ` +
        `${linearMebibytes.toFixed(1)} MiB, graphics ceiling ${graphicsMebibytes.toFixed(1)} MiB.`,
        false);

    requestAnimationFrame(renderLoop);
}

start().catch((error) => reportStatus(`Startup failed: ${error.message}`, true));


// Handing the engine a disc.
//
// The page owns the File and the event loop; the engine owns the formats. So
// the page drives: open, then step, and whenever a step leaves a range wanted,
// slice it out of the File, write it where the module asked, and step again.
//
// A File is a handle, not bytes — the browser reads ranges off disk on demand —
// so a three gigabyte image costs the page nothing to hold. That is the same
// property vic-extractor relies on to read a retail disc in a megabyte and a
// half of wasm.
const DISC_STATUS_WORKING = 1;
const DISC_STATUS_READY = 2;
const STEPS_BEFORE_YIELDING = 64;

async function loadDisc(file) {
    const exports = runtimeState.instance.exports;

    if (!exports.victoriaWebOpenDisc(file.size)) {
        reportDiscMessage(`could not start reading ${file.name}`);
        return;
    }
    reportDiscMessage(`reading ${file.name}…`);

    for (let step = 1; ; step += 1) {
        const status = exports.victoriaWebStepDiscLoad();

        if (status !== DISC_STATUS_WORKING) {
            reportDiscMessage(status === DISC_STATUS_READY
                ? `${file.name} loaded`
                : `nothing on ${file.name} could be drawn — see the log`);
            return;
        }

        const length = exports.victoriaWebGetWantedLength();
        if (length > 0) {
            const offset = exports.victoriaWebGetWantedOffset();
            const bytes = new Uint8Array(await file.slice(offset, offset + length).arrayBuffer());
            // Taken after the call that reports the pointer, since a growing
            // linear memory detaches any view made before it.
            new Uint8Array(exports.memory.buffer,
                           exports.victoriaWebGetDeliveryPointer(), length).set(bytes);
            exports.victoriaWebDeliver();
        } else if (step % STEPS_BEFORE_YIELDING === 0) {
            // Walking a catalogue asks for nothing, and a few thousand of those
            // in a row would hold the frame. Let the page breathe.
            await new Promise((resolve) => setTimeout(resolve, 0));
        }
    }
}

function reportDiscMessage(text) {
    const element = document.getElementById("discStatus");
    if (element) {
        element.textContent = text;
    }
    console.log(`disc: ${text}`);
}

function connectDiscPicker() {
    const input = document.getElementById("discInput");
    const zone = document.getElementById("discZone");

    if (input) {
        input.addEventListener("change", (event) => {
            if (event.target.files[0]) {
                loadDisc(event.target.files[0]);
            }
        });
    }
    if (zone) {
        zone.addEventListener("dragover", (event) => {
            event.preventDefault();
            zone.classList.add("isHot");
        });
        zone.addEventListener("dragleave", () => zone.classList.remove("isHot"));
        zone.addEventListener("drop", (event) => {
            event.preventDefault();
            zone.classList.remove("isHot");
            if (event.dataTransfer.files[0]) {
                loadDisc(event.dataTransfer.files[0]);
            }
        });
    }
}

if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", connectDiscPicker);
} else {
    connectDiscPicker();
}
