
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

let simulatedMilliseconds = 0;
let frameCostMilliseconds = 0;
const reportedGraphicsKibibytes = 32 * 1024;
const shaderCompileMilliseconds = 50;

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
            simulatedMilliseconds += shaderCompileMilliseconds;
            return 1;
        },
        setClearColor: (red, green, blue) => calls.push({ name: "setClearColor", red, green, blue }),
        setTriangleTint: (tint) => calls.push({ name: "setTriangleTint", tint }),
        createMeshPipeline: (pointer, length) => {
            calls.push({ name: "createMeshPipeline", shaderSource: decodeUTF8(pointer, length) });
            return 1;
        },
        uploadMesh: (vertexPointer, vertexCount, indexPointer, indexCount) => {
            calls.push({ name: "uploadMesh", vertexCount, indexCount });
            return 1;
        },
        setMeshPart: (partIndex, firstIndex, indexCount) => {
            calls.push({ name: "setMeshPart", partIndex, firstIndex, indexCount });
        },
        updateMeshVertices: (vertexPointer, vertexCount) => {
            calls.push({ name: "updateMeshVertices", vertexCount });
            return 1;
        },
        uploadPartTexture: (partIndex, pixelPointer, width, height) => {
            calls.push({ name: "uploadPartTexture", partIndex, width, height });
            return 1;
        },
        uploadTexture: (pixelPointer, width, height) => {
            calls.push({ name: "uploadTexture", width, height });
            return 1;
        },
        uploadOverlay: (pixelPointer, width, height) => {
            calls.push({ name: "uploadOverlay", width, height });
            return 1;
        },
        setMeshUniforms: () => calls.push({ name: "setMeshUniforms" }),
        submitFrame: () => {
            calls.push({ name: "submitFrame" });
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

check("draws the triangle when no disc was given",
      calls.some((call) => call.name === "setTriangleTint"));
check("and does not build a mesh pipeline it was never given a mesh for",
      !calls.some((call) => call.name === "createMeshPipeline"));
check("nor uploads one", !calls.some((call) => call.name === "uploadMesh"));

const lastMicroseconds = instance.exports.victoriaWebGetFrameMicroseconds();
const worstMicroseconds = instance.exports.victoriaWebGetWorstFrameMicroseconds();
const averageMicroseconds = instance.exports.victoriaWebGetAverageFrameMicroseconds();
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
        setMeshPart: () => {},
        updateMeshVertices: () => 1,
        uploadPartTexture: () => 1,
        uploadTexture: () => 1,
        uploadOverlay: () => 1,
        setMeshUniforms: () => {},
        submitFrame: () => {},
        queryGraphicsMemoryKibibytes: () => reportedGraphicsKibibytes,
        warmUpPipeline: () => refusalCalls.push({ name: "warmUpPipeline" })
    }
};
const refusalModule = await WebAssembly.instantiate(readFileSync(modulePath), refusalImports);
const refusedInitialize = refusalModule.instance.exports.victoriaWebInitialize(1024, 576, 8);

check("initialisation fails when the graphics ceiling is too low", refusedInitialize === 0);
check("nothing is charged after a refusal",
      refusalModule.instance.exports.victoriaWebGetGraphicsMemoryUsedBytes() === 0);
