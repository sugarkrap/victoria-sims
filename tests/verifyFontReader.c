/* Checks the font reader against a font we wrote ourselves.
 *
 * The game's fonts cannot appear here — nothing retail is ever committed — so
 * scripts/makeFontFixture.py writes one into testAssets/fonts/fixture.mxf, six
 * glyphs chosen so each catches a different way of reading TrueType wrongly.
 * Every expectation below is a number that script put there on purpose, which
 * is the only way a fixture is worth having: a fixture that agrees with the
 * reader by construction agrees with a broken reader too.
 *
 * The one thing the fixture cannot prove is that the reader works on the game's
 * own files, since it is authored to look like them rather than being one. What
 * it does prove is the part that would otherwise be a constant somebody read
 * off a disc and typed in: the mask and the offset are FOUND, and the last case
 * here hands the same font over with no mask at all to show that nothing is
 * hardcoded to nine and 0x9d. */

#include <stdio.h>

#include "utils/assert.h"
#include "victoria/fontReader.h"

static Integer32 failureCount = 0;

#define FILE_CAPACITY 8192U
static Unsigned8 fileBytes[FILE_CAPACITY];
static Unsigned8 plainBytes[FILE_CAPACITY];

/* Room for the fixture's busiest glyph several times over, so nothing here
   overflows by accident and the overflow case below is the only one that does. */
#define POINT_CAPACITY 64U
#define CONTOUR_CAPACITY 8U
static Integer32 pointX[POINT_CAPACITY];
static Integer32 pointY[POINT_CAPACITY];
static Unsigned8 pointIsOnCurve[POINT_CAPACITY];
static Unsigned32 contourLastPoint[CONTOUR_CAPACITY];

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

static void bindOutline(FontGlyphOutline *outline, Unsigned32 pointRoom, Unsigned32 contourRoom)
{
    fontOutlineBind(outline, pointX, pointY, pointIsOnCurve, contourLastPoint, pointRoom,
                    contourRoom);
}

