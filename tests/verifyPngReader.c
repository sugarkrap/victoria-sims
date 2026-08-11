#include <stdio.h>

#include "utils/assert.h"
#include "victoria/pngReader.h"

#define FIXTURE_WIDTH 8U
#define FIXTURE_HEIGHT 8U
#define RGBA_CAPACITY ((MemorySize)FIXTURE_WIDTH * FIXTURE_HEIGHT * 4UL)
#define SCRATCH_CAPACITY 512UL

static Integer32 failureCount = 0;
static Unsigned8 rgba[RGBA_CAPACITY];
static Unsigned8 scratch[SCRATCH_CAPACITY];
static Unsigned8 corrupted[256UL];

static const Unsigned8 rgbaPngFixture[125UL] = {
    0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU, 0x00U, 0x00U, 0x00U, 0x0DU,
    0x49U, 0x48U, 0x44U, 0x52U, 0x00U, 0x00U, 0x00U, 0x08U, 0x00U, 0x00U, 0x00U, 0x08U,
    0x08U, 0x06U, 0x00U, 0x00U, 0x00U, 0xC4U, 0x0FU, 0xBEU, 0x8BU, 0x00U, 0x00U, 0x00U,
    0x44U, 0x49U, 0x44U, 0x41U, 0x54U, 0x78U, 0x9CU, 0x63U, 0x64U, 0xE5U, 0x62U, 0x3FU,
    0x21U, 0xC7U, 0x20U, 0xF2U, 0x1FU, 0x17U, 0x66U, 0x61U, 0x90U, 0x13U, 0xF9U, 0xC1U,
    0xC0U, 0xF0U, 0xE6U, 0x3FU, 0x03U, 0x83U, 0xC6U, 0x7FU, 0x6CU, 0x34U, 0x54U, 0x81U,
    0xC6U, 0x7FU, 0x06U, 0x86U, 0x23U, 0xFFU, 0x19U, 0x18U, 0x44U, 0xA0U, 0x18U, 0xC1U,
    0x47U, 0x32U, 0x41U, 0xE4U, 0x3FU, 0x36U, 0x1AU, 0xC9U, 0x04U, 0x98U, 0xEEU, 0x12U,
    0x24U, 0x36U, 0x56U, 0x37U, 0x88U, 0x50U, 0xD9U, 0x0DU, 0x00U, 0xCEU, 0x0DU, 0x57U,
    0x84U, 0xD3U, 0xF1U, 0xECU, 0xE5U, 0x00U, 0x00U, 0x00U, 0x00U, 0x49U, 0x45U, 0x4EU,
    0x44U, 0xAEU, 0x42U, 0x60U, 0x82U,
};

static const Unsigned8 truecolourPngFixture[114UL] = {
    0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU, 0x00U, 0x00U, 0x00U, 0x0DU,
    0x49U, 0x48U, 0x44U, 0x52U, 0x00U, 0x00U, 0x00U, 0x08U, 0x00U, 0x00U, 0x00U, 0x08U,
    0x08U, 0x02U, 0x00U, 0x00U, 0x00U, 0x4BU, 0x6DU, 0x29U, 0xDCU, 0x00U, 0x00U, 0x00U,
    0x39U, 0x49U, 0x44U, 0x41U, 0x54U, 0x78U, 0x9CU, 0x63U, 0x64U, 0xE5U, 0x62U, 0x97U,
    0x63U, 0x10U, 0xC1U, 0x44U, 0x2CU, 0x0CU, 0x72U, 0x22U, 0x0CU, 0x0CU, 0x6FU, 0x18U,
    0x18U, 0x34U, 0xD0U, 0x48U, 0x88U, 0x84U, 0x06U, 0x03U, 0xC3U, 0x11U, 0x06U, 0x06U,
    0x11U, 0x06U, 0x06U, 0x04U, 0x1BU, 0xAEU, 0x03U, 0x9DU, 0x84U, 0xEBU, 0x80U, 0x28U,
    0x2FU, 0x81U, 0x31U, 0xD0U, 0xEDU, 0x10U, 0xA1U, 0xC0U, 0x0EU, 0x00U, 0xBEU, 0x38U,
    0x18U, 0x2CU, 0x23U, 0xDFU, 0x40U, 0x44U, 0x00U, 0x00U, 0x00U, 0x00U, 0x49U, 0x45U,
    0x4EU, 0x44U, 0xAEU, 0x42U, 0x60U, 0x82U,
};

