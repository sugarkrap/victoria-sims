#include "render/software/rasterizer.h"

#include "victoria/freestandingRuntime.h"
#include "victoria/graphicsMemoryBudget.h"
#include "victoria/platformInterface.h"
#include "victoria/profiler.h"
#include "victoria/renderInterface.h"
#include "victoria/softwareSurface.h"

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


/* Same geometry and colours as the shader backends, so the three can be
   compared frame to frame rather than taken on trust. */
static const RasterizerVertex triangleVertices[3] = {
    { 0.0f, 0.6f, 1.0f, 0.35f, 0.55f },
    { -0.6f, -0.5f, 0.35f, 0.75f, 1.0f },
    { 0.6f, -0.5f, 1.0f, 0.9f, 0.4f }
};

#define CLEAR_COLOR 0x0F1219U

static SoftwareSurface surface;
static Boolean surfaceIsReady = BOOLEAN_FALSE;

/* The mesh, if a disc gave us one, and the working storage for drawing it.
   Both come from the arena at the moment the mesh arrives. */
static const GeometryMesh *activeMesh = NULL_POINTER;
static Real32 *projectedPositions = NULL_POINTER;
static Unsigned32 *triangleOrder = NULL_POINTER;
static Unsigned32 *depthBinCounts = NULL_POINTER;
static Real32 meshCentre[3];
static Real32 meshRadius = 1.0f;

/* Triangles are drawn back to front by bucketing them on depth rather than
   sorting. There is no depth buffer here — adding one would put a test in the
   span filler, which is the one loop that has to stay identical between the
   plain and NEON paths — and a bucket pass is linear where a sort is not. Two
   triangles landing in the same bucket can come out in either order, which
   shows on a surface that folds back on itself and is invisible everywhere
   else. */
#define DEPTH_BIN_COUNT 2048U

/* Lit from over the viewer's shoulder, slightly to one side. There is no
   material yet, so this is what stops a mesh reading as a silhouette. */
static const Real32 lightDirection[3] = { -0.34f, -0.47f, 0.81f };

MemorySize renderQueryGraphicsMemoryBytes(void)
{
    /* There is no graphics memory here. The framebuffer is ordinary system
       memory and is charged to the arena, which is where it actually lives —
       counting it twice would make both ledgers wrong. */
    return 0UL;
}

const SoftwareSurface *renderSoftwareGetSurface(void)
{
    return &surface;
}

static void resizeSurface(Unsigned32 widthInPixels, Unsigned32 heightInPixels)
{
    if (widthInPixels > (Unsigned32)VICTORIA_SOFTWARE_MAXIMUM_WIDTH)
    {
        widthInPixels = (Unsigned32)VICTORIA_SOFTWARE_MAXIMUM_WIDTH;
    }
    if (heightInPixels > (Unsigned32)VICTORIA_SOFTWARE_MAXIMUM_HEIGHT)
    {
        heightInPixels = (Unsigned32)VICTORIA_SOFTWARE_MAXIMUM_HEIGHT;
    }
    if (widthInPixels == 0U)
    {
        widthInPixels = 1U;
    }
    if (heightInPixels == 0U)
    {
        heightInPixels = 1U;
    }

    surface.widthInPixels = widthInPixels;
    surface.heightInPixels = heightInPixels;
}

Boolean renderInitialize(MemoryArena *arena, Unsigned32 widthInPixels, Unsigned32 heightInPixels)
{
    const MemorySize maximumPixelCount =
        (MemorySize)VICTORIA_SOFTWARE_MAXIMUM_WIDTH * (MemorySize)VICTORIA_SOFTWARE_MAXIMUM_HEIGHT;

    /* Reserved once, at the largest size that will ever be needed. A resize
       past it clamps: there is no allocator to grow with, and pretending
       otherwise is how the no-allocation rule gets quietly broken. */
    surface.pixels = (Unsigned32 *)memoryArenaAllocate(arena, maximumPixelCount * sizeof(Unsigned32), 16UL);
    if (surface.pixels == NULL_POINTER)
    {
        platformLogMessage("renderer: no arena space for the software framebuffer");
        return BOOLEAN_FALSE;
    }

    surface.pitchInPixels = (Unsigned32)VICTORIA_SOFTWARE_MAXIMUM_WIDTH;
    resizeSurface(widthInPixels, heightInPixels);
    surfaceIsReady = BOOLEAN_TRUE;

    /* No shaders to build, so nothing to warm up. The zones still exist so the
       report has the same shape whichever backend is running. */
    VICTORIA_PROFILE_ZONE_BEGIN("renderCompileShaders");
    VICTORIA_PROFILE_ZONE_END();
    VICTORIA_PROFILE_ZONE_BEGIN("renderWarmUpShaders");
    rasterizerClear(&surface, CLEAR_COLOR);
    VICTORIA_PROFILE_ZONE_END();

    platformLogMessage("renderer: software backend ready");
    platformLogMessage(rasterizerGetSpanImplementationName());
    return BOOLEAN_TRUE;
}

