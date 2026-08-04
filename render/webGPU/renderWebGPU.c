#include "victoria/freestandingRuntime.h"
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

Boolean renderInitialize(MemoryArena *arena, Unsigned32 widthInPixels, Unsigned32 heightInPixels)
{
    (void)arena;

    hostConfigureSurface(widthInPixels, heightInPixels);

    if (hostCreateTrianglePipeline(triangleShaderSource, (Unsigned32)stringLength(triangleShaderSource)) == 0)
    {
        platformLogMessage("renderer: WebGPU pipeline creation failed");
        return BOOLEAN_FALSE;
    }

    platformLogMessage("renderer: WebGPU backend ready");
    return BOOLEAN_TRUE;
}

void renderResize(Unsigned32 widthInPixels, Unsigned32 heightInPixels)
{
    hostConfigureSurface(widthInPixels, heightInPixels);
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
}
