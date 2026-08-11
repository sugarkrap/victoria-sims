
#include <stdio.h>

#include "utils/assert.h"
#include "victoria/glyphRaster.h"

static Integer32 failureCount = 0;

#define FILE_CAPACITY 8192U
static Unsigned8 fileBytes[FILE_CAPACITY];

static Integer32 pointX[128];
static Integer32 pointY[128];
static Unsigned8 pointIsOnCurve[128];
static Unsigned32 contourLastPoint[16];

static GlyphRasterEdge edges[512];
static Integer32 crossingX[64];
static Integer32 crossingDirection[64];
static Unsigned16 rowCoverage[128];

#define BITMAP_PITCH 64U
static Unsigned8 bitmap[BITMAP_PITCH * 64U];
static Unsigned8 secondBitmap[BITMAP_PITCH * 64U];

static MemorySize loadFile(const char *path, Unsigned8 *destination, MemorySize capacity)
{
    FILE *handle = fopen(path, "rb");
    MemorySize read;

    if (handle == NULL)
    {
        return 0UL;
    }
    read = (MemorySize)fread(destination, 1, (size_t)capacity, handle);
    (void)fclose(handle);
    return read;
}

static void bindAll(GlyphRasterizer *rasterizer, FontGlyphOutline *outline)
{
    glyphRasterizerBind(rasterizer, edges, (Unsigned32)VICTORIA_ARRAY_LENGTH(edges), crossingX,
                        crossingDirection, (Unsigned32)VICTORIA_ARRAY_LENGTH(crossingX),
                        rowCoverage, (Unsigned32)VICTORIA_ARRAY_LENGTH(rowCoverage));
    fontOutlineBind(outline, pointX, pointY, pointIsOnCurve, contourLastPoint, 128U, 16U);
}

static Unsigned8 pixelAt(Unsigned32 column, Unsigned32 row)
{
    return bitmap[((MemorySize)row * BITMAP_PITCH) + column];
}