check("no pipeline is built once the ceiling has refused",
      !refusalCalls.some((call) => call.name === "createTrianglePipeline"));

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
    check(`reads a bounded amount (${bytesFetched} of ${image.length} bytes)`,
          bytesFetched < image.length * 3);
    check("built a mesh pipeline once it had geometry",
          calls.some((call) => call.name === "createMeshPipeline"));

    const uploads = calls.filter((call) => call.name === "uploadMesh");
    check("uploaded something to draw", uploads.length > 0);

    const said = (fragment) => loggedMessages.some((text) => text.includes(fragment));

    check("finds all four of a Sim's parts by name",
          said("4 of 4 of a whole Sim's parts are on this disc by name"));
    check("reads a part carrying more than one primitive",
          said("amBodyNaked_cres — 16 vertices, 24 triangles, 16 of them weighted across 2 range(s)"));
    check("joins the parts into one model",
          said("a whole Sim — 3 part(s) joined into 32 vertices and 48 triangles across 4 range(s)"));
    check("and every vertex of every part reaches the join",
          said("32 vertices and 48 triangles"));
    check("hangs them on the skeleton they are weighted to",
          said("hung on auskel_cres — 3 node(s)"));

    check("dresses the Sim out of the catalogue",
          said("the wardrobe was offered 7 entr(ies) that reach a shape and dresses 5 of 5 part(s)"));
    check("reading the skin tone off the body it painted",
          said("dresses 5 of 5 part(s), at the tone s1"));
    check("and taking the face whose tone matches, not the one it met first",
          said("amFace_cres — wearing CASIE_amface_s1"));
    check("wears a top and a bottom together",
          said("a top — wearing amtopjersey_green") &&
          said("a bottom — wearing ambottomshorts_red"));
    check("and drops the whole body they replace",
          said("amBodyNaked_cres — chosen but not worn: ambodyoveralls_blue"));
    check("refuses a garment authored for another age",
          said("authored for another age or gender"));
    check("and joins the dressed Sim again", said("a dressed Sim — 4 part(s) joined"));

    check("paints a garment with the material its entry names, not its shape's",
          said("which its catalogue entry named, not its shape"));

    const upload = uploads[uploads.length - 1];
    const parts = calls.filter((call) => call.name === "setMeshPart");
    check("told the host which range each part covers", parts.length > 0);
    if (parts.length > 0) {
        const finalParts = parts.slice(-upload.partCountAtUpload ?? parts.length);
        check("the first part starts at the beginning", parts[0].firstIndex === 0);
        check("every range has triangles in it",
              parts.every((part) => part.indexCount > 0 && part.indexCount % 3 === 0));
        check("and no range runs past the mesh it belongs to",
              finalParts.every((part) => part.firstIndex + part.indexCount <= upload.indexCount));
    }
    const painted = calls.filter((call) => call.name === "uploadPartTexture");
    check("paints each range separately", painted.length >= 4);
    check("counts packages against everything else",
          loggedMessages.some((text) =>
              text.includes("6 package(s)") && text.includes("3 other file(s)")));
    check("names the largest non-package file first",
          loggedMessages.some((text) => text.includes("KiB  TSData.exe")));

    const installer = loggedMessages.find((text) => text.includes("TSData.exe —"));
    check("probes the installer for a signature", Boolean(installer));
    if (installer) {
        check("naming Delphi's stub, which is what marks it out",
              installer.includes("Delphi-built"));
        check("and printing the front and 0x30 both",
              installer.includes("4D 5A 50 00") && installer.includes("and at 0x30"));
    }

    check("says the table is not where the old loaders keep it",
          loggedMessages.some((text) => text.includes("keeps no table at 0x30")));
    check("reads the program's own section table to find where it ends",
          loggedMessages.some((text) =>
              text.includes("1 section(s) ending at 0x00000200") &&
              text.includes("appended past it")));
    check("and names what is appended there",
          loggedMessages.some((text) =>
              text.includes("what is appended starts 52 61 72 21") &&
              text.includes("a RAR archive")));

    check("names a stored entry, its size and where its data begins",
          loggedMessages.some((text) =>
              text.includes("stored 1 KiB at 0x00000258") &&
              text.includes("mounted.package")));
    check("and a packed one by its unpacked size",
          loggedMessages.some((text) =>
              text.includes("packed 32 bytes at 0x000008EB") &&
              text.includes("Sims02.package")));
    check("counting both kinds separately",
          loggedMessages.some((text) =>
              text.includes("walked 2 archive entries") &&
              text.includes("1 stored (1 KiB), 1 packed")));
    check("mounting the stored package and not the packed one",
          loggedMessages.some((text) => text.includes("1 package(s) mounted")));
    check("and the mounted package really is a package where it says it is",
          loggedMessages.some((text) =>
              text.includes("the first mounted package really is one")));

    const probe = loggedMessages.find((text) => text.includes("data1.cab —"));
    check("probes a file it cannot name", Boolean(probe));
    if (probe) {
        check("reporting the bytes it read", probe.includes("70 6C 61 63 65 68 6F 6C"));
        check("and refusing to name a format it does not recognise",
              probe.includes("unrecognised"));
    }

    {
        const seen = new Map();
        let worst = 0;
        let worstText = "";

        for (const text of loggedMessages) {
            const count = (seen.get(text) ?? 0) + 1;
            seen.set(text, count);
            if (count > worst) {
                worst = count;
                worstText = text;
            }
        }
        check(`nothing repeats more than the three mesh uploads (worst says "${worstText.slice(0, 40)}" ${worst}x)`,
              worst <= 3);
    }

    console.log(`  ${steps} steps, ${rangesFetched} ranges, ${bytesFetched} bytes`);
}

if (failureCount > 0) {
    console.error(`\n${failureCount} check(s) failed`);
    process.exit(1);
}
console.log("\nall web module checks passed");
