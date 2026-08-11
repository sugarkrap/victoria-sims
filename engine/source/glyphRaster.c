#include "victoria/glyphRaster.h"

#define SAMPLES_PER_ROW 4U
#define COVERAGE_PER_SAMPLE 64

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
    return (Unsigned32)((((Unsigned64)pixelSize << 16) + (unitsPerEm / 2U)) / unitsPerEm);
}

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

static Integer32 frameY(const RasterFrame *frame, Integer32 fontY)
{
    return frame->originY - toSubpixels(fontY, frame->scale);
}

static void addEdge(GlyphRasterizer *rasterizer, Integer32 firstX, Integer32 firstY,
                    Integer32 secondX, Integer32 secondY)
{
    GlyphRasterEdge *edge;

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
    addEdge(rasterizer, currentX, currentY, startX, startY);
}

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

            destination[pixel] = (Unsigned8)((value > 255U) ? 255U : value);
        }
    }
    return BOOLEAN_TRUE;
}