static const Unsigned8 palettePngFixture[100UL] = {
    0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU, 0x00U, 0x00U, 0x00U, 0x0DU,
    0x49U, 0x48U, 0x44U, 0x52U, 0x00U, 0x00U, 0x00U, 0x08U, 0x00U, 0x00U, 0x00U, 0x08U,
    0x08U, 0x03U, 0x00U, 0x00U, 0x00U, 0xF3U, 0xD1U, 0x4EU, 0xB9U, 0x00U, 0x00U, 0x00U,
    0x09U, 0x50U, 0x4CU, 0x54U, 0x45U, 0xFAU, 0x0AU, 0x14U, 0x1EU, 0xF0U, 0x28U, 0x32U,
    0x3CU, 0xE6U, 0x8BU, 0xF1U, 0xBFU, 0xFAU, 0x00U, 0x00U, 0x00U, 0x16U, 0x49U, 0x44U,
    0x41U, 0x54U, 0x78U, 0xDAU, 0x63U, 0x60U, 0x60U, 0x64U, 0x02U, 0x23U, 0x18U, 0xCDU,
    0xC4U, 0x00U, 0xA3U, 0x19U, 0x48U, 0x91U, 0x02U, 0x00U, 0x09U, 0x2AU, 0x00U, 0x41U,
    0x9FU, 0xFDU, 0x73U, 0x25U, 0x00U, 0x00U, 0x00U, 0x00U, 0x49U, 0x45U, 0x4EU, 0x44U,
    0xAEU, 0x42U, 0x60U, 0x82U,
};

static const Unsigned8 greyscalePngFixture[75UL] = {
    0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU, 0x00U, 0x00U, 0x00U, 0x0DU,
    0x49U, 0x48U, 0x44U, 0x52U, 0x00U, 0x00U, 0x00U, 0x08U, 0x00U, 0x00U, 0x00U, 0x08U,
    0x08U, 0x00U, 0x00U, 0x00U, 0x00U, 0xE1U, 0x64U, 0xE1U, 0x57U, 0x00U, 0x00U, 0x00U,
    0x12U, 0x49U, 0x44U, 0x41U, 0x54U, 0x78U, 0x9CU, 0x63U, 0x64U, 0x10U, 0x80U, 0x00U,
    0x26U, 0x16U, 0x28U, 0x20U, 0x8FU, 0x01U, 0x00U, 0x3BU, 0x68U, 0x01U, 0x60U, 0x31U,
    0xC5U, 0xD2U, 0x7AU, 0x00U, 0x00U, 0x00U, 0x00U, 0x49U, 0x45U, 0x4EU, 0x44U, 0xAEU,
    0x42U, 0x60U, 0x82U,
};

static const Unsigned8 sixteenBitPngFixture[45UL] = {
    0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU, 0x00U, 0x00U, 0x00U, 0x0DU,
    0x49U, 0x48U, 0x44U, 0x52U, 0x00U, 0x00U, 0x00U, 0x08U, 0x00U, 0x00U, 0x00U, 0x08U,
    0x10U, 0x02U, 0x00U, 0x00U, 0x00U, 0x1BU, 0xFDU, 0xF5U, 0x9FU, 0x00U, 0x00U, 0x00U,
    0x00U, 0x49U, 0x45U, 0x4EU, 0x44U, 0xAEU, 0x42U, 0x60U, 0x82U,
};

static const Unsigned8 interlacedPngFixture[45UL] = {
    0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU, 0x00U, 0x00U, 0x00U, 0x0DU,
    0x49U, 0x48U, 0x44U, 0x52U, 0x00U, 0x00U, 0x00U, 0x08U, 0x00U, 0x00U, 0x00U, 0x08U,
    0x08U, 0x06U, 0x00U, 0x00U, 0x01U, 0xB3U, 0x08U, 0x8EU, 0x1DU, 0x00U, 0x00U, 0x00U,
    0x00U, 0x49U, 0x45U, 0x4EU, 0x44U, 0xAEU, 0x42U, 0x60U, 0x82U,
};