void renderResize(Unsigned32 widthInPixels, Unsigned32 heightInPixels)
{
    if (surfaceIsReady == BOOLEAN_FALSE)
    {
        return;
    }
    resizeSurface(widthInPixels, heightInPixels);
}


void renderSetTexture(const Unsigned8 *rgbaPixels, Unsigned32 widthInPixels,
                      Unsigned32 heightInPixels)
{
    /* Accepted and ignored. This backend exists for hardware whose graphics
       predates programmable shaders, where the cost of sampling per pixel in
       software is the whole frame budget. Refusing to build would be worse
       than shading flat: the model still draws, and the ladder still holds. */
    (void)rgbaPixels;
    (void)widthInPixels;
    (void)heightInPixels;
}

void renderSetMesh(const GeometryMesh *mesh, MemoryArena *arena)
{
    Real32 minimum[3];
    Real32 maximum[3];
    Real32 extent;
    Unsigned32 axis;

    activeMesh = NULL_POINTER;
    if (mesh == NULL_POINTER || mesh->vertexCount == 0U || mesh->indexCount < 3U)
    {
        return;
    }

    projectedPositions = (Real32 *)memoryArenaAllocate(
        arena, (MemorySize)mesh->vertexCount * 3UL * sizeof(Real32), sizeof(Real32));
    triangleOrder = (Unsigned32 *)memoryArenaAllocate(
        arena, (MemorySize)(mesh->indexCount / 3U) * sizeof(Unsigned32), sizeof(Unsigned32));
    depthBinCounts = (Unsigned32 *)memoryArenaAllocate(
        arena, (MemorySize)(DEPTH_BIN_COUNT + 1U) * sizeof(Unsigned32), sizeof(Unsigned32));
    if (projectedPositions == NULL_POINTER || triangleOrder == NULL_POINTER ||
        depthBinCounts == NULL_POINTER)
    {
        platformLogMessage("render: not enough arena for the mesh, keeping the triangle");
        return;
    }

    geometryMeshGetBounds(mesh, minimum, maximum);
    meshRadius = 0.001f;
    for (axis = 0U; axis < 3U; axis++)
    {
        meshCentre[axis] = (minimum[axis] + maximum[axis]) * 0.5f;
        extent = (maximum[axis] - minimum[axis]) * 0.5f;
        if (extent > meshRadius)
        {
            meshRadius = extent;
        }
    }
    activeMesh = mesh;
}

/* Sims models are z-up, and the rasterizer wants normalised device coordinates
   with y up, so the axes are swapped on the way through. The camera orbits the
   model at a distance set by its own size, which is what lets a teapot and a
   Sim both arrive framed without anyone tuning a number. */
static void projectMesh(Real32 angleInRadians)
{
    Real32 sine = mathSine(angleInRadians);
    Real32 cosine = mathCosine(angleInRadians);
    Real32 distance = meshRadius * 3.2f;
    Real32 aspect = (Real32)surface.widthInPixels / (Real32)surface.heightInPixels;
    Unsigned32 index;

    for (index = 0U; index < activeMesh->vertexCount; index++)
    {
        const Real32 *source = &activeMesh->positions[(MemorySize)index * 3UL];
        Real32 *target = &projectedPositions[(MemorySize)index * 3UL];
        Real32 x = source[0] - meshCentre[0];
        Real32 y = source[1] - meshCentre[1];
        Real32 z = source[2] - meshCentre[2];
        Real32 rotatedX = (x * cosine) - (y * sine);
        Real32 rotatedY = (x * sine) + (y * cosine);
        Real32 depth = rotatedY + distance;

        if (depth < 0.01f)
        {
            depth = 0.01f;
        }
        target[0] = (rotatedX * 1.6f) / (depth * aspect);
        target[1] = (z * 1.6f) / depth;
        target[2] = depth;
    }
}

