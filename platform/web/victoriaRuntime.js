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
    bindGroup: null,
    clearColor: { r: 0, g: 0, b: 0, a: 1 },
    startTimestamp: 0
};

function readUtf8(pointer, length) {
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
            console.log(readUtf8(pointer, length));
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
                code: readUtf8(shaderPointer, shaderLength)
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

        submitFrame() {
            const encoder = runtimeState.device.createCommandEncoder();
            const pass = encoder.beginRenderPass({
                colorAttachments: [{
                    view: runtimeState.context.getCurrentTexture().createView(),
                    clearValue: runtimeState.clearColor,
                    loadOp: "clear",
                    storeOp: "store"
                }]
            });
            pass.setPipeline(runtimeState.pipeline);
            pass.setBindGroup(0, runtimeState.bindGroup);
            pass.draw(3, 1, 0, 0);
            pass.end();
            runtimeState.device.queue.submit([encoder.finish()]);
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

function renderLoop(timestamp) {
    if (runtimeState.startTimestamp === 0) {
        runtimeState.startTimestamp = timestamp;
    }
    resizeToDisplaySize();
    runtimeState.instance.exports.victoriaWebRenderFrame((timestamp - runtimeState.startTimestamp) / 1000);
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

    if (!runtimeState.instance.exports.victoriaWebInitialize(initialWidth, initialHeight)) {
        reportStatus("Engine failed to initialize.", true);
        return;
    }

    const budgetMebibytes = runtimeState.instance.exports.victoriaWebGetBudgetTotalBytes() / (1024 * 1024);
    const linearMebibytes = runtimeState.memory.buffer.byteLength / (1024 * 1024);
    reportStatus(
        `WebGPU running. Arena ${budgetMebibytes} MiB, wasm linear memory ${linearMebibytes.toFixed(1)} MiB.`,
        false);

    requestAnimationFrame(renderLoop);
}

start().catch((error) => reportStatus(`Startup failed: ${error.message}`, true));
