#include "victoria/glyphRaster.h"

/* Sample rows down each pixel row. Across a row the coverage is exact, so this
 * is the only place the picture is approximated at all.
 *
 * Four is where the returns stop for text. Two leaves visible steps on a
 * near-horizontal stroke — the top of an 'e', the crossbar of an 'A' — and
 * eight costs twice as much for a difference nobody has ever picked out of a
 * line-up. It divides 256 exactly, which is why a full pixel is worth 64 and
 * four of them come to 256 with nothing left over. */
#define SAMPLES_PER_ROW 4U
#define COVERAGE_PER_SAMPLE 64

/* Fractional bits in the coordinates everything here works in. */
#define SUBPIXEL_BITS 8
#define SUBPIXEL_ONE (1 << SUBPIXEL_BITS)

void glyphRasterizerBind(GlyphRasterizer *rasterizer, GlyphRasterEdge *edges,
                         Unsigned32 edgeCapacity, Integer32 *crossingX,
                         Integer32 *crossingDirection, Unsigned32 crossingCapacity,
                         Unsigned16 *rowCoverage, Unsigned32 rowCapacity)
{
    rasterizer->edges = edges;
    rasterizer->edgeCapacity = (edges != NULL_POINTER) ? edgeCapacity : 0U;
    rasterizer->edgeCount = 0U;
    rasterizer->crossingX = crossingX;
    rasterizer->crossingDirection = crossingDirection;
    rasterizer->crossingCapacity =
        (crossingX != NULL_POINTER && crossingDirection != NULL_POINTER) ? crossingCapacity : 0U;
    rasterizer->rowCoverage = rowCoverage;
    rasterizer->rowCapacity = (rowCoverage != NULL_POINTER) ? rowCapacity : 0U;
    rasterizer->refusedTooComplex = 0U;
}

Unsigned32 glyphRasterScaleFor(Unsigned32 unitsPerEm, Unsigned32 pixelSize)
{
    if (unitsPerEm == 0U)
    {
        return 0U;
    }
    /* Rounded, not truncated. Truncating here makes every glyph in the font a
       shade small, which nobody notices, and makes the advances a shade small,
       which shows up as a line of text that drifts left of where it should end. */
    return (Unsigned32)((((Unsigned64)pixelSize << 16) + (unitsPerEm / 2U)) / unitsPerEm);
}

/* A font unit into 24.8 bitmap space. The scale is 16.16 and a coordinate has
   eight fractional bits, so the product carries twenty-four too many. */
static Integer32 toSubpixels(Integer32 fontUnits, Unsigned32 scale)
{
    Integer64 product = (Integer64)fontUnits * (Integer64)scale;

    return (Integer32)(product >> (16 - SUBPIXEL_BITS));
}

static Integer32 floorToPixel(Integer32 subpixels)
{
    return (subpixels >= 0) ? (subpixels >> SUBPIXEL_BITS)
                            : -(((-subpixels) + SUBPIXEL_ONE - 1) >> SUBPIXEL_BITS);
}

static Integer32 ceilingToPixel(Integer32 subpixels)
{
    return -floorToPixel(-subpixels);
}

Boolean glyphRasterPlace(const FontGlyphOutline *outline, Unsigned32 scale,
                         GlyphPlacement *placement)
{
    Integer32 left;
    Integer32 right;
    Integer32 top;
    Integer32 bottom;

    placement->leftBearing = 0;
    placement->topBearing = 0;
    placement->widthInPixels = 0U;
    placement->heightInPixels = 0U;
    if (outline->pointCount == 0U || outline->contourCount == 0U || scale == 0U)
    {
        return BOOLEAN_FALSE;
    }

    left = floorToPixel(toSubpixels(outline->minimumX, scale));
    right = ceilingToPixel(toSubpixels(outline->maximumX, scale));
    bottom = floorToPixel(toSubpixels(outline->minimumY, scale));
    top = ceilingToPixel(toSubpixels(outline->maximumY, scale));
    if (right <= left || top <= bottom)
    {
        return BOOLEAN_FALSE;
    }

    placement->leftBearing = left;
    placement->topBearing = top;
    placement->widthInPixels = (Unsigned32)(right - left);
    placement->heightInPixels = (Unsigned32)(top - bottom);
    return BOOLEAN_TRUE;
}

/* Where the outline is being drawn, so every point can be turned into bitmap
 * coordinates by the same two subtractions. Held in a struct rather than passed
 * as four arguments through five functions. */
typedef struct RasterFrame
{
    Unsigned32 scale;
    Integer32 originX;
    Integer32 originY;
} RasterFrame;

static Integer32 frameX(const RasterFrame *frame, Integer32 fontX)
{
    return toSubpixels(fontX, frame->scale) - frame->originX;
}