/* Counting sort on depth: one pass to count, one to place. */
static Unsigned32 orderTrianglesBackToFront(void)
{
    Unsigned32 triangleCount = activeMesh->indexCount / 3U;
    Unsigned32 kept = 0U;
    Real32 nearest = 0.0f;
    Real32 farthest = 0.0f;
    Real32 scale;
    Unsigned32 index;
    Boolean haveRange = BOOLEAN_FALSE;

    for (index = 0U; index <= DEPTH_BIN_COUNT; index++)
    {
        depthBinCounts[index] = 0U;
    }

    for (index = 0U; index < triangleCount; index++)
    {
        const Unsigned16 *face = &activeMesh->indices[(MemorySize)index * 3UL];
        const Real32 *first = &projectedPositions[(MemorySize)face[0] * 3UL];
        const Real32 *second = &projectedPositions[(MemorySize)face[1] * 3UL];
        const Real32 *third = &projectedPositions[(MemorySize)face[2] * 3UL];
        Real32 area = ((second[0] - first[0]) * (third[1] - first[1])) -
                      ((third[0] - first[0]) * (second[1] - first[1]));
        Real32 depth;

        /* Facing away from the camera. Culling here rather than in the
           rasterizer keeps the far side of a closed model from being drawn over
           the near side, which is what stands in for a depth buffer. */
        if (area <= 0.0f)
        {
            continue;
        }
        depth = (first[2] + second[2] + third[2]) * (1.0f / 3.0f);
        if (!haveRange)
        {
            nearest = depth;
            farthest = depth;
            haveRange = BOOLEAN_TRUE;
        }
        if (depth < nearest)
        {
            nearest = depth;
        }
        if (depth > farthest)
        {
            farthest = depth;
        }
        kept++;
    }

    if (kept == 0U)
    {
        return 0U;
    }
    scale = (farthest > nearest) ? ((Real32)(DEPTH_BIN_COUNT - 1U) / (farthest - nearest)) : 0.0f;

    for (index = 0U; index < triangleCount; index++)
    {
        const Unsigned16 *face = &activeMesh->indices[(MemorySize)index * 3UL];
        const Real32 *first = &projectedPositions[(MemorySize)face[0] * 3UL];
        const Real32 *second = &projectedPositions[(MemorySize)face[1] * 3UL];
        const Real32 *third = &projectedPositions[(MemorySize)face[2] * 3UL];
        Real32 area = ((second[0] - first[0]) * (third[1] - first[1])) -
                      ((third[0] - first[0]) * (second[1] - first[1]));
        Real32 depth;
        Unsigned32 bin;

        if (area <= 0.0f)
        {
            continue;
        }
        depth = (first[2] + second[2] + third[2]) * (1.0f / 3.0f);
        /* Farthest first, so the near side is drawn last and wins. */
        bin = (Unsigned32)((farthest - depth) * scale);
        if (bin >= DEPTH_BIN_COUNT)
        {
            bin = DEPTH_BIN_COUNT - 1U;
        }
        depthBinCounts[bin + 1U]++;
    }
    for (index = 1U; index <= DEPTH_BIN_COUNT; index++)
    {
        depthBinCounts[index] += depthBinCounts[index - 1U];
    }

    for (index = 0U; index < triangleCount; index++)
    {
        const Unsigned16 *face = &activeMesh->indices[(MemorySize)index * 3UL];
        const Real32 *first = &projectedPositions[(MemorySize)face[0] * 3UL];
        const Real32 *second = &projectedPositions[(MemorySize)face[1] * 3UL];
        const Real32 *third = &projectedPositions[(MemorySize)face[2] * 3UL];
        Real32 area = ((second[0] - first[0]) * (third[1] - first[1])) -
                      ((third[0] - first[0]) * (second[1] - first[1]));
        Real32 depth;
        Unsigned32 bin;

        if (area <= 0.0f)
        {
            continue;
        }
        depth = (first[2] + second[2] + third[2]) * (1.0f / 3.0f);
        bin = (Unsigned32)((farthest - depth) * scale);
        if (bin >= DEPTH_BIN_COUNT)
        {
            bin = DEPTH_BIN_COUNT - 1U;
        }
        triangleOrder[depthBinCounts[bin]] = index;
        depthBinCounts[bin]++;
    }
    return kept;
}

/* Flat shaded from the face normal in model space, so the shading turns with
   the model rather than being painted on. */
