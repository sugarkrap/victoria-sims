#include "utils/strings.h"
#include "victoria/freestandingRuntime.h"
#include "victoria/graphicsMemoryBudget.h"
#include "victoria/platformInterface.h"
#include "victoria/profiler.h"
#include "render/meshCamera.h"
#include "victoria/renderInterface.h"

/* Radians a second the camera goes round the model. Settable so a frame
   being compared against another can be taken from the same angle — see the
   note on renderSetCameraOrbitRate for why that is worth a knob. */
static Real32 cameraOrbitRate = RENDER_CAMERA_ORBIT_DEFAULT;
/* Where the orbit starts from, so holding the camera still can hold it
   somewhere worth looking at rather than wherever nought happens to be. */
static Real32 cameraStartAngle = 0.0f;

void renderSetCameraOrbitRate(Real32 radiansPerSecond)
{
    cameraOrbitRate = radiansPerSecond;
}

void renderSetCameraAngle(Real32 radians)
{
    cameraStartAngle = radians;
}


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

/* New positions for the mesh already uploaded, written over the buffer the host
 * is holding rather than into one of their own.
 *
 * Distinct from uploadMesh because uploadMesh means a new model: it throws away
 * the part ranges and the part textures, since neither means anything against a
 * mesh they did not come from. An animation re-sends its vertices every frame,
 * and sending them through uploadMesh stripped the Sim of its three skins on
 * the first frame it moved — after which the whole body drew in one call under
 * whatever single texture was left, banding the arms with a face.
 *
 * The host refuses a vertex count other than the one it holds, because that is
 * a different model and this is the entry point that cannot make one. */
WEB_IMPORT("updateMeshVertices")
extern Integer32 hostUpdateMeshVertices(const Real32 *interleavedVertices, Unsigned32 vertexCount);

/* Sixteen floats of matrix followed by four of light direction, which is the
   padding WebGPU wants anyway. */
WEB_IMPORT("setMeshUniforms")
extern void hostSetMeshUniforms(const Real32 *values);

/* The image the mesh is painted with. Sent before the pipeline is built, so the
   host has a texture to bind rather than having to rebuild the binding later. */
WEB_IMPORT("uploadTexture")
extern Integer32 hostUploadTexture(const Unsigned8 *rgbaPixels, Unsigned32 widthInPixels,
                                   Unsigned32 heightInPixels);

/* One range of the mesh's indices, so the host can draw a part at a time. Sent
   after the mesh and before any part texture, because a texture belongs to a
   part and there are no parts until these arrive. */
WEB_IMPORT("setMeshPart")
extern void hostSetMeshPart(Unsigned32 partIndex, Unsigned32 firstIndex, Unsigned32 indexCount);

/* A texture for one part, which the host binds into a group of that part's own.
   WebGPU binds a texture through a bind group rather than to a slot, so a part
   with its own texture needs its own group — which is the whole reason this
   could not simply reuse uploadTexture. */
WEB_IMPORT("uploadPartTexture")
extern Integer32 hostUploadPartTexture(Unsigned32 partIndex, const Unsigned8 *rgbaPixels,
                                       Unsigned32 widthInPixels, Unsigned32 heightInPixels);

/* The engine's interface, as one premultiplied image drawn over everything. The
 * host builds its own pipeline for it the first time one arrives, for the same
 * reason the OpenGL ES backend does: a run that never opens the menu never pays
 * for it. Nought by nought takes it away again. */
WEB_IMPORT("uploadOverlay")
extern Integer32 hostUploadOverlay(const Unsigned8 *pixels, Unsigned32 widthInPixels,
                                   Unsigned32 heightInPixels);

#define TRIANGLE_UNIFORM_BUFFER_BYTES 16UL

static Unsigned32 shaderProgramCount = 0;
static Boolean uniformBufferIsCharged = BOOLEAN_FALSE;

static Boolean meshIsReady = BOOLEAN_FALSE;
static MemorySize meshChargedBytes = 0UL;
/* How many vertices the host holds, so an update can refuse a mesh of another
   shape rather than sending the host a buffer it will not fit. */
static Unsigned32 meshVertexCountUploaded = 0U;
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
    /* Set here rather than left to the first resize event: a window that never
       resizes from its initial, non-square shape would otherwise be drawn as
       if it were square for its entire session. */
    viewportAspect = (heightInPixels > 0U) ? ((Real32)widthInPixels / (Real32)heightInPixels) : 1.0f;

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


/* Position, normal and texture coordinate, eight floats a vertex, into storage
   the caller owns. Shared by the upload and the per-frame update so the two
   cannot disagree about the layout the pipeline was built against. */
static void interleaveMeshVertices(const GeometryMesh *mesh, Real32 *interleaved)
{
    Unsigned32 index;

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
}

