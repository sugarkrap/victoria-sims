#ifndef VICTORIA_GLYPH_RASTER_HEADER
#define VICTORIA_GLYPH_RASTER_HEADER

#include "victoria/coreTypes.h"
#include "victoria/fontReader.h"

/* Turns an outline into pixels, in integers, all the way down.
 *
 * There is no floating point here and there will not be. The floor of the
 * device ladder is an ARMv5TE with no unit for it, where every float is a
 * function call into a software library, and a glyph is tens of thousands of
 * arithmetic operations. Fixed point is not a stylistic preference in this file
 * — it is the difference between a font that appears and one that does not.
 *
 * Coordinates are 24.8: eight fractional bits, which at any size a user
 * interface uses puts the error a long way below a pixel. The scale that gets
 * outlines into that space is 16.16, because a font's units per em is commonly
 * 2048 and a pixel size commonly 11, and 16.16 keeps that ratio to five decimal
 * places where 8.8 would round it to two.
 *
 * Hinting is not run. A TrueType font carries a small program per glyph whose
 * job is to move points onto the pixel grid at small sizes, and running it
 * means implementing a stack machine with its own instruction set. What is done
 * instead is to anti-alias: four samples down each pixel row and exact coverage
 * across it. Unhinted-and-smoothed is how every renderer written in the last
 * fifteen years draws text, and it is what the outlines were drawn for.
 *
 * The cost is real and is paid exactly once. See fontAtlas.h — on a machine
 * with a disc the pixels are written there and read back on every later run, so
 * the slowest device in the ladder rasterizes a font one time and never again.
 *
 * Nothing here allocates. Every array it works in is bound by the caller. */

/* One line segment of a flattened outline, in 24.8 bitmap coordinates.
 *
 * Held with its ends in the order they were walked plus which way that was,
 * rather than sorted so the first is uppermost, because the direction is what
 * says whether a crossing adds to the winding or takes away from it — and that
 * is what puts the hole in an 'o'. */
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

    /* Where a sample row meets the edges, gathered and then sorted. A glyph
       needs as many of these as its widest row has crossings, which for text is
       single figures and for a decorative capital is dozens. */
    Integer32 *crossingX;
    Integer32 *crossingDirection;
    Unsigned32 crossingCapacity;

    /* One pixel row's worth of coverage, accumulated across the sample rows
       before being written out. Sixteen bits because four samples of a full
       pixel is 256, which does not fit in eight. */
    Unsigned16 *rowCoverage;
    Unsigned32 rowCapacity;

    /* Glyphs that would not fit in the arrays above, and glyphs whose extent
       would not fit the bitmap. Counted rather than merely refused, because a
       font that silently drops its widest letters looks like a font with a
       missing letter and not like a rasterizer that ran out of room. */
    Unsigned32 refusedTooComplex;
} GlyphRasterizer;

void glyphRasterizerBind(GlyphRasterizer *rasterizer, GlyphRasterEdge *edges,
                         Unsigned32 edgeCapacity, Integer32 *crossingX,
                         Integer32 *crossingDirection, Unsigned32 crossingCapacity,
                         Unsigned16 *rowCoverage, Unsigned32 rowCapacity);

/* Where a glyph's pixels sit and how far the pen moves afterwards. Everything
 * here is in whole pixels, because that is what a caller blitting it needs.
 *
 * leftBearing is how far right of the pen the leftmost column is, and is
 * commonly negative for a letter that leans back over the one before it.
 * topBearing is how far ABOVE the baseline the top row is, so a caller draws at
 * (penX + leftBearing, baselineY - topBearing) and never has to think about
 * which way the two coordinate systems point. */
typedef struct GlyphPlacement
{
    Integer32 leftBearing;
    Integer32 topBearing;
    Unsigned32 widthInPixels;
    Unsigned32 heightInPixels;
} GlyphPlacement;

/* Pixels per font unit, in 16.16. Nought when the font has no units per em,
   which cannot happen for a font that opened. */
Unsigned32 glyphRasterScaleFor(Unsigned32 unitsPerEm, Unsigned32 pixelSize);

/* How big this outline comes out at this scale, without drawing it. Returns
   false for an outline with nothing in it — a space — which places at nought by
   nought and is not an error. */
Boolean glyphRasterPlace(const FontGlyphOutline *outline, Unsigned32 scale,
                         GlyphPlacement *placement);

/* Draws the outline into coverage, one byte a pixel, nought for uncovered and
 * 255 for wholly covered. The caller supplies the storage and its pitch, and
 * the placement it got from glyphRasterPlace — passing a different one draws
 * the glyph in the wrong place rather than failing, which is why the two are
 * separate calls and the placement is an input here.
 *
 * The bytes under the glyph are overwritten in full, so a caller reusing one
 * buffer does not have to clear it. */
Boolean glyphRasterDraw(GlyphRasterizer *rasterizer, const FontGlyphOutline *outline,
                        Unsigned32 scale, const GlyphPlacement *placement, Unsigned8 *coverage,
                        Unsigned32 pitchInBytes);

#endif