/* Down the page rather than up it: a font's y grows towards the sky and a
 * bitmap's grows towards the floor. Doing the flip here, once, is why nothing
 * below has to remember which way it is looking. */
static Integer32 frameY(const RasterFrame *frame, Integer32 fontY)
{
    return frame->originY - toSubpixels(fontY, frame->scale);
}

static void addEdge(GlyphRasterizer *rasterizer, Integer32 firstX, Integer32 firstY,
                    Integer32 secondX, Integer32 secondY)
{
    GlyphRasterEdge *edge;

    /* A horizontal edge is crossed by no sample row, so it contributes nothing
       and taking it would only cost a division that answers infinity. */
    if (firstY == secondY)
    {
        return;
    }
    if (rasterizer->edgeCount >= rasterizer->edgeCapacity)
    {
        rasterizer->refusedTooComplex++;
        return;
    }
    edge = &rasterizer->edges[rasterizer->edgeCount];
    if (firstY < secondY)
    {
        edge->topX = firstX;
        edge->topY = firstY;
        edge->bottomX = secondX;
        edge->bottomY = secondY;
        edge->direction = 1;
    }
    else
    {
        edge->topX = secondX;
        edge->topY = secondY;
        edge->bottomX = firstX;
        edge->bottomY = firstY;
        edge->direction = -1;
    }
    rasterizer->edgeCount++;
}

static Integer32 absoluteOf(Integer32 value)
{
    return (value < 0) ? -value : value;
}

/* Splits a quadratic into straight pieces.
 *
 * How many is judged from the control polygon, which is longer than the curve
 * it describes and so always errs towards more pieces rather than fewer. One
 * piece per three pixels of it keeps the sagitta well under a sample; the floor
 * of two matters for a curve seen nearly edge-on, and the ceiling of twenty-four
 * stops a decorative swash at a large size from eating the edge list. */
static void addQuadratic(GlyphRasterizer *rasterizer, Integer32 startX, Integer32 startY,
                         Integer32 controlX, Integer32 controlY, Integer32 endX, Integer32 endY)
{
    Integer32 span = absoluteOf(controlX - startX) + absoluteOf(controlY - startY) +
                     absoluteOf(endX - controlX) + absoluteOf(endY - controlY);
    Integer32 pieces = (span >> SUBPIXEL_BITS) / 3;
    Integer32 previousX = startX;
    Integer32 previousY = startY;
    Integer32 step;

    if (pieces < 2)
    {
        pieces = 2;
    }
    if (pieces > 24)
    {
        pieces = 24;
    }
    for (step = 1; step <= pieces; step++)
    {
        /* De Casteljau written out as the polynomial, in 64 bits so the squared
           parameter cannot overflow at a large size. */
        Integer64 t = ((Integer64)step << SUBPIXEL_BITS) / pieces;
        Integer64 inverse = SUBPIXEL_ONE - t;
        Integer64 x = (inverse * inverse * startX) + (2 * inverse * t * controlX) +
                      (t * t * endX);
        Integer64 y = (inverse * inverse * startY) + (2 * inverse * t * controlY) +
                      (t * t * endY);
        Integer32 nextX = (Integer32)(x >> (2 * SUBPIXEL_BITS));
        Integer32 nextY = (Integer32)(y >> (2 * SUBPIXEL_BITS));

        addEdge(rasterizer, previousX, previousY, nextX, nextY);
        previousX = nextX;
        previousY = nextY;
    }
}

/* Walks one contour into edges.
 *
 * The format leaves out any on-curve point that happens to sit exactly halfway
 * between two control points, so a run of off-curve points is a run of curves
 * joined at midpoints that are not written down anywhere. Every reader has to
 * put them back, and a reader that does not draws a letter with its curves
 * pulled towards the wrong side. */