static Boolean patternPixelsMatch(Boolean withAlpha)
{
    Unsigned32 x;
    Unsigned32 y;

    for (y = 0U; y < FIXTURE_HEIGHT; y++)
    {
        for (x = 0U; x < FIXTURE_WIDTH; x++)
        {
            const Unsigned8 *pixel = rgba + ((MemorySize)y * FIXTURE_WIDTH + x) * 4UL;
            Unsigned8 expectedAlpha = withAlpha ? (Unsigned8)(200U - (x + y * 8U)) : 255U;

            if (pixel[0] != (Unsigned8)(x * 30U + 5U) || pixel[1] != (Unsigned8)(y * 30U + 10U) ||
                pixel[2] != (Unsigned8)((x ^ y) * 20U + 7U) || pixel[3] != expectedAlpha)
            {
                return BOOLEAN_FALSE;
            }
        }
    }
    return BOOLEAN_TRUE;
}

static Boolean palettePixelsMatch(void)
{
    static const Unsigned8 palette[3U][3U] = {
        { 250U, 10U, 20U }, { 30U, 240U, 40U }, { 50U, 60U, 230U }
    };
    Unsigned32 x;
    Unsigned32 y;

    for (y = 0U; y < FIXTURE_HEIGHT; y++)
    {
        for (x = 0U; x < FIXTURE_WIDTH; x++)
        {
            const Unsigned8 *pixel = rgba + ((MemorySize)y * FIXTURE_WIDTH + x) * 4UL;
            Unsigned32 index = (x + y) % 3U;

            if (pixel[0] != palette[index][0] || pixel[1] != palette[index][1] ||
                pixel[2] != palette[index][2] || pixel[3] != 255U)
            {
                return BOOLEAN_FALSE;
            }
        }
    }
    return BOOLEAN_TRUE;
}

static Boolean greyscalePixelsMatch(void)
{
    Unsigned32 x;
    Unsigned32 y;

    for (y = 0U; y < FIXTURE_HEIGHT; y++)
    {
        for (x = 0U; x < FIXTURE_WIDTH; x++)
        {
            const Unsigned8 *pixel = rgba + ((MemorySize)y * FIXTURE_WIDTH + x) * 4UL;
            Unsigned8 level = (Unsigned8)(x * 16U + y * 4U);

            if (pixel[0] != level || pixel[1] != level || pixel[2] != level || pixel[3] != 255U)
            {
                return BOOLEAN_FALSE;
            }
        }
    }
    return BOOLEAN_TRUE;
}

static MemorySize buildCorruptedIdatCrc(void)
{
    MemorySize index;
    MemorySize cursor = 8UL;

    for (index = 0UL; index < sizeof(rgbaPngFixture); index++)
    {
        corrupted[index] = rgbaPngFixture[index];
    }
    while (cursor + 12UL <= sizeof(rgbaPngFixture))
    {
        MemorySize chunkLength = ((MemorySize)corrupted[cursor] << 24) |
                                 ((MemorySize)corrupted[cursor + 1UL] << 16) |
                                 ((MemorySize)corrupted[cursor + 2UL] << 8) |
                                 (MemorySize)corrupted[cursor + 3UL];

        if (corrupted[cursor + 4UL] == 'I' && corrupted[cursor + 5UL] == 'D' &&
            corrupted[cursor + 6UL] == 'A' && corrupted[cursor + 7UL] == 'T')
        {
            corrupted[cursor + 8UL + chunkLength] ^= 0xFFU;
            return sizeof(rgbaPngFixture);
        }
        cursor += 8UL + chunkLength + 4UL;
    }
    return 0UL;
}

