#ifndef VICTORIA_GLYPH_RASTER_HEADER
#define VICTORIA_GLYPH_RASTER_HEADER

#include "victoria/coreTypes.h"
#include "victoria/fontReader.h"

typedef struct GlyphRasterEdge
{
    Integer32 topX;
    Integer32 topY;
    Integer32 bottomX;
    Integer32 bottomY;
    Integer32 direction;
} GlyphRasterEdge;

typedef struct GlyphRasterizer
{
    GlyphRasterEdge *edges;
    Unsigned32 edgeCapacity;
    Unsigned32 edgeCount;

    Integer32 *crossingX;
    Integer32 *crossingDirection;
    Unsigned32 crossingCapacity;

    Unsigned16 *rowCoverage;
    Unsigned32 rowCapacity;

    Unsigned32 refusedTooComplex;
} GlyphRasterizer;

void glyphRasterizerBind(GlyphRasterizer *rasterizer, GlyphRasterEdge *edges,
                         Unsigned32 edgeCapacity, Integer32 *crossingX,
                         Integer32 *crossingDirection, Unsigned32 crossingCapacity,
                         Unsigned16 *rowCoverage, Unsigned32 rowCapacity);

typedef struct GlyphPlacement
{
    Integer32 leftBearing;
    Integer32 topBearing;
    Unsigned32 widthInPixels;
    Unsigned32 heightInPixels;
} GlyphPlacement;

Unsigned32 glyphRasterScaleFor(Unsigned32 unitsPerEm, Unsigned32 pixelSize);

Boolean glyphRasterPlace(const FontGlyphOutline *outline, Unsigned32 scale,
                         GlyphPlacement *placement);

Boolean glyphRasterDraw(GlyphRasterizer *rasterizer, const FontGlyphOutline *outline,
                        Unsigned32 scale, const GlyphPlacement *placement, Unsigned8 *coverage,
                        Unsigned32 pitchInBytes);

#endif