static void addContour(GlyphRasterizer *rasterizer, const FontGlyphOutline *outline,
                       const RasterFrame *frame, Unsigned32 firstPoint, Unsigned32 lastPoint)
{
    Unsigned32 count = lastPoint - firstPoint + 1U;
    Unsigned32 startIndex = 0U;
    Integer32 startX;
    Integer32 startY;
    Integer32 currentX;
    Integer32 currentY;
    Unsigned32 step;
    Boolean foundOnCurve = BOOLEAN_FALSE;

    if (count < 2U)
    {
        return;
    }
    for (step = 0U; step < count; step++)
    {
        if (outline->pointIsOnCurve[firstPoint + step])
        {
            startIndex = step;
            foundOnCurve = BOOLEAN_TRUE;
            break;
        }
    }
    if (foundOnCurve)
    {
        startX = frameX(frame, outline->pointX[firstPoint + startIndex]);
        startY = frameY(frame, outline->pointY[firstPoint + startIndex]);
    }
    else
    {
        /* A contour of nothing but control points — an 'o' drawn as four
           curves, which some fonts do. It begins halfway between the last and
           the first, and there is nowhere else it could begin.
         *
         * Starting from the LAST point rather than the first matters: the walk
         * below treats the starting index as already used up, and here the
         * first point is still a control point that has to be drawn through.
         * Starting at nought loses the first curve of every such contour, which
         * on an 'o' takes a quarter of it away. */
        startIndex = count - 1U;
        startX = (frameX(frame, outline->pointX[firstPoint]) +
                  frameX(frame, outline->pointX[lastPoint])) /
                 2;
        startY = (frameY(frame, outline->pointY[firstPoint]) +
                  frameY(frame, outline->pointY[lastPoint])) /
                 2;
    }
    currentX = startX;
    currentY = startY;

    for (step = 1U; step <= count; step++)
    {
        Unsigned32 index = firstPoint + ((startIndex + step) % count);
        Integer32 x = frameX(frame, outline->pointX[index]);
        Integer32 y = frameY(frame, outline->pointY[index]);

        if (outline->pointIsOnCurve[index])
        {
            addEdge(rasterizer, currentX, currentY, x, y);
            currentX = x;
            currentY = y;
            continue;
        }

        {
            Unsigned32 afterIndex = firstPoint + ((startIndex + step + 1U) % count);
            Integer32 endX;
            Integer32 endY;

            if (step == count)
            {
                /* The last point is a control point, so the curve closes on
                   where the contour began. */
                endX = startX;
                endY = startY;
            }
            else if (outline->pointIsOnCurve[afterIndex])
            {
                endX = frameX(frame, outline->pointX[afterIndex]);
                endY = frameY(frame, outline->pointY[afterIndex]);
                step++;
            }
            else
            {
                endX = (x + frameX(frame, outline->pointX[afterIndex])) / 2;
                endY = (y + frameY(frame, outline->pointY[afterIndex])) / 2;
            }
            addQuadratic(rasterizer, currentX, currentY, x, y, endX, endY);
            currentX = endX;
            currentY = endY;
        }
    }
    /* Closed whatever happened. An outline with a gap in it fills to the edge
       of the bitmap, which is unmistakable and is not what anybody wants. */
    addEdge(rasterizer, currentX, currentY, startX, startY);
}

/* Adds the coverage of one horizontal span to a row.
 *
 * The two end pixels get the fraction of themselves the span actually covers,
 * and everything between gets the lot. This is the whole of the horizontal
 * anti-aliasing and it is exact, which is why only four samples are needed the
 * other way. */
static void addSpan(Unsigned16 *row, Unsigned32 width, Integer32 fromX, Integer32 toX)
{
    Integer32 limit = (Integer32)width << SUBPIXEL_BITS;
    Unsigned32 first;
    Unsigned32 last;
    Unsigned32 pixel;

    if (fromX < 0)
    {
        fromX = 0;
    }
    if (toX > limit)
    {
        toX = limit;
    }
    if (toX <= fromX)
    {
        return;
    }
    first = (Unsigned32)(fromX >> SUBPIXEL_BITS);
    last = (Unsigned32)((toX - 1) >> SUBPIXEL_BITS);
    if (first == last)
    {
        row[first] = (Unsigned16)(row[first] +
                                  (Unsigned16)(((toX - fromX) * COVERAGE_PER_SAMPLE) >>
                                               SUBPIXEL_BITS));
        return;
    }
    row[first] = (Unsigned16)(row[first] +
                              (Unsigned16)(((((Integer32)(first + 1U) << SUBPIXEL_BITS) - fromX) *
                                            COVERAGE_PER_SAMPLE) >>
                                           SUBPIXEL_BITS));
    for (pixel = first + 1U; pixel < last; pixel++)
    {
        row[pixel] = (Unsigned16)(row[pixel] + COVERAGE_PER_SAMPLE);
    }
    row[last] = (Unsigned16)(row[last] +
                             (Unsigned16)(((toX - ((Integer32)last << SUBPIXEL_BITS)) *
                                           COVERAGE_PER_SAMPLE) >>
                                          SUBPIXEL_BITS));
}

/* Every place the edges meet one sample row, in order across the page.
 *
 * The half-open rule — a crossing counts at the top of an edge and not at the
 * bottom — is what keeps a point shared by two edges from being counted twice.
 * Counted twice it cancels itself out, and the fill stops at a vertex and
 * leaks out of the letter to the edge of the bitmap. */