int main(void)
{
    Unsigned32 width = 0U;
    Unsigned32 height = 0U;
    MemorySize scratchNeeded = 0UL;
    PngReadResult result;

    printf("-- truecolour with alpha (colour type 6) --\n");
    {
        result = pngPeekDimensions(rgbaPngFixture, sizeof(rgbaPngFixture), &width, &height,
                                   &scratchNeeded);
        checkThat(&failureCount, "peek reads OK", result == PNG_READ_OK);
        checkThat(&failureCount, "peek reports 8x8", width == FIXTURE_WIDTH && height == FIXTURE_HEIGHT);
        checkThat(&failureCount, "peek sizes the scratch exactly",
                  scratchNeeded == (MemorySize)FIXTURE_HEIGHT * (FIXTURE_WIDTH * 4UL + 1UL));

        result = pngReadToRgba(rgbaPngFixture, sizeof(rgbaPngFixture), rgba, RGBA_CAPACITY,
                               scratch, scratchNeeded, &width, &height);
        checkThat(&failureCount, "decode reads OK with the peeked scratch size", result == PNG_READ_OK);
        checkThat(&failureCount, "decode dimensions agree with the peek",
                  width == FIXTURE_WIDTH && height == FIXTURE_HEIGHT);
        checkThat(&failureCount, "every pixel matches the pattern, alpha included",
                  patternPixelsMatch(BOOLEAN_TRUE));
    }

    printf("\n-- truecolour without alpha (colour type 2) --\n");
    {
        result = pngReadToRgba(truecolourPngFixture, sizeof(truecolourPngFixture), rgba,
                               RGBA_CAPACITY, scratch, SCRATCH_CAPACITY, &width, &height);
        checkThat(&failureCount, "decode reads OK", result == PNG_READ_OK);
        checkThat(&failureCount, "every pixel matches the pattern and is opaque",
                  patternPixelsMatch(BOOLEAN_FALSE));
    }

    printf("\n-- palette (colour type 3) --\n");
    {
        result = pngReadToRgba(palettePngFixture, sizeof(palettePngFixture), rgba,
                               RGBA_CAPACITY, scratch, SCRATCH_CAPACITY, &width, &height);
        checkThat(&failureCount, "decode reads OK", result == PNG_READ_OK);
        checkThat(&failureCount, "every index resolves to its palette entry, opaque",
                  palettePixelsMatch());
    }

    printf("\n-- greyscale (colour type 0) --\n");
    {
        result = pngReadToRgba(greyscalePngFixture, sizeof(greyscalePngFixture), rgba,
                               RGBA_CAPACITY, scratch, SCRATCH_CAPACITY, &width, &height);
        checkThat(&failureCount, "decode reads OK", result == PNG_READ_OK);
        checkThat(&failureCount, "every grey level lands on all three channels, opaque",
                  greyscalePixelsMatch());
    }

    printf("\n-- refusing what it should --\n");
    {
        static const char notPng[] = "this is not a PNG, only a sentence";
        MemorySize corruptedSize = buildCorruptedIdatCrc();

        checkThat(&failureCount, "rejects non-PNG input",
                  pngPeekDimensions((const Unsigned8 *)notPng, sizeof(notPng) - 1UL,
                                    &width, &height, &scratchNeeded) == PNG_READ_INVALID);

        checkThat(&failureCount, "found an IDAT to corrupt", corruptedSize != 0UL);
        checkThat(&failureCount, "a flipped IDAT checksum byte is caught",
                  pngPeekDimensions(corrupted, corruptedSize, &width, &height,
                                    &scratchNeeded) == PNG_READ_INVALID);

        checkThat(&failureCount, "16-bit channels are unsupported, not misread",
                  pngPeekDimensions(sixteenBitPngFixture, sizeof(sixteenBitPngFixture),
                                    &width, &height, &scratchNeeded) == PNG_READ_UNSUPPORTED);
        checkThat(&failureCount, "Adam7 interlacing is unsupported, not misread",
                  pngPeekDimensions(interlacedPngFixture, sizeof(interlacedPngFixture),
                                    &width, &height, &scratchNeeded) == PNG_READ_UNSUPPORTED);
    }

    return checkSummarize(failureCount, "PNG reader");
}
