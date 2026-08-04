#ifndef VICTORIA_RASTERIZER_HEADER
#define VICTORIA_RASTERIZER_HEADER

#include "victoria/coreTypes.h"
#include "victoria/softwareSurface.h"

typedef struct RasterizerVertex
{
    /* Normalised device coordinates, matching what the shader backends take:
       -1..1 with positive Y upwards. */
    Real32 positionX;
    Real32 positionY;
    Real32 red;
    Real32 green;
    Real32 blue;
} RasterizerVertex;

void rasterizerClear(const SoftwareSurface *surface, Unsigned32 packedColor);
void rasterizerDrawTriangle(const SoftwareSurface *surface, const RasterizerVertex *vertices,
                            Real32 colorScale);

/* One horizontal run of a triangle, with colour interpolated across it. Split
   out because it is the whole inner loop, and the only thing worth
   vectorising. Both implementations must produce identical bytes.

   redAtStart and friends are the colour at pixelX == startX, in 0..255 fixed
   point scaled by 65536; the step values are the per-pixel increment in the
   same units. */
void rasterizerFillSpan(Unsigned32 *rowPixels, Unsigned32 startX, Unsigned32 endX,
                        Integer32 redAtStart, Integer32 greenAtStart, Integer32 blueAtStart,
                        Integer32 redStep, Integer32 greenStep, Integer32 blueStep);

/* Names whichever span implementation was compiled in, for the profiler
   report and for tests that need to know which one they measured. */
const char *rasterizerGetSpanImplementationName(void);

#endif