static Real32 shadeFace(const Unsigned16 *face)
{
    const Real32 *first = &activeMesh->positions[(MemorySize)face[0] * 3UL];
    const Real32 *second = &activeMesh->positions[(MemorySize)face[1] * 3UL];
    const Real32 *third = &activeMesh->positions[(MemorySize)face[2] * 3UL];
    Real32 firstEdge[3];
    Real32 secondEdge[3];
    Real32 normal[3];
    Real32 lengthSquared;
    Real32 lambert;
    Unsigned32 axis;

    for (axis = 0U; axis < 3U; axis++)
    {
        firstEdge[axis] = second[axis] - first[axis];
        secondEdge[axis] = third[axis] - first[axis];
    }
    normal[0] = (firstEdge[1] * secondEdge[2]) - (firstEdge[2] * secondEdge[1]);
    normal[1] = (firstEdge[2] * secondEdge[0]) - (firstEdge[0] * secondEdge[2]);
    normal[2] = (firstEdge[0] * secondEdge[1]) - (firstEdge[1] * secondEdge[0]);

    lengthSquared = (normal[0] * normal[0]) + (normal[1] * normal[1]) + (normal[2] * normal[2]);
    if (lengthSquared <= 0.0f)
    {
        return 0.35f;
    }
    /* This used to run four Newton steps from an estimate of one, inline. That
       is accurate for a cross product near unit length and wrong by a factor of
       sixty for one a thousandth of that — which is what a Sim's triangles
       make, the model being under two units across and carrying eighteen
       hundred vertices. Every normal came out too short, every lambert came out
       near nought, and the whole body shaded at the 0.28 floor: a Sim in
       silhouette. The teapot's triangles are ten times larger and were fine,
       which is why it stood for as long as it did. */
    {
        Real32 length = mathSquareRoot(lengthSquared);

        if (length <= 0.0f)
        {
            return 0.35f;
        }
        for (axis = 0U; axis < 3U; axis++)
        {
            normal[axis] /= length;
        }
    }

    lambert = (normal[0] * lightDirection[0]) + (normal[1] * lightDirection[1]) +
              (normal[2] * lightDirection[2]);
    if (lambert < 0.0f)
    {
        lambert = -lambert;
    }
    return 0.28f + (0.72f * lambert);
}

static void drawMesh(Real32 elapsedSeconds)
{
    Unsigned32 kept;
    Unsigned32 position;

    projectMesh(cameraStartAngle + (elapsedSeconds * cameraOrbitRate));
    kept = orderTrianglesBackToFront();

    for (position = 0U; position < kept; position++)
    {
        Unsigned32 triangle = triangleOrder[position];
        const Unsigned16 *face = &activeMesh->indices[(MemorySize)triangle * 3UL];
        Real32 shade = shadeFace(face);
        RasterizerVertex vertices[3];
        Unsigned32 corner;

        for (corner = 0U; corner < 3U; corner++)
        {
            const Real32 *projected = &projectedPositions[(MemorySize)face[corner] * 3UL];

            vertices[corner].positionX = projected[0];
            vertices[corner].positionY = projected[1];
            vertices[corner].red = shade;
            vertices[corner].green = shade * 0.93f;
            vertices[corner].blue = shade * 0.84f;
        }
        rasterizerDrawTriangle(&surface, vertices, 1.0f);
    }
}

/* The overlay, kept by pointer. It lives in an arena and outlives the frame, so
   there is nothing to copy and nothing to upload — which is the one place this
   backend has an easier job than the two that talk to a device. */
static const Unsigned8 *overlayPixels = NULL_POINTER;
static Unsigned32 overlayWidth = 0U;
static Unsigned32 overlayHeight = 0U;

void renderSetOverlay(const Unsigned8 *pixels, Unsigned32 widthInPixels,
                      Unsigned32 heightInPixels)
{
    if (pixels == NULL_POINTER || widthInPixels == 0U || heightInPixels == 0U)
    {
        overlayPixels = NULL_POINTER;
        overlayWidth = 0U;
        overlayHeight = 0U;
        return;
    }
    overlayPixels = pixels;
    overlayWidth = widthInPixels;
    overlayHeight = heightInPixels;
}

/* value * alpha / 255, without the divide.
 *
 * Dividing by 255 costs more than everything else in this loop put together on
 * the hardware this backend exists for. Adding the top byte back before
 * shifting is the standard way round it, and is exact for every one of the
 * 65536 pairs rather than approximately right. */
static Unsigned32 scaleByAlpha(Unsigned32 value, Unsigned32 alpha)
{
    Unsigned32 product = (value * alpha) + 128U;

    return (product + (product >> 8)) >> 8;
}

/* The overlay over the scene, premultiplied: out = source + scene * (1 - a).
 *
 * The same arithmetic the two shader backends get from a blend mode, written
 * out. One pass over the pixels, because a walk of the framebuffer is the
 * expensive thing here and doing it twice would double the only cost. */
