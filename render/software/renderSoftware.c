#include "render/software/rasterizer.h"

#include "victoria/freestandingRuntime.h"
#include "victoria/graphicsMemoryBudget.h"
#include "victoria/platformInterface.h"
#include "victoria/profiler.h"
#include "victoria/renderInterface.h"
#include "victoria/softwareSurface.h"

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

    VICTORIA_PROFILE_ZONE_BEGIN("rasterizerDrawTriangle");
    rasterizerDrawTriangle(&surface, triangleVertices, colorPulse);
    VICTORIA_PROFILE_ZONE_END();

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