int main(void)
{
    FontReader font;
    FontGlyphOutline outline;
    GlyphRasterizer rasterizer;
    GlyphPlacement placement;
    MemorySize fileSize;
    Unsigned32 scale;

    fileSize = loadFile("testAssets/fonts/fixture.mxf", fileBytes, FILE_CAPACITY);
    if (!checkThat(&failureCount, "the font fixture is where it should be", fileSize > 0UL))
    {
        printf("  (run scripts/makeFontFixture.py)\n");
        return checkSummarize(failureCount, "glyph rasterizer");
    }
    if (!checkThat(&failureCount, "and it opens", fontReaderOpen(&font, fileBytes, fileSize)))
    {
        return checkSummarize(failureCount, "glyph rasterizer");
    }

    scale = glyphRasterScaleFor(font.unitsPerEm, 20U);
    checkThat(&failureCount, "the scale is pixels per font unit in 16.16", scale == 1280U);
    checkThat(&failureCount, "and a font with no em is refused rather than dividing by it",
              glyphRasterScaleFor(0U, 20U) == 0U);

    bindAll(&rasterizer, &outline);
    (void)fontReaderGetGlyphOutline(&font, 2U, &outline);
    checkThat(&failureCount, "a square places", glyphRasterPlace(&outline, scale, &placement));
    checkThat(&failureCount, "at the pixel its outline reaches",
              placement.leftBearing == 1 && placement.topBearing == 10);
    checkThat(&failureCount, "over as many pixels as it covers",
              placement.widthInPixels == 9U && placement.heightInPixels == 9U);

    checkThat(&failureCount, "and draws",
              glyphRasterDraw(&rasterizer, &outline, scale, &placement, bitmap, BITMAP_PITCH));
    checkThat(&failureCount, "with a solid interior", pixelAt(4U, 4U) == 255U);
    checkThat(&failureCount, "an edge pixel part covered rather than all or nothing",
              pixelAt(8U, 4U) > 150U && pixelAt(8U, 4U) < 240U);
    checkThat(&failureCount, "and a pixel the shape barely enters barely covered",
              pixelAt(0U, 4U) < 40U);
    checkThat(&failureCount, "nothing was refused", rasterizer.refusedTooComplex == 0U);

    {
        Unsigned32 row;
        Unsigned32 solidRows = 0U;

        bindAll(&rasterizer, &outline);
        (void)fontReaderGetGlyphOutline(&font, 3U, &outline);
        (void)glyphRasterPlace(&outline, scale, &placement);
        (void)glyphRasterDraw(&rasterizer, &outline, scale, &placement, bitmap, BITMAP_PITCH);
        for (row = 0U; row < placement.heightInPixels; row++)
        {
            if (pixelAt(3U, row) > 128U)
            {
                solidRows++;
            }
        }
        checkThat(&failureCount, "a glyph with a top and a bottom has both",
                  solidRows > 2U && solidRows < placement.heightInPixels);
        checkThat(&failureCount, "and is the right way up",
                  pixelAt(1U, placement.heightInPixels - 2U) > pixelAt(4U, 1U));
    }

    bindAll(&rasterizer, &outline);
    (void)fontReaderGetGlyphOutline(&font, 0U, &outline);
    checkThat(&failureCount, "a ring places over its outer contour",
              glyphRasterPlace(&outline, scale, &placement) && placement.widthInPixels == 12U &&
                  placement.heightInPixels == 12U);
    checkThat(&failureCount, "and draws",
              glyphRasterDraw(&rasterizer, &outline, scale, &placement, bitmap, BITMAP_PITCH));
    checkThat(&failureCount, "its band is solid", pixelAt(0U, 6U) == 255U);
    checkThat(&failureCount, "and its hole is empty",
              pixelAt(5U, 6U) == 0U && pixelAt(6U, 5U) == 0U && pixelAt(6U, 6U) == 0U);
    checkThat(&failureCount, "with the band on the far side solid too", pixelAt(11U, 6U) > 128U);

    {
        MemorySize index;
        Boolean identical = BOOLEAN_TRUE;

        for (index = 0UL; index < sizeof(bitmap); index++)
        {
            bitmap[index] = 0xAAU;
        }
        (void)glyphRasterDraw(&rasterizer, &outline, scale, &placement, bitmap, BITMAP_PITCH);
        checkThat(&failureCount, "drawing over old pixels leaves none of them",
                  pixelAt(5U, 6U) == 0U);

        for (index = 0UL; index < sizeof(secondBitmap); index++)
        {
            secondBitmap[index] = 0U;
        }
        (void)glyphRasterDraw(&rasterizer, &outline, scale, &placement, secondBitmap,
                              BITMAP_PITCH);
        for (index = 0UL; index < (MemorySize)placement.heightInPixels * BITMAP_PITCH; index++)
        {
            if (index % BITMAP_PITCH < placement.widthInPixels &&
                bitmap[index] != secondBitmap[index])
            {
                identical = BOOLEAN_FALSE;
            }
        }
        checkThat(&failureCount, "and the same glyph twice is the same pixels", identical);
    }

    bindAll(&rasterizer, &outline);
    (void)fontReaderGetGlyphOutline(&font, 1U, &outline);
    checkThat(&failureCount, "a glyph with no outline does not place",
              !glyphRasterPlace(&outline, scale, &placement) && placement.widthInPixels == 0U);

    {
        GlyphRasterizer cramped;

        glyphRasterizerBind(&cramped, edges, (Unsigned32)VICTORIA_ARRAY_LENGTH(edges), crossingX,
                            crossingDirection, (Unsigned32)VICTORIA_ARRAY_LENGTH(crossingX),
                            rowCoverage, 4U);
        fontOutlineBind(&outline, pointX, pointY, pointIsOnCurve, contourLastPoint, 128U, 16U);
        (void)fontReaderGetGlyphOutline(&font, 2U, &outline);
        (void)glyphRasterPlace(&outline, scale, &placement);
        checkThat(&failureCount, "a glyph wider than the working row is refused",
                  !glyphRasterDraw(&cramped, &outline, scale, &placement, bitmap, BITMAP_PITCH) &&
                      cramped.refusedTooComplex == 1U);
    }
    {
        GlyphRasterizer cramped;

        glyphRasterizerBind(&cramped, edges, 2U, crossingX, crossingDirection,
                            (Unsigned32)VICTORIA_ARRAY_LENGTH(crossingX), rowCoverage,
                            (Unsigned32)VICTORIA_ARRAY_LENGTH(rowCoverage));
        (void)fontReaderGetGlyphOutline(&font, 3U, &outline);
        (void)glyphRasterPlace(&outline, scale, &placement);
        (void)glyphRasterDraw(&cramped, &outline, scale, &placement, bitmap, BITMAP_PITCH);
        checkThat(&failureCount, "and a glyph with more edges than there is room for is counted",
                  cramped.refusedTooComplex > 0U);
    }
    {
        GlyphRasterizer unbound;

        glyphRasterizerBind(&unbound, NULL_POINTER, 8U, NULL_POINTER, NULL_POINTER, 8U,
                            NULL_POINTER, 8U);
        checkThat(&failureCount, "a rasterizer bound to nothing draws nothing rather than crashing",
                  !glyphRasterDraw(&unbound, &outline, scale, &placement, bitmap, BITMAP_PITCH));
    }

    return checkSummarize(failureCount, "glyph rasterizer");
}
