#ifndef VICTORIA_RASTERIZER_HEADER
#define VICTORIA_RASTERIZER_HEADER

#include "victoria/coreTypes.h"
#include "victoria/softwareSurface.h"

typedef struct RasterizerVertex
{
    Real32 positionX;
    Real32 positionY;
    Real32 red;
    Real32 green;
    Real32 blue;
} RasterizerVertex;

void rasterizerClear(const SoftwareSurface *surface, Unsigned32 packedColor);
void rasterizerDrawTriangle(const SoftwareSurface *surface, const RasterizerVertex *vertices,
                            Real32 colorScale);

void rasterizerFillSpan(Unsigned32 *rowPixels, Unsigned32 startX, Unsigned32 endX,
                        Integer32 redAtStart, Integer32 greenAtStart, Integer32 blueAtStart,
                        Integer32 redStep, Integer32 greenStep, Integer32 blueStep);

const char *rasterizerGetSpanImplementationName(void);

#endif
