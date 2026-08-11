#include <stdio.h>

#include "utils/assert.h"
#include "victoria/tgaReader.h"

#define TGA_HEADER_SIZE 18UL
#define TGA_BUFFER_CAPACITY 256UL
#define RGBA_CAPACITY 256UL

#define DESCRIPTOR_BOTTOM_TO_TOP 0x00U
#define DESCRIPTOR_TOP_TO_BOTTOM 0x20U
#define DESCRIPTOR_RIGHT_TO_LEFT 0x10U

static Integer32 failureCount = 0;
static Unsigned8 tga[TGA_BUFFER_CAPACITY];
static Unsigned8 rgba[RGBA_CAPACITY];
static Unsigned8 rgbaFlipped[RGBA_CAPACITY];

static MemorySize writeHeader(Unsigned8 imageType, Unsigned16 width, Unsigned16 height,
                              Unsigned8 pixelDepth, Unsigned8 descriptor)
{
    MemorySize index;

    for (index = 0UL; index < TGA_HEADER_SIZE; index++)
    {
        tga[index] = 0U;
    }
    tga[2] = imageType;
    tga[12] = (Unsigned8)(width & 0xFFU);
    tga[13] = (Unsigned8)(width >> 8);
    tga[14] = (Unsigned8)(height & 0xFFU);
    tga[15] = (Unsigned8)(height >> 8);
    tga[16] = pixelDepth;
    tga[17] = descriptor;
    return TGA_HEADER_SIZE;
}

static void writePatternPixel(MemorySize cursor, Unsigned32 pixelIndex, Boolean withAlpha)
{
    tga[cursor] = (Unsigned8)(pixelIndex * 7U + 1U);
    tga[cursor + 1UL] = (Unsigned8)(pixelIndex * 11U + 2U);
    tga[cursor + 2UL] = (Unsigned8)(pixelIndex * 13U + 3U);
    if (withAlpha)
    {
        tga[cursor + 3UL] = (Unsigned8)(pixelIndex * 17U + 4U);
    }
}

static Boolean outputMatchesPattern(Unsigned32 width, Unsigned32 height, Unsigned8 descriptor,
                                    Boolean withAlpha)
{
    Unsigned32 pixelIndex;
    Unsigned32 pixelCount = width * height;

    for (pixelIndex = 0U; pixelIndex < pixelCount; pixelIndex++)
    {
        Unsigned32 fileRow = pixelIndex / width;
        Unsigned32 column = pixelIndex % width;
        Unsigned32 row = (descriptor & DESCRIPTOR_TOP_TO_BOTTOM) != 0U ? fileRow
                                                                       : (height - 1U - fileRow);
        Unsigned32 col = (descriptor & DESCRIPTOR_RIGHT_TO_LEFT) != 0U ? (width - 1U - column)
                                                                       : column;
        const Unsigned8 *pixel = rgba + ((MemorySize)row * width + col) * 4UL;
        Unsigned8 expectedAlpha = withAlpha ? (Unsigned8)(pixelIndex * 17U + 4U) : 255U;

        if (pixel[0] != (Unsigned8)(pixelIndex * 13U + 3U) ||
            pixel[1] != (Unsigned8)(pixelIndex * 11U + 2U) ||
            pixel[2] != (Unsigned8)(pixelIndex * 7U + 1U) || pixel[3] != expectedAlpha)
        {
            return BOOLEAN_FALSE;
        }
    }
    return BOOLEAN_TRUE;
}

static Boolean outputsMatchVerticallyMirrored(Unsigned32 width, Unsigned32 height)
{
    Unsigned32 pixelIndex;
    Unsigned32 pixelCount = width * height;
    Unsigned32 channel;

    for (pixelIndex = 0U; pixelIndex < pixelCount; pixelIndex++)
    {
        Unsigned32 row = pixelIndex / width;
        Unsigned32 column = pixelIndex % width;
        MemorySize first = ((MemorySize)row * width + column) * 4UL;
        MemorySize second = ((MemorySize)(height - 1U - row) * width + column) * 4UL;

        for (channel = 0U; channel < 4U; channel++)
        {
            if (rgba[first + channel] != rgbaFlipped[second + channel])
            {
                return BOOLEAN_FALSE;
            }
        }
    }
    return BOOLEAN_TRUE;
}

