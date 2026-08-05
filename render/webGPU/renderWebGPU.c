#include "utils/strings.h"
#include "victoria/freestandingRuntime.h"
#include "victoria/graphicsMemoryBudget.h"
#include "victoria/platformInterface.h"
#include "victoria/profiler.h"
#include "render/meshCamera.h"
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

/* Mesh drawing. The host is handed pointers into linear memory and copies out
   of them; nothing here waits on the result, because a WebGPU upload is
   recorded rather than performed. */
WEB_IMPORT("createMeshPipeline")
extern Integer32 hostCreateMeshPipeline(const char *shaderSource, Unsigned32 shaderLength);

WEB_IMPORT("uploadMesh")
extern Integer32 hostUploadMesh(const Real32 *interleavedVertices, Unsigned32 vertexCount,
                                const Unsigned16 *indices, Unsigned32 indexCount);

/* Sixteen floats of matrix followed by four of light direction, which is the
   padding WebGPU wants anyway. */
WEB_IMPORT("setMeshUniforms")
extern void hostSetMeshUniforms(const Real32 *values);

/* The image the mesh is painted with. Sent before the pipeline is built, so the
   host has a texture to bind rather than having to rebuild the binding later. */
WEB_IMPORT("uploadTexture")
extern Integer32 hostUploadTexture(const Unsigned8 *rgbaPixels, Unsigned32 widthInPixels,
                                   Unsigned32 heightInPixels);

#define TRIANGLE_UNIFORM_BUFFER_BYTES 16UL

static Unsigned32 shaderProgramCount = 0;
static Boolean uniformBufferIsCharged = BOOLEAN_FALSE;

static Boolean meshIsReady = BOOLEAN_FALSE;
static MemorySize meshChargedBytes = 0UL;
static MemorySize textureChargedBytes = 0UL;
static MeshCamera meshCamera;
static Real32 viewportAspect = 1.0f;

/* Same lighting as the OpenGL ES backend, in the language this one speaks. The
   light arrives already rotated into model space, so a vertex is lit from its
   own normal and no normal matrix is needed. */
static const char *meshShaderSource =
    "struct Uniforms {\n"
    "    modelViewProjection : mat4x4<f32>,\n"
    "    lightDirection : vec4<f32>,\n"
    "};\n"
    "@group(0) @binding(0) var<uniform> uniforms : Uniforms;\n"
    "@group(0) @binding(1) var meshSampler : sampler;\n"
    "@group(0) @binding(2) var meshTexture : texture_2d<f32>;\n"
    "struct VertexOutput {\n"
    "    @builtin(position) position : vec4<f32>,\n"
    "    @location(0) normal : vec3<f32>,\n"
    "    @location(1) textureCoordinate : vec2<f32>,\n"
    "};\n"
    "@vertex\n"
    "fn vertexMain(@location(0) vertexPosition : vec3<f32>,\n"
    "              @location(1) vertexNormal : vec3<f32>,\n"
    "              @location(2) vertexTextureCoordinate : vec2<f32>) -> VertexOutput {\n"
    "    var output : VertexOutput;\n"
    "    output.position = uniforms.modelViewProjection * vec4<f32>(vertexPosition, 1.0);\n"
    "    output.normal = vertexNormal;\n"
    "    output.textureCoordinate = vertexTextureCoordinate;\n"
    "    return output;\n"
    "}\n"
    "@fragment\n"
    "fn fragmentMain(input : VertexOutput) -> @location(0) vec4<f32> {\n"
    "    let normal = normalize(input.normal);\n"
    "    let lambert = abs(dot(normal, uniforms.lightDirection.xyz));\n"
    "    let shade = 0.28 + (0.72 * lambert);\n"
    /* White when there is no image, so an untextured mesh is lit exactly as it
       was before any of this existed. */
    "    let painted = textureSample(meshTexture, meshSampler, input.textureCoordinate);\n"
    "    return vec4<f32>(painted.rgb * vec3<f32>(shade, shade * 0.93, shade * 0.84), 1.0);\n"
    "}\n";

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
    viewportAspect = (heightInPixels > 0U) ? ((Real32)widthInPixels / (Real32)heightInPixels) : 1.0f;
}