static void drawOverlay(void)
{
    Unsigned32 rows = (overlayHeight < surface.heightInPixels) ? overlayHeight
                                                              : surface.heightInPixels;
    Unsigned32 columns = (overlayWidth < surface.widthInPixels) ? overlayWidth
                                                                : surface.widthInPixels;
    Unsigned32 row;

    for (row = 0U; row < rows; row++)
    {
        const Unsigned8 *source = &overlayPixels[(MemorySize)row * overlayWidth * 4UL];
        Unsigned32 *destination = &surface.pixels[(MemorySize)row * surface.pitchInPixels];
        Unsigned32 column;

        for (column = 0U; column < columns; column++)
        {
            const Unsigned8 *pixel = &source[column * 4U];
            Unsigned32 alpha = pixel[3];
            Unsigned32 behind;
            Unsigned32 keep;

            /* Nothing here at all: the common case by a long way, since an
               interface is mostly the space around it. */
            if (alpha == 0U)
            {
                continue;
            }
            if (alpha == 255U)
            {
                destination[column] = ((Unsigned32)pixel[0] << 16) |
                                      ((Unsigned32)pixel[1] << 8) | (Unsigned32)pixel[2];
                continue;
            }
            behind = destination[column];
            keep = 255U - alpha;
            destination[column] =
                ((pixel[0] + scaleByAlpha((behind >> 16) & 0xFFU, keep)) << 16) |
                ((pixel[1] + scaleByAlpha((behind >> 8) & 0xFFU, keep)) << 8) |
                (pixel[2] + scaleByAlpha(behind & 0xFFU, keep));
        }
    }
}

void renderDrawFrame(Real32 elapsedSeconds)
{
    Real32 colorPulse = 0.65f + (0.35f * mathSine(elapsedSeconds * 1.5f));

    if (surfaceIsReady == BOOLEAN_FALSE)
    {
        return;
    }

    VICTORIA_PROFILE_ZONE_BEGIN("renderDrawFrame");

    VICTORIA_PROFILE_ZONE_BEGIN("rasterizerClear");
    rasterizerClear(&surface, CLEAR_COLOR);
    VICTORIA_PROFILE_ZONE_END();

    if (activeMesh != NULL_POINTER)
    {
        VICTORIA_PROFILE_ZONE_BEGIN("drawMesh");
        drawMesh(elapsedSeconds);
        VICTORIA_PROFILE_ZONE_END();
    }
    else
    {
        VICTORIA_PROFILE_ZONE_BEGIN("rasterizerDrawTriangle");
        rasterizerDrawTriangle(&surface, triangleVertices, colorPulse);
        VICTORIA_PROFILE_ZONE_END();
    }

    if (overlayPixels != NULL_POINTER)
    {
        VICTORIA_PROFILE_ZONE_BEGIN("drawOverlay");
        drawOverlay();
        VICTORIA_PROFILE_ZONE_END();
    }

    VICTORIA_PROFILE_ZONE_END();
}

void renderShutdown(void)
{
    surfaceIsReady = BOOLEAN_FALSE;
}

Unsigned32 renderGetShaderProgramCount(void)
{
    return 0U;
}

/* Nothing to do: this backend keeps a pointer to the mesh rather than a copy of
   it, so vertices moved in place are already what the next frame rasterizes.
   The camera is deliberately not re-framed — that would zoom the model about as
   an animation moved it. */
/* Nothing to do, and it is worth saying why rather than leaving a bare no-op.
 *
 * This backend keeps the caller's GeometryMesh by pointer and reads its
 * positions afresh every frame — projectMesh and shadeFace both work straight
 * off activeMesh. The engine poses by rewriting those positions in place, so
 * the new pose is already here by the time this is called. The two backends
 * that upload to a device need telling; this one does not.
 *
 * That makes it correct by a coupling rather than by construction. An engine
 * that ever posed into a buffer of its own would freeze this backend at the
 * bind pose and say nothing, so the coupling is written down here. */
void renderUpdateMeshVertices(const GeometryMesh *mesh, MemoryArena *arena)
{
    (void)mesh;
    (void)arena;
}

void renderSetPartTexture(Unsigned32 partIndex, const Unsigned8 *rgbaPixels,
                          Unsigned32 widthInPixels, Unsigned32 heightInPixels)
{
    /* Accepted and ignored, for the same reason renderSetTexture is: this
       backend shades flat because the hardware at the bottom of the ladder
       cannot afford to sample per pixel. A part's own texture changes nothing
       when no texture is sampled at all. */
    (void)partIndex;
    (void)rgbaPixels;
    (void)widthInPixels;
    (void)heightInPixels;
}