static Boolean outputsMatchHorizontallyMirrored(Unsigned32 width, Unsigned32 height)
{
    Unsigned32 pixelIndex;
    Unsigned32 pixelCount = width * height;
    Unsigned32 channel;

    for (pixelIndex = 0U; pixelIndex < pixelCount; pixelIndex++)
    {
        Unsigned32 row = pixelIndex / width;
        Unsigned32 column = pixelIndex % width;
        MemorySize first = ((MemorySize)row * width + column) * 4UL;
        MemorySize second = ((MemorySize)row * width + (width - 1U - column)) * 4UL;

        for (channel = 0U; channel < 4U; channel++)
        {
            if (rgba[first + channel] != rgbaFlipped[second + channel])
            {
                return BOOLEAN_FALSE;
            }
        }
    }
    return BOOLEAN_TRUE;
}

static MemorySize buildTruecolour24Fixture(Unsigned8 descriptor)
{
    MemorySize cursor = writeHeader(2U, 4U, 3U, 24U, descriptor);
    Unsigned32 pixelIndex;

    for (pixelIndex = 0U; pixelIndex < 12U; pixelIndex++)
    {
        writePatternPixel(cursor, pixelIndex, BOOLEAN_FALSE);
        cursor += 3UL;
    }
    return cursor;
}

