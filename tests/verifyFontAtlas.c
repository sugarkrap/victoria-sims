
#include <stdio.h>

#include "utils/assert.h"
#include "victoria/fontAtlas.h"

static Integer32 failureCount = 0;

#define FILE_CAPACITY 8192U
static Unsigned8 fileBytes[FILE_CAPACITY];

#define SHEET_WIDTH 128U
#define SHEET_HEIGHT 128U
static Unsigned8 sheet[SHEET_WIDTH * SHEET_HEIGHT];
static Unsigned8 secondSheet[SHEET_WIDTH * SHEET_HEIGHT];
static Unsigned8 stored[SHEET_WIDTH * SHEET_HEIGHT * 2U];

static Integer32 pointX[256];
static Integer32 pointY[256];
static Unsigned8 pointIsOnCurve[256];
static Unsigned32 contourLastPoint[32];
static GlyphRasterEdge edges[1024];
static Integer32 crossingX[128];
static Integer32 crossingDirection[128];
static Unsigned16 rowCoverage[256];

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

static Unsigned32 inkIn(const FontAtlas *atlas, const FontAtlasGlyph *glyph)
{
    Unsigned32 total = 0U;
    Unsigned32 row;

    for (row = 0U; row < glyph->heightInPixels; row++)
    {
        Unsigned32 column;

        for (column = 0U; column < glyph->widthInPixels; column++)
        {
            total += atlas->sheet[(((MemorySize)glyph->sheetY + row) * atlas->sheetWidth) +
                                  glyph->sheetX + column];
        }
    }
    return total;
}