void renderSetMesh(const GeometryMesh *mesh, MemoryArena *arena)
{
    MemorySize marker;
    Real32 *interleaved;
    MemorySize vertexBytes;
    MemorySize indexBytes;

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

    /* Whatever the last mesh charged, given back before this one asks. Without
       it every call charges afresh and nothing is released, which is invisible
       while a mesh is set once per load and fatal once one is re-sent per
       frame. */
    if (meshChargedBytes > 0UL)
    {
        graphicsMemoryBudgetRelease(GRAPHICS_MEMORY_CATEGORY_BUFFER, meshChargedBytes);
        meshChargedBytes = 0UL;
    }

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
    interleaveMeshVertices(mesh, interleaved);

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

    meshVertexCountUploaded = mesh->vertexCount;
    {
        Unsigned32 part;
        Unsigned32 partCount = (mesh->storedPrimitiveCount < RENDER_PART_LIMIT)
                                   ? mesh->storedPrimitiveCount
                                   : RENDER_PART_LIMIT;

        for (part = 0U; part < partCount; part++)
        {
            Unsigned32 indexCount = mesh->primitives[part].indexCount;

            /* A model with more parts than can be held draws the rest under the
               last range, so nothing goes missing and the join is visible. */
            if (part + 1U == RENDER_PART_LIMIT && mesh->storedPrimitiveCount > RENDER_PART_LIMIT)
            {
                indexCount = mesh->indexCount - mesh->primitives[part].firstIndex;
            }
            hostSetMeshPart(part, mesh->primitives[part].firstIndex, indexCount);
        }
    }
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

static MemorySize overlayChargedBytes = 0UL;

void renderSetOverlay(const Unsigned8 *pixels, Unsigned32 widthInPixels,
                      Unsigned32 heightInPixels)
{
    MemorySize wantedBytes;

    if (pixels == NULL_POINTER || widthInPixels == 0U || heightInPixels == 0U)
    {
        (void)hostUploadOverlay(NULL_POINTER, 0U, 0U);
        if (overlayChargedBytes > 0UL)
        {
            graphicsMemoryBudgetRelease(GRAPHICS_MEMORY_CATEGORY_TEXTURE, overlayChargedBytes);
            overlayChargedBytes = 0UL;
        }
        return;
    }

    wantedBytes = (MemorySize)widthInPixels * (MemorySize)heightInPixels * 4UL;
    if (wantedBytes > overlayChargedBytes &&
        graphicsMemoryBudgetRequest(GRAPHICS_MEMORY_CATEGORY_TEXTURE,
                                    wantedBytes - overlayChargedBytes) == BOOLEAN_FALSE)
    {
        platformLogMessage("render: the graphics ceiling will not hold the text overlay");
        return;
    }
    if (hostUploadOverlay(pixels, widthInPixels, heightInPixels) == 0)
    {
        platformLogMessage("render: the host would not take the text overlay");
        return;
    }
    if (wantedBytes < overlayChargedBytes)
    {
        graphicsMemoryBudgetRelease(GRAPHICS_MEMORY_CATEGORY_TEXTURE,
                                    overlayChargedBytes - wantedBytes);
    }
    overlayChargedBytes = wantedBytes;
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
        Real32 angle = cameraStartAngle + (elapsedSeconds * cameraOrbitRate);

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

/* Re-sends the vertices of a mesh already uploaded. Charges the ledger nothing
   and builds no pipeline: both are already there, and the host replaces the
   buffer contents rather than adding to them. It also leaves the part ranges
   and part textures alone, which uploadMesh would not — see the note on
   hostUpdateMeshVertices for what that cost. */
void renderUpdateMeshVertices(const GeometryMesh *mesh, MemoryArena *arena)
{
    MemorySize marker;
    MemorySize vertexBytes;
    Real32 *interleaved;

    if (mesh == NULL_POINTER || !meshIsReady || mesh->positions == NULL_POINTER ||
        mesh->normals == NULL_POINTER || mesh->vertexCount != meshVertexCountUploaded)
    {
        return;
    }

    vertexBytes = (MemorySize)mesh->vertexCount * 8UL * sizeof(Real32);
    marker = memoryArenaGetMarker(arena);
    interleaved = (Real32 *)memoryArenaAllocate(arena, vertexBytes, sizeof(Real32));
    if (interleaved == NULL_POINTER)
    {
        memoryArenaRewindToMarker(arena, marker);
        return;
    }
    interleaveMeshVertices(mesh, interleaved);
    (void)hostUpdateMeshVertices(interleaved, mesh->vertexCount);
    memoryArenaRewindToMarker(arena, marker);
}

/* Charged per part, as the OpenGL ES backend charges them, so a device that
   cannot afford a Sim's three textures refuses the third rather than the
   driver refusing it later. */
static MemorySize partTextureChargedBytes[RENDER_PART_LIMIT];

void renderSetPartTexture(Unsigned32 partIndex, const Unsigned8 *rgbaPixels,
                          Unsigned32 widthInPixels, Unsigned32 heightInPixels)
{
    MemorySize wantedBytes;

    if (partIndex >= RENDER_PART_LIMIT)
    {
        return;
    }
    if (rgbaPixels == NULL_POINTER || widthInPixels == 0U || heightInPixels == 0U)
    {
        if (partTextureChargedBytes[partIndex] > 0UL)
        {
            graphicsMemoryBudgetRelease(GRAPHICS_MEMORY_CATEGORY_TEXTURE,
                                        partTextureChargedBytes[partIndex]);
            partTextureChargedBytes[partIndex] = 0UL;
        }
        return;
    }

    wantedBytes = (MemorySize)widthInPixels * (MemorySize)heightInPixels * 4UL;
    if (graphicsMemoryBudgetRequest(GRAPHICS_MEMORY_CATEGORY_TEXTURE, wantedBytes) == BOOLEAN_FALSE)
    {
        platformLogMessage("render: the graphics ceiling will not hold this part's texture");
        return;
    }
    if (hostUploadPartTexture(partIndex, rgbaPixels, widthInPixels, heightInPixels) == 0)
    {
        platformLogMessage("render: the host would not take this part's texture");
        graphicsMemoryBudgetRelease(GRAPHICS_MEMORY_CATEGORY_TEXTURE, wantedBytes);
        return;
    }
    /* Released only once the new one is in, so the ceiling cannot admit a
       texture it then turns out not to hold. */
    if (partTextureChargedBytes[partIndex] > 0UL)
    {
        graphicsMemoryBudgetRelease(GRAPHICS_MEMORY_CATEGORY_TEXTURE,
                                    partTextureChargedBytes[partIndex]);
    }
    partTextureChargedBytes[partIndex] = wantedBytes;
}