void renderSetMesh(const GeometryMesh *mesh, MemoryArena *arena)
{
    MemorySize marker;
    Real32 *interleaved;
    MemorySize vertexBytes;
    MemorySize indexBytes;
    Unsigned32 index;

    meshIsReady = BOOLEAN_FALSE;
    if (mesh == NULL_POINTER || mesh->vertexCount == 0U || mesh->indexCount < 3U ||
        mesh->normals == NULL_POINTER)
    {
        return;
    }

    /* Position, normal and texture coordinate: eight floats a vertex. The
       coordinate is carried even when the mesh has none, because a vertex
       layout that changes shape per mesh means a pipeline per mesh. */
    vertexBytes = (MemorySize)mesh->vertexCount * 8UL * sizeof(Real32);
    indexBytes = (MemorySize)mesh->indexCount * sizeof(Unsigned16);

    if (graphicsMemoryBudgetRequest(GRAPHICS_MEMORY_CATEGORY_BUFFER, vertexBytes + indexBytes) ==
        BOOLEAN_FALSE)
    {
        platformLogMessage("render: the graphics ceiling will not hold this mesh");
        return;
    }
    meshChargedBytes = vertexBytes + indexBytes;

    if (hostCreateMeshPipeline(meshShaderSource, (Unsigned32)stringLength(meshShaderSource)) == 0)
    {
        platformLogMessage("render: the host would not build the mesh pipeline");
        graphicsMemoryBudgetRelease(GRAPHICS_MEMORY_CATEGORY_BUFFER, meshChargedBytes);
        meshChargedBytes = 0UL;
        return;
    }
    shaderProgramCount += 1U;

    marker = memoryArenaGetMarker(arena);
    interleaved = (Real32 *)memoryArenaAllocate(arena, vertexBytes, sizeof(Real32));
    if (interleaved == NULL_POINTER)
    {
        platformLogMessage("render: not enough arena to stage the mesh");
        memoryArenaRewindToMarker(arena, marker);
        graphicsMemoryBudgetRelease(GRAPHICS_MEMORY_CATEGORY_BUFFER, meshChargedBytes);
        meshChargedBytes = 0UL;
        return;
    }
    for (index = 0U; index < mesh->vertexCount; index++)
    {
        const Real32 *position = &mesh->positions[(MemorySize)index * 3UL];
        const Real32 *normal = &mesh->normals[(MemorySize)index * 3UL];
        Real32 *target = &interleaved[(MemorySize)index * 8UL];

        target[0] = position[0];
        target[1] = position[1];
        target[2] = position[2];
        target[3] = normal[0];
        target[4] = normal[1];
        target[5] = normal[2];
        if (mesh->textureCoordinates != NULL_POINTER)
        {
            target[6] = mesh->textureCoordinates[(MemorySize)index * 2UL];
            target[7] = mesh->textureCoordinates[(MemorySize)index * 2UL + 1UL];
        }
        else
        {
            target[6] = 0.0f;
            target[7] = 0.0f;
        }
    }

    /* The host copies during this call, so the scratch can go back straight
       afterwards. */
    if (hostUploadMesh(interleaved, mesh->vertexCount, mesh->indices, mesh->indexCount) == 0)
    {
        platformLogMessage("render: the host would not take the mesh");
        memoryArenaRewindToMarker(arena, marker);
        graphicsMemoryBudgetRelease(GRAPHICS_MEMORY_CATEGORY_BUFFER, meshChargedBytes);
        meshChargedBytes = 0UL;
        return;
    }
    memoryArenaRewindToMarker(arena, marker);

    meshCameraFrame(&meshCamera, mesh);
    meshIsReady = BOOLEAN_TRUE;
    platformLogMessage("render: mesh uploaded to the WebGPU backend");
}

void renderSetTexture(const Unsigned8 *rgbaPixels, Unsigned32 widthInPixels,
                      Unsigned32 heightInPixels)
{
    MemorySize wantedBytes;

    if (rgbaPixels == NULL_POINTER || widthInPixels == 0U || heightInPixels == 0U)
    {
        return;
    }
    wantedBytes = (MemorySize)widthInPixels * (MemorySize)heightInPixels * 4UL;

    if (graphicsMemoryBudgetRequest(GRAPHICS_MEMORY_CATEGORY_TEXTURE, wantedBytes) == BOOLEAN_FALSE)
    {
        platformLogMessage("render: the graphics ceiling will not hold this texture");
        return;
    }
    if (hostUploadTexture(rgbaPixels, widthInPixels, heightInPixels) == 0)
    {
        platformLogMessage("render: the host would not take the texture");
        graphicsMemoryBudgetRelease(GRAPHICS_MEMORY_CATEGORY_TEXTURE, wantedBytes);
        return;
    }
    /* Released before charging again, so loading a second model does not leave
       the first model's texture counted against the ceiling forever. */
    if (textureChargedBytes > 0UL)
    {
        graphicsMemoryBudgetRelease(GRAPHICS_MEMORY_CATEGORY_TEXTURE, textureChargedBytes);
    }
    textureChargedBytes = wantedBytes;
    platformLogMessage("render: texture uploaded to the WebGPU backend");
}

void renderDrawFrame(Real32 elapsedSeconds)
{
    Real32 colorPulse = 0.65f + (0.35f * mathSine(elapsedSeconds * 1.5f));

    VICTORIA_PROFILE_ZONE_BEGIN("renderDrawFrame");

    hostSetClearColor(0.06f, 0.07f, 0.11f);
    if (meshIsReady == BOOLEAN_TRUE)
    {
        /* Matrix then light, laid out the way the shader's uniform block
           expects, so the host has nothing to rearrange. */
        Real32 uniforms[20];
        Real32 angle = elapsedSeconds * 0.6f;

        meshCameraBuildMatrix(&meshCamera, angle, viewportAspect, uniforms);
        meshCameraGetLightDirection(angle, &uniforms[16]);
        uniforms[19] = 0.0f;
        hostSetMeshUniforms(uniforms);
    }
    else
    {
        hostSetTriangleTint(colorPulse);
    }
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
    if (meshChargedBytes > 0UL)
    {
        graphicsMemoryBudgetRelease(GRAPHICS_MEMORY_CATEGORY_BUFFER, meshChargedBytes);
        meshChargedBytes = 0UL;
    }
    if (textureChargedBytes > 0UL)
    {
        graphicsMemoryBudgetRelease(GRAPHICS_MEMORY_CATEGORY_TEXTURE, textureChargedBytes);
        textureChargedBytes = 0UL;
    }
    meshIsReady = BOOLEAN_FALSE;
    shaderProgramCount = 0U;
}