int main(void)
{
    FontReader font;
    FontGlyphOutline outline;
    MemorySize fileSize;

    fileSize = loadFile("testAssets/fonts/fixture.mxf", fileBytes, FILE_CAPACITY);
    if (!checkThat(&failureCount, "the font fixture is where it should be", fileSize > 0UL))
    {
        printf("  (run scripts/makeFontFixture.py)\n");
        return checkSummarize(failureCount, "font reader");
    }

    checkThat(&failureCount, "the container opens", fontReaderOpen(&font, fileBytes, fileSize));

    /* Both of these are DERIVED. If either were a constant in the reader this
       would still pass, so the unmasked case at the bottom is what actually
       earns the claim. */
    checkThat(&failureCount, "and the payload was found past the header",
              font.payloadOffset == 9UL);
    checkThat(&failureCount, "and the mask with it", font.obfuscationKey == 0x9DU);

    checkThat(&failureCount, "the head table is read", font.unitsPerEm == 1024U);
    checkThat(&failureCount, "including which form the locations take",
              font.locationsAreLong == 1U);
    checkThat(&failureCount, "the glyph count comes from the maximum profile",
              font.glyphCount == 6U);
    checkThat(&failureCount, "and the vertical metrics from the horizontal header",
              font.ascender == 800 && font.descender == -200 && font.lineGap == 90);
    checkThat(&failureCount, "with a metric for every glyph", font.horizontalMetricCount == 6U);

    /* The character map. A reader can get format 4 nearly right in several
       ways, and each of them draws text that looks like text. */
    checkThat(&failureCount, "a character maps to its glyph", fontReaderFindGlyph(&font, 'A') == 2U);
    checkThat(&failureCount, "and so do the ones after it",
              fontReaderFindGlyph(&font, 'B') == 3U && fontReaderFindGlyph(&font, 'C') == 4U &&
                  fontReaderFindGlyph(&font, 'D') == 5U);
    checkThat(&failureCount, "a space is a glyph like any other",
              fontReaderFindGlyph(&font, ' ') == 1U);
    checkThat(&failureCount, "a character the font does not cover is the missing glyph",
              fontReaderFindGlyph(&font, 'Z') == 0U);
    checkThat(&failureCount, "and so is one past the end of the map entirely",
              fontReaderFindGlyph(&font, 0x4E00U) == 0U);

    checkThat(&failureCount, "advances come out of the metrics table",
              fontReaderGetAdvanceWidth(&font, 2U) == 600 &&
                  fontReaderGetAdvanceWidth(&font, 4U) == 800);
    checkThat(&failureCount, "and a glyph past the last metric repeats the last one",
              fontReaderGetAdvanceWidth(&font, 99U) == 300);

    /* A space. Not an error, and the difference matters: a reader that answered
       false here would drop every space in a line of text. */
    bindOutline(&outline, POINT_CAPACITY, CONTOUR_CAPACITY);
    checkThat(&failureCount, "a glyph with no outline still reads",
              fontReaderGetGlyphOutline(&font, 1U, &outline));
    checkThat(&failureCount, "and has nothing in it",
              outline.pointCount == 0U && outline.contourCount == 0U && !outline.overflowed);

    /* Sixteen-bit deltas, one contour. */
    bindOutline(&outline, POINT_CAPACITY, CONTOUR_CAPACITY);
    checkThat(&failureCount, "a simple glyph reads", fontReaderGetGlyphOutline(&font, 2U, &outline));
    checkThat(&failureCount, "with its points and its contour",
              outline.pointCount == 4U && outline.contourCount == 1U &&
                  outline.contourLastPoint[0] == 3U);
    checkThat(&failureCount, "and coordinates accumulated from the deltas",
              outline.pointX[0] == 100 && outline.pointY[0] == 100 && outline.pointX[1] == 500 &&
                  outline.pointY[2] == 500);
    checkThat(&failureCount, "and an extent taken from the points themselves",
              outline.minimumX == 100 && outline.minimumY == 100 && outline.maximumX == 500 &&
                  outline.maximumY == 500);
    checkThat(&failureCount, "every point on the curve",
              outline.pointIsOnCurve[0] && outline.pointIsOnCurve[1] && outline.pointIsOnCurve[2] &&
                  outline.pointIsOnCurve[3]);

    /* Two contours, the second wound against the first. A reader that lost the
       second would draw a solid box where the fixture means a ring, which is
       the same thing that happens when winding is ignored — so the count is
       checked here and the hole is checked by the rasterizer's own suite. */
    bindOutline(&outline, POINT_CAPACITY, CONTOUR_CAPACITY);
    (void)fontReaderGetGlyphOutline(&font, 0U, &outline);
    checkThat(&failureCount, "a glyph with a counter keeps both contours",
              outline.pointCount == 8U && outline.contourCount == 2U);
    checkThat(&failureCount, "and each contour knows where it ends",
              outline.contourLastPoint[0] == 3U && outline.contourLastPoint[1] == 7U);
    checkThat(&failureCount, "with the extent covering the outer one",
              outline.minimumX == 0 && outline.maximumX == 600);

    /* Short deltas, run-length flags and an off-curve point — the packing the
       other glyphs deliberately do not use. Getting the repeat count off by one
       reads every coordinate after it at the wrong offset, which is why the
       extent is checked rather than just the count. */
    bindOutline(&outline, POINT_CAPACITY, CONTOUR_CAPACITY);
    (void)fontReaderGetGlyphOutline(&font, 3U, &outline);
    checkThat(&failureCount, "short deltas and repeated flags read",
              outline.pointCount == 4U && outline.contourCount == 1U);
    checkThat(&failureCount, "with the coordinates they encode",
              outline.pointX[0] == 60 && outline.pointX[1] == 260 && outline.pointX[2] == 300 &&
                  outline.pointX[3] == 60);
    checkThat(&failureCount, "and the heights too, which is what a mis-sized x array loses",
              outline.pointY[0] == 60 && outline.pointY[1] == 60 && outline.pointY[2] == 300 &&
                  outline.pointY[3] == 460);
    checkThat(&failureCount, "the off-curve point is marked as one",
              outline.pointIsOnCurve[0] && outline.pointIsOnCurve[1] && !outline.pointIsOnCurve[2] &&
                  outline.pointIsOnCurve[3]);

    /* A composite. An accented letter is one of these, and a reader without
       them draws every accented character as nothing at all. */
    bindOutline(&outline, POINT_CAPACITY, CONTOUR_CAPACITY);
    (void)fontReaderGetGlyphOutline(&font, 4U, &outline);
    checkThat(&failureCount, "a composite gathers its component's points",
              outline.pointCount == 4U && outline.contourCount == 1U);
    checkThat(&failureCount, "moved by the offset the composite gives",
              outline.minimumX == 300 && outline.maximumX == 700 && outline.minimumY == 100 &&
                  outline.maximumY == 500);

    bindOutline(&outline, POINT_CAPACITY, CONTOUR_CAPACITY);
    (void)fontReaderGetGlyphOutline(&font, 5U, &outline);
    checkThat(&failureCount, "and a scaled composite is scaled",
              outline.minimumX == 50 && outline.maximumX == 250 && outline.minimumY == 50 &&
                  outline.maximumY == 250);

    /* Not enough room. Taking a prefix would be worse than taking nothing: the
       heights live after the widths in the file, so a walk that stopped early
       would find them at the wrong address and give the points that DID fit a
       plausible wrong y. */
    bindOutline(&outline, 2U, CONTOUR_CAPACITY);
    (void)fontReaderGetGlyphOutline(&font, 2U, &outline);
    checkThat(&failureCount, "a glyph with no room for it is refused whole",
              outline.overflowed && outline.pointCount == 0U);

    bindOutline(&outline, POINT_CAPACITY, CONTOUR_CAPACITY);
    checkThat(&failureCount, "a glyph past the end of the font is not read",
              !fontReaderGetGlyphOutline(&font, 6U, &outline));

    /* The claim that nothing is hardcoded, tested the only way it can be:
       hand the reader the same font with the mask taken off and the header
       gone, and watch it find offset nought and mask nought. */
    {
        FontReader plain;
        MemorySize index;

        for (index = 0UL; index + 9UL < fileSize; index++)
        {
            plainBytes[index] = (Unsigned8)(fileBytes[index + 9UL] ^ 0x9DU);
        }
        checkThat(&failureCount, "an unmasked font opens by the same route",
                  fontReaderOpen(&plain, plainBytes, fileSize - 9UL));
        checkThat(&failureCount, "with nothing in front of it and nothing over it",
                  plain.payloadOffset == 0UL && plain.obfuscationKey == 0U);
        checkThat(&failureCount, "and reads the same glyph out of it",
                  fontReaderFindGlyph(&plain, 'A') == 2U &&
                      fontReaderGetAdvanceWidth(&plain, 2U) == 600);
    }

    /* Bytes that are not a font at all. A reader that says yes here goes on to
       read a table directory out of whatever it was handed. */
    {
        FontReader rubbish;
        static Unsigned8 noise[256];
        MemorySize index;

        for (index = 0UL; index < sizeof(noise); index++)
        {
            noise[index] = (Unsigned8)(index * 7U);
        }
        checkThat(&failureCount, "noise is not a font",
                  !fontReaderOpen(&rubbish, noise, sizeof(noise)));
        checkThat(&failureCount, "nor is nothing at all",
                  !fontReaderOpen(&rubbish, NULL_POINTER, 0UL));
        checkThat(&failureCount, "and a font cut short is refused rather than walked off the end",
                  !fontReaderOpen(&rubbish, fileBytes, 64UL));
    }

    return checkSummarize(failureCount, "font reader");
}
