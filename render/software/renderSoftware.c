#include "render/software/rasterizer.h"

#include "victoria/freestandingRuntime.h"
#include "victoria/graphicsMemoryBudget.h"
#include "victoria/platformInterface.h"
#include "victoria/profiler.h"
#include "victoria/renderInterface.h"
#include "victoria/softwareSurface.h"

static Real32 cameraOrbitRate = RENDER_CAMERA_ORBIT_DEFAULT;
static Real32 cameraStartAngle = 0.0f;

void renderSetCameraOrbitRate(Real32 radiansPerSecond)
{
    cameraOrbitRate = radiansPerSecond;
}

void renderSetCameraAngle(Real32 radians)
{
    cameraStartAngle = radians;
}

static const RasterizerVertex triangleVertices[3] = {
    { 0.0f, 0.6f, 1.0f, 0.35f, 0.55f },
    { -0.6f, -0.5f, 0.35f, 0.75f, 1.0f },
    { 0.6f, -0.5f, 1.0f, 0.9f, 0.4f }
};

#define CLEAR_COLOR 0x0F1219U

static SoftwareSurface surface;
static Boolean surfaceIsReady = BOOLEAN_FALSE;

static const GeometryMesh *activeMesh = NULL_POINTER;
static Real32 *projectedPositions = NULL_POINTER;
static Unsigned32 *triangleOrder = NULL_POINTER;
static Unsigned32 *depthBinCounts = NULL_POINTER;
static Real32 meshCentre[3];
static Real32 meshRadius = 1.0f;

#define DEPTH_BIN_COUNT 2048U

static const Real32 lightDirection[3] = { -0.34f, -0.47f, 0.81f };

MemorySize renderQueryGraphicsMemoryBytes(void)
{
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

    surface.pixels = (Unsigned32 *)memoryArenaAllocate(arena, maximumPixelCount * sizeof(Unsigned32), 16UL);
    if (surface.pixels == NULL_POINTER)
    {
        platformLogMessage("renderer: no arena space for the software framebuffer");
        return BOOLEAN_FALSE;
    }

    surface.pitchInPixels = (Unsigned32)VICTORIA_SOFTWARE_MAXIMUM_WIDTH;
    resizeSurface(widthInPixels, heightInPixels);
    surfaceIsReady = BOOLEAN_TRUE;

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

static Unsigned32 scaleByAlpha(Unsigned32 value, Unsigned32 alpha)
{
    Unsigned32 product = (value * alpha) + 128U;

    return (product + (product >> 8)) >> 8;
}

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

static void drawPlaceholderTriangle(Real32 colorScale)
{
    RasterizerVertex correctedVertices[3];
    Real32 aspect = (Real32)surface.widthInPixels / (Real32)surface.heightInPixels;
    Unsigned32 index;

    for (index = 0U; index < 3U; index += 1U)
    {
        correctedVertices[index] = triangleVertices[index];
        correctedVertices[index].positionX = triangleVertices[index].positionX / aspect;
    }

    rasterizerDrawTriangle(&surface, correctedVertices, colorScale);
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
        drawPlaceholderTriangle(colorPulse);
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

void renderUpdateMeshVertices(const GeometryMesh *mesh, MemoryArena *arena)
{
    (void)mesh;
    (void)arena;
}

void renderSetPartTexture(Unsigned32 partIndex, const Unsigned8 *rgbaPixels,
                          Unsigned32 widthInPixels, Unsigned32 heightInPixels)
{
    (void)partIndex;
    (void)rgbaPixels;
    (void)widthInPixels;
    (void)heightInPixels;
}