int main(void)
{
    Unsigned32 width = 0U;
    Unsigned32 height = 0U;
    MemorySize tgaSize;
    TgaReadResult result;

    printf("-- 24-bit uncompressed, bottom-left origin --\n");
    {
        Unsigned32 decodedWidth = 0U;
        Unsigned32 decodedHeight = 0U;

        tgaSize = buildTruecolour24Fixture(DESCRIPTOR_BOTTOM_TO_TOP);
        result = tgaPeekDimensions(tga, tgaSize, &width, &height);
        checkThat(&failureCount, "peek reads OK", result == TGA_READ_OK);
        checkThat(&failureCount, "peek reports 4x3", width == 4U && height == 3U);

        result = tgaReadToRgba(tga, tgaSize, rgba, RGBA_CAPACITY, &decodedWidth, &decodedHeight);
        checkThat(&failureCount, "decode reads OK", result == TGA_READ_OK);
        checkThat(&failureCount, "decode dimensions agree with the peek",
                  decodedWidth == width && decodedHeight == height);
        checkThat(&failureCount, "every pixel matches the pattern, opaque",
                  outputMatchesPattern(4U, 3U, DESCRIPTOR_BOTTOM_TO_TOP, BOOLEAN_FALSE));
        checkThat(&failureCount, "the first file pixel lands in the bottom-left corner",
                  rgba[32] == 3U && rgba[33] == 2U && rgba[34] == 1U && rgba[35] == 255U);
    }

    printf("\n-- 32-bit uncompressed --\n");
    {
        MemorySize cursor = writeHeader(2U, 4U, 2U, 32U, DESCRIPTOR_BOTTOM_TO_TOP);
        Unsigned32 pixelIndex;

        for (pixelIndex = 0U; pixelIndex < 8U; pixelIndex++)
        {
            writePatternPixel(cursor, pixelIndex, BOOLEAN_TRUE);
            cursor += 4UL;
        }
        tgaSize = cursor;

        result = tgaReadToRgba(tga, tgaSize, rgba, RGBA_CAPACITY, &width, &height);
        checkThat(&failureCount, "decode reads OK", result == TGA_READ_OK);
        checkThat(&failureCount, "every pixel matches the pattern with its own alpha",
                  outputMatchesPattern(4U, 2U, DESCRIPTOR_BOTTOM_TO_TOP, BOOLEAN_TRUE));
        checkThat(&failureCount, "alpha is read per pixel, not invented",
                  rgba[19] == 4U && rgba[15] == (Unsigned8)(7U * 17U + 4U));
    }

    printf("\n-- 32-bit run-length encoded --\n");
    {
        MemorySize cursor = writeHeader(10U, 4U, 2U, 32U, DESCRIPTOR_BOTTOM_TO_TOP);
        Unsigned32 rawIndex;

        tga[cursor] = 0x84U;
        tga[cursor + 1UL] = 9U;
        tga[cursor + 2UL] = 8U;
        tga[cursor + 3UL] = 7U;
        tga[cursor + 4UL] = 6U;
        cursor += 5UL;
        tga[cursor] = 0x02U;
        cursor++;
        for (rawIndex = 0U; rawIndex < 3U; rawIndex++)
        {
            writePatternPixel(cursor, 5U + rawIndex, BOOLEAN_TRUE);
            cursor += 4UL;
        }
        tgaSize = cursor;

        result = tgaReadToRgba(tga, tgaSize, rgba, RGBA_CAPACITY, &width, &height);
        checkThat(&failureCount, "decode reads OK", result == TGA_READ_OK);
        checkThat(&failureCount, "the repeat packet fills five pixels with the one colour",
                  rgba[16] == 7U && rgba[17] == 8U && rgba[18] == 9U && rgba[19] == 6U &&
                      rgba[28] == 7U && rgba[0] == 7U && rgba[3] == 6U);
        checkThat(&failureCount, "the raw packet's pixels land after it, across the row boundary",
                  rgba[4] == (Unsigned8)(5U * 13U + 3U) && rgba[7] == (Unsigned8)(5U * 17U + 4U) &&
                      rgba[12] == (Unsigned8)(7U * 13U + 3U) && rgba[15] == (Unsigned8)(7U * 17U + 4U));
    }

    printf("\n-- origin flags --\n");
    {
        tgaSize = buildTruecolour24Fixture(DESCRIPTOR_BOTTOM_TO_TOP);
        result = tgaReadToRgba(tga, tgaSize, rgba, RGBA_CAPACITY, &width, &height);
        checkThat(&failureCount, "bottom-left reference decode reads OK", result == TGA_READ_OK);

        tgaSize = buildTruecolour24Fixture(DESCRIPTOR_TOP_TO_BOTTOM);
        result = tgaReadToRgba(tga, tgaSize, rgbaFlipped, RGBA_CAPACITY, &width, &height);
        checkThat(&failureCount, "top-left decode reads OK", result == TGA_READ_OK);
        checkThat(&failureCount, "the same bytes land mirrored top to bottom",
                  outputsMatchVerticallyMirrored(4U, 3U));
        checkThat(&failureCount, "the first file pixel lands in the top-left corner",
                  rgbaFlipped[0] == 3U && rgbaFlipped[1] == 2U && rgbaFlipped[2] == 1U);

        tgaSize = buildTruecolour24Fixture(DESCRIPTOR_RIGHT_TO_LEFT);
        result = tgaReadToRgba(tga, tgaSize, rgbaFlipped, RGBA_CAPACITY, &width, &height);
        checkThat(&failureCount, "bottom-right decode reads OK", result == TGA_READ_OK);
        checkThat(&failureCount, "the same bytes land mirrored left to right",
                  outputsMatchHorizontallyMirrored(4U, 3U));
        checkThat(&failureCount, "the first file pixel lands in the bottom-right corner",
                  rgbaFlipped[44] == 3U && rgbaFlipped[46] == 1U);
    }

    printf("\n-- refusing what it should --\n");
    {
        tgaSize = buildTruecolour24Fixture(DESCRIPTOR_BOTTOM_TO_TOP);
        checkThat(&failureCount, "pixel data cut one byte short is invalid, not guessed",
                  tgaReadToRgba(tga, tgaSize - 1UL, rgba, RGBA_CAPACITY, &width, &height) ==
                      TGA_READ_INVALID);
        checkThat(&failureCount, "a file shorter than its own header is invalid",
                  tgaPeekDimensions(tga, TGA_HEADER_SIZE - 1UL, &width, &height) == TGA_READ_INVALID);

        tgaSize = writeHeader(2U, 4U, 3U, 16U, DESCRIPTOR_BOTTOM_TO_TOP) + 24UL;
        checkThat(&failureCount, "16 bits per pixel is unsupported, not misread",
                  tgaReadToRgba(tga, tgaSize, rgba, RGBA_CAPACITY, &width, &height) ==
                      TGA_READ_UNSUPPORTED);

        tgaSize = writeHeader(1U, 4U, 3U, 24U, DESCRIPTOR_BOTTOM_TO_TOP) + 36UL;
        checkThat(&failureCount, "colour mapped images are unsupported, not misread",
                  tgaReadToRgba(tga, tgaSize, rgba, RGBA_CAPACITY, &width, &height) ==
                      TGA_READ_UNSUPPORTED);
    }

    return checkSummarize(failureCount, "TGA reader");
}