int main(void)
{
    FontAtlas atlas;
    FontReader font;
    FontGlyphOutline outline;
    GlyphRasterizer rasterizer;
    MemorySize fileSize;
    MemorySize storedSize;

    fontAtlasBind(&atlas, sheet, SHEET_WIDTH, SHEET_HEIGHT);
    checkThat(&failureCount, "the built-in font builds an atlas",
              fontAtlasBuildFromBuiltin(&atlas, 1U) && atlas.ready);
    checkThat(&failureCount, "with a line height that leaves room between lines",
              atlas.lineHeight > atlas.pixelSize);
    checkThat(&failureCount, "and every character in it", atlas.glyphsRefused == 0U);
    {
        const FontAtlasGlyph *letter = fontAtlasFind(&atlas, 'A');
        const FontAtlasGlyph *space = fontAtlasFind(&atlas, ' ');

        checkThat(&failureCount, "a letter has ink", letter != NULL_POINTER && inkIn(&atlas, letter) > 0U);
        checkThat(&failureCount, "a space has none but still advances",
                  space != NULL_POINTER && inkIn(&atlas, space) == 0U &&
                      space->advanceInPixels > 0U);
        checkThat(&failureCount, "a character outside the range is not in the atlas",
                  fontAtlasFind(&atlas, 0x00E9U) == NULL_POINTER &&
                      fontAtlasFind(&atlas, 9U) == NULL_POINTER);
    }
    checkThat(&failureCount, "and a line of text measures wider than one letter",
              fontAtlasMeasureLine(&atlas, "AB") > fontAtlasMeasureLine(&atlas, "A"));
    checkThat(&failureCount, "measuring stops at a newline",
              fontAtlasMeasureLine(&atlas, "A\nBBBB") == fontAtlasMeasureLine(&atlas, "A"));

    {
        FontAtlas unbound;

        fontAtlasBind(&unbound, NULL_POINTER, SHEET_WIDTH, SHEET_HEIGHT);
        checkThat(&failureCount, "an atlas with no sheet refuses to build",
                  !fontAtlasBuildFromBuiltin(&unbound, 1U));
        checkThat(&failureCount, "and finds nothing rather than crashing",
                  fontAtlasFind(&unbound, 'A') == NULL_POINTER &&
                      fontAtlasMeasureLine(&unbound, "AAAA") == 0U);
    }

    fileSize = loadFile("testAssets/fonts/fixture.mxf", fileBytes, FILE_CAPACITY);
    if (!checkThat(&failureCount, "the font fixture is where it should be",
                   fileSize > 0UL && fontReaderOpen(&font, fileBytes, fileSize)))
    {
        printf("  (run scripts/makeFontFixture.py)\n");
        return checkSummarize(failureCount, "font atlas");
    }

    glyphRasterizerBind(&rasterizer, edges, (Unsigned32)VICTORIA_ARRAY_LENGTH(edges), crossingX,
                        crossingDirection, (Unsigned32)VICTORIA_ARRAY_LENGTH(crossingX),
                        rowCoverage, (Unsigned32)VICTORIA_ARRAY_LENGTH(rowCoverage));
    fontOutlineBind(&outline, pointX, pointY, pointIsOnCurve, contourLastPoint, 256U, 32U);

    fontAtlasBind(&atlas, sheet, SHEET_WIDTH, SHEET_HEIGHT);
    checkThat(&failureCount, "an atlas builds from a real font",
              fontAtlasBuildFromFont(&atlas, &font, &rasterizer, &outline, 16U, 0x1234U) &&
                  atlas.ready);
    checkThat(&failureCount, "at the size it was asked for", atlas.pixelSize == 16U);
    checkThat(&failureCount, "with the metrics the font declares",
              atlas.baseline > 0U && atlas.lineHeight > atlas.baseline);
    checkThat(&failureCount, "and remembers what it was made from", atlas.sourceMark == 0x1234U);
    {
        const FontAtlasGlyph *letter = fontAtlasFind(&atlas, 'A');

        checkThat(&failureCount, "a glyph the font has is drawn",
                  letter != NULL_POINTER && letter->widthInPixels > 0U && inkIn(&atlas, letter) > 0U);
        checkThat(&failureCount, "and a character the font does not cover gets the missing glyph",
                  fontAtlasFind(&atlas, 'Z') != NULL_POINTER &&
                      inkIn(&atlas, fontAtlasFind(&atlas, 'Z')) > 0U);
    }
    checkThat(&failureCount, "glyphs are laid out without landing on each other",
              fontAtlasFind(&atlas, 'A')->sheetX != fontAtlasFind(&atlas, 'B')->sheetX ||
                  fontAtlasFind(&atlas, 'A')->sheetY != fontAtlasFind(&atlas, 'B')->sheetY);
    checkThat(&failureCount, "and only as far down the sheet as they reach",
              atlas.usedRows > 0U && atlas.usedRows <= SHEET_HEIGHT);

    {
        FontAtlas rebuilt;

        fontAtlasBind(&rebuilt, secondSheet, SHEET_WIDTH, SHEET_HEIGHT);
        (void)fontAtlasBuildFromBuiltin(&rebuilt, 1U);
        checkThat(&failureCount, "the built-in font gives a space a cell of its own",
                  fontAtlasFind(&rebuilt, ' ')->widthInPixels > 0U);
        (void)fontAtlasBuildFromFont(&rebuilt, &font, &rasterizer, &outline, 16U, 0x1234U);
        checkThat(&failureCount, "and building over it leaves the space with no pixels at all",
                  fontAtlasFind(&rebuilt, ' ')->widthInPixels == 0U &&
                      fontAtlasFind(&rebuilt, ' ')->heightInPixels == 0U);
        checkThat(&failureCount, "though it still advances the pen",
                  fontAtlasFind(&rebuilt, ' ')->advanceInPixels > 0U);
    }

    storedSize = fontAtlasStore(&atlas, stored, sizeof(stored));
    checkThat(&failureCount, "an atlas stores", storedSize > 0UL);
    checkThat(&failureCount, "and says beforehand how much room it needs",
              storedSize == fontAtlasStoredSize(&atlas));
    checkThat(&failureCount, "and does not store the empty part of the sheet",
              storedSize < (MemorySize)SHEET_WIDTH * (MemorySize)SHEET_HEIGHT);
    checkThat(&failureCount, "and refuses to write into somewhere too small",
              fontAtlasStore(&atlas, stored, storedSize - 1UL) == 0UL);

    {
        FontAtlas restored;
        Unsigned32 index;
        Boolean samePixels = BOOLEAN_TRUE;

        fontAtlasBind(&restored, secondSheet, SHEET_WIDTH, SHEET_HEIGHT);
        checkThat(&failureCount, "and restores",
                  fontAtlasRestore(&restored, stored, storedSize, 0x1234U) && restored.ready);
        checkThat(&failureCount, "with the metrics it went in with",
                  restored.pixelSize == atlas.pixelSize && restored.baseline == atlas.baseline &&
                      restored.lineHeight == atlas.lineHeight &&
                      restored.usedRows == atlas.usedRows);
        for (index = 0U; index < (Unsigned32)FONT_ATLAS_CHARACTER_COUNT; index++)
        {
            const FontAtlasGlyph *first = &atlas.glyphs[index];
            const FontAtlasGlyph *second = &restored.glyphs[index];

            if (first->sheetX != second->sheetX || first->sheetY != second->sheetY ||
                first->widthInPixels != second->widthInPixels ||
                first->heightInPixels != second->heightInPixels ||
                first->leftBearing != second->leftBearing ||
                first->topBearing != second->topBearing ||
                first->advanceInPixels != second->advanceInPixels)
            {
                samePixels = BOOLEAN_FALSE;
            }
        }
        checkThat(&failureCount, "and every glyph where it was", samePixels);

        samePixels = BOOLEAN_TRUE;
        for (index = 0U; index < atlas.usedRows * SHEET_WIDTH; index++)
        {
            if (sheet[index] != secondSheet[index])
            {
                samePixels = BOOLEAN_FALSE;
            }
        }
        checkThat(&failureCount, "and every pixel of it", samePixels);
    }

    {
        FontAtlas restored;
        Unsigned8 damaged[sizeof(stored)];
        MemorySize index;

        fontAtlasBind(&restored, secondSheet, SHEET_WIDTH, SHEET_HEIGHT);
        checkThat(&failureCount, "a block made from a different font is refused",
                  !fontAtlasRestore(&restored, stored, storedSize, 0x9999U) && !restored.ready);

        for (index = 0UL; index < storedSize; index++)
        {
            damaged[index] = stored[index];
        }
        damaged[0] = 0x00U;
        checkThat(&failureCount, "and one that is not an atlas at all",
                  !fontAtlasRestore(&restored, damaged, storedSize, 0x1234U));

        for (index = 0UL; index < storedSize; index++)
        {
            damaged[index] = stored[index];
        }
        damaged[storedSize / 2UL] = (Unsigned8)(damaged[storedSize / 2UL] ^ 0xFFU);
        checkThat(&failureCount, "and one with a byte changed in it",
                  !fontAtlasRestore(&restored, damaged, storedSize, 0x1234U));

        checkThat(&failureCount, "and one that stops short",
                  !fontAtlasRestore(&restored, stored, storedSize - 8UL, 0x1234U));

        {
            FontAtlas narrow;
            static Unsigned8 narrowSheet[64U * 64U];

            fontAtlasBind(&narrow, narrowSheet, 64U, 64U);
            checkThat(&failureCount, "and one written for a wider sheet than this build has",
                      !fontAtlasRestore(&narrow, stored, storedSize, 0x1234U));
        }
    }

    {
        FontAtlas cramped;
        static Unsigned8 tinySheet[32U * 16U];

        fontAtlasBind(&cramped, tinySheet, 32U, 16U);
        (void)fontAtlasBuildFromFont(&cramped, &font, &rasterizer, &outline, 16U, 0x55U);
        checkThat(&failureCount, "an atlas that runs out of sheet says how much it lost",
                  cramped.glyphsRefused > 0U);
    }

    return checkSummarize(failureCount, "font atlas");
}
