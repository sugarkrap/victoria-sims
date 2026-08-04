#include "utils/strings.h"
#include "victoria/freestandingRuntime.h"
#include "victoria/graphicsMemoryBudget.h"
#include "victoria/platformInterface.h"
#include "victoria/profiler.h"
#include "victoria/renderInterface.h"

/* WebGPU has no C entry point in a bare wasm32 module, so the backend is a
   thin command layer over host functions. Everything that decides what to draw
   stays here in C; the host only executes. */
#define WEB_IMPORT(importName) \
    __attribute__((import_module("victoriaRender"), import_name(importName)))

WEB_IMPORT("configureSurface")
extern void hostConfigureSurface(Unsigned32 widthInPixels, Unsigned32 heightInPixels);

WEB_IMPORT("createTrianglePipeline")
extern Integer32 hostCreateTrianglePipeline(const char *shaderSource, Unsigned32 shaderLength);

WEB_IMPORT("setClearColor")
extern void hostSetClearColor(Real32 red, Real32 green, Real32 blue);

WEB_IMPORT("setTriangleTint")
extern void hostSetTriangleTint(Real32 tint);

WEB_IMPORT("submitFrame")
extern void hostSubmitFrame(void);

/* Derived from the adapter's reported limits. WebGPU deliberately does not
   expose total video memory, so this is an informed guess made by the host,
   and zero when it has nothing to go on. */
WEB_IMPORT("queryGraphicsMemoryKibibytes")
extern Unsigned32 hostQueryGraphicsMemoryKibibytes(void);

/* Forces pipeline creation and a first submission to complete now rather than
   during the first visible frame. */
WEB_IMPORT("warmUpPipeline")
extern void hostWarmUpPipeline(void);

#define TRIANGLE_UNIFORM_BUFFER_BYTES 16UL

static Unsigned32 shaderProgramCount = 0;
static Boolean uniformBufferIsCharged = BOOLEAN_FALSE;

static const char *triangleShaderSource =
    "struct VertexOutput {\n"
    "    @builtin(position) position : vec4<f32>,\n"
    "    @location(0) color : vec3<f32>,\n"
    "};\n"
    "@group(0) @binding(0) var<uniform> triangleTint : vec4<f32>;\n"
    "@vertex\n"
    "fn vertexMain(@builtin(vertex_index) vertexIndex : u32) -> VertexOutput {\n"
    "    var positions = array<vec2<f32>, 3>(\n"
    "        vec2<f32>(0.0, 0.6),\n"
    "        vec2<f32>(-0.6, -0.5),\n"
    "        vec2<f32>(0.6, -0.5));\n"
    "    var colors = array<vec3<f32>, 3>(\n"
    "        vec3<f32>(1.0, 0.35, 0.55),\n"
    "        vec3<f32>(0.35, 0.75, 1.0),\n"
    "        vec3<f32>(1.0, 0.9, 0.4));\n"
    "    var output : VertexOutput;\n"
    "    output.position = vec4<f32>(positions[vertexIndex], 0.0, 1.0);\n"
    "    output.color = colors[vertexIndex] * triangleTint.x;\n"
    "    return output;\n"
    "}\n"
    "@fragment\n"
    "fn fragmentMain(input : VertexOutput) -> @location(0) vec4<f32> {\n"
    "    return vec4<f32>(input.color, 1.0);\n"
    "}\n";

MemorySize renderQueryGraphicsMemoryBytes(void)
{
    return (MemorySize)hostQueryGraphicsMemoryKibibytes() * 1024UL;
}

Boolean renderInitialize(MemoryArena *arena, Unsigned32 widthInPixels, Unsigned32 heightInPixels)
{
    (void)arena;

    hostConfigureSurface(widthInPixels, heightInPixels);

    if (graphicsMemoryBudgetRequest(GRAPHICS_MEMORY_CATEGORY_BUFFER, TRIANGLE_UNIFORM_BUFFER_BYTES) ==
        BOOLEAN_FALSE)
    {
        platformLogMessage("renderer: uniform buffer refused by the graphics memory ceiling");
        return BOOLEAN_FALSE;
    }
    uniformBufferIsCharged = BOOLEAN_TRUE;

    VICTORIA_PROFILE_ZONE_BEGIN("renderCompileShaders");
    if (hostCreateTrianglePipeline(triangleShaderSource, (Unsigned32)stringLength(triangleShaderSource)) == 0)
    {
        platformLogMessage("renderer: WebGPU pipeline creation failed");
        VICTORIA_PROFILE_ZONE_END();
        return BOOLEAN_FALSE;
    }
    shaderProgramCount = 1U;
    VICTORIA_PROFILE_ZONE_END();

    VICTORIA_PROFILE_ZONE_BEGIN("renderWarmUpShaders");
    hostWarmUpPipeline();
    VICTORIA_PROFILE_ZONE_END();

    platformLogMessage("renderer: WebGPU backend ready");
    return BOOLEAN_TRUE;
}

Unsigned32 renderGetShaderProgramCount(void)
{
    return shaderProgramCount;
}

void renderResize(Unsigned32 widthInPixels, Unsigned32 heightInPixels)
{
    hostConfigureSurface(widthInPixels, heightInPixels);
}


void renderSetMesh(const GeometryMesh *mesh, MemoryArena *arena)
{
    (void)arena;
    /* Not implemented here yet: this backend still draws the placeholder
       triangle. Said out loud rather than ignored, so a disc that loaded
       correctly is not mistaken for one that did not. */
    if (mesh != NULL_POINTER)
    {
        platformLogMessage("render: the WebGPU backend cannot draw a mesh yet");
    }
}

void renderDrawFrame(Real32 elapsedSeconds)
{
    Real32 colorPulse = 0.65f + (0.35f * mathSine(elapsedSeconds * 1.5f));

    VICTORIA_PROFILE_ZONE_BEGIN("renderDrawFrame");

    hostSetClearColor(0.06f, 0.07f, 0.11f);
    hostSetTriangleTint(colorPulse);
    hostSubmitFrame();

    VICTORIA_PROFILE_ZONE_END();
}

void renderShutdown(void)
{
    if (uniformBufferIsCharged == BOOLEAN_TRUE)
    {
        graphicsMemoryBudgetRelease(GRAPHICS_MEMORY_CATEGORY_BUFFER, TRIANGLE_UNIFORM_BUFFER_BYTES);
        uniformBufferIsCharged = BOOLEAN_FALSE;
    }
    shaderProgramCount = 0U;
}