static Unsigned32 gatherCrossings(GlyphRasterizer *rasterizer, Integer32 sampleY)
{
    Unsigned32 found = 0U;
    Unsigned32 index;

    for (index = 0U; index < rasterizer->edgeCount; index++)
    {
        const GlyphRasterEdge *edge = &rasterizer->edges[index];
        Integer32 x;

        if (sampleY < edge->topY || sampleY >= edge->bottomY)
        {
            continue;
        }
        if (found >= rasterizer->crossingCapacity)
        {
            rasterizer->refusedTooComplex++;
            break;
        }
        x = edge->topX + (Integer32)((((Integer64)(sampleY - edge->topY)) *
                                      (Integer64)(edge->bottomX - edge->topX)) /
                                     (Integer64)(edge->bottomY - edge->topY));

        /* Insertion sort on the way in. The list is nearly always under a dozen
           long and very often already in order, which is the one case where
           this beats everything cleverer. */
        {
            Unsigned32 position = found;

            while (position > 0U && rasterizer->crossingX[position - 1U] > x)
            {
                rasterizer->crossingX[position] = rasterizer->crossingX[position - 1U];
                rasterizer->crossingDirection[position] =
                    rasterizer->crossingDirection[position - 1U];
                position--;
            }
            rasterizer->crossingX[position] = x;
            rasterizer->crossingDirection[position] = edge->direction;
        }
        found++;
    }
    return found;
}

Boolean glyphRasterDraw(GlyphRasterizer *rasterizer, const FontGlyphOutline *outline,
                        Unsigned32 scale, const GlyphPlacement *placement, Unsigned8 *coverage,
                        Unsigned32 pitchInBytes)
{
    RasterFrame frame;
    Unsigned32 contour;
    Unsigned32 row;
    Unsigned32 firstPoint = 0U;

    if (rasterizer->edges == NULL_POINTER || coverage == NULL_POINTER ||
        placement->widthInPixels == 0U || placement->heightInPixels == 0U)
    {
        return BOOLEAN_FALSE;
    }
    if (placement->widthInPixels > rasterizer->rowCapacity)
    {
        rasterizer->refusedTooComplex++;
        return BOOLEAN_FALSE;
    }

    frame.scale = scale;
    /* Multiplied and not shifted: both of these are commonly negative, and
       shifting a negative left is not something C99 promises anything about. */
    frame.originX = placement->leftBearing * SUBPIXEL_ONE;
    frame.originY = placement->topBearing * SUBPIXEL_ONE;

    rasterizer->edgeCount = 0U;
    for (contour = 0U; contour < outline->contourCount; contour++)
    {
        Unsigned32 lastPoint = outline->contourLastPoint[contour];

        if (lastPoint >= outline->pointCount || lastPoint < firstPoint)
        {
            break;
        }
        addContour(rasterizer, outline, &frame, firstPoint, lastPoint);
        firstPoint = lastPoint + 1U;
    }
    if (rasterizer->edgeCount == 0U)
    {
        return BOOLEAN_FALSE;
    }

    for (row = 0U; row < placement->heightInPixels; row++)
    {
        Unsigned8 *destination = &coverage[(MemorySize)row * pitchInBytes];
        Unsigned32 pixel;
        Unsigned32 sample;

        for (pixel = 0U; pixel < placement->widthInPixels; pixel++)
        {
            rasterizer->rowCoverage[pixel] = 0U;
        }

        for (sample = 0U; sample < SAMPLES_PER_ROW; sample++)
        {
            /* Through the middle of each sample's own band rather than at its
               edge, so a stroke exactly one pixel high still catches a sample
               instead of falling between two of them. */
            Integer32 sampleY = (Integer32)((row * (Unsigned32)SUBPIXEL_ONE) +
                                            ((sample * 2U + 1U) * (Unsigned32)SUBPIXEL_ONE /
                                             (2U * SAMPLES_PER_ROW)));
            Unsigned32 crossingCount = gatherCrossings(rasterizer, sampleY);
            Integer32 winding = 0;
            Integer32 spanStart = 0;
            Unsigned32 index;

            for (index = 0U; index < crossingCount; index++)
            {
                Integer32 before = winding;

                winding += rasterizer->crossingDirection[index];
                if (before == 0 && winding != 0)
                {
                    spanStart = rasterizer->crossingX[index];
                }
                else if (before != 0 && winding == 0)
                {
                    addSpan(rasterizer->rowCoverage, placement->widthInPixels, spanStart,
                            rasterizer->crossingX[index]);
                }
            }
        }

        for (pixel = 0U; pixel < placement->widthInPixels; pixel++)
        {
            Unsigned32 value = rasterizer->rowCoverage[pixel];

            /* Four full samples come to 256, which is one more than a byte
               holds. Clamping rather than scaling keeps a solid interior solid
               instead of a shade under it. */
            destination[pixel] = (Unsigned8)((value > 255U) ? 255U : value);
        }
    }
    return BOOLEAN_TRUE;
}
