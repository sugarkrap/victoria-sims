#include "victoria/fontAtlas.h"

#include "utils/checksum.h"
#include "victoria/builtinFont.h"

#define GLYPH_MARGIN 1U

#define ATLAS_MAGIC 0x544E4656UL
#define ATLAS_VERSION 1UL

#define ATLAS_HEADER_BYTES 40UL
#define ATLAS_GLYPH_BYTES 9UL
#define ATLAS_TRAILER_BYTES 4UL

static void clearGlyphs(FontAtlas *atlas)
{
    Unsigned32 index;

    for (index = 0U; index < (Unsigned32)FONT_ATLAS_CHARACTER_COUNT; index++)
    {
        atlas->glyphs[index].sheetX = 0U;
        atlas->glyphs[index].sheetY = 0U;
        atlas->glyphs[index].widthInPixels = 0U;
        atlas->glyphs[index].heightInPixels = 0U;
        atlas->glyphs[index].leftBearing = 0;
        atlas->glyphs[index].topBearing = 0;
        atlas->glyphs[index].advanceInPixels = 0U;
    }
}

void fontAtlasBind(FontAtlas *atlas, Unsigned8 *sheet, Unsigned32 sheetWidth,
                   Unsigned32 sheetHeight)
{
    atlas->sheet = sheet;
    atlas->sheetWidth = (sheet != NULL_POINTER) ? sheetWidth : 0U;
    atlas->sheetHeight = (sheet != NULL_POINTER) ? sheetHeight : 0U;
    atlas->usedRows = 0U;
    atlas->pixelSize = 0U;
    atlas->lineHeight = 0U;
    atlas->baseline = 0U;
    atlas->sourceMark = 0U;
    atlas->glyphsRefused = 0U;
    atlas->ready = BOOLEAN_FALSE;
    clearGlyphs(atlas);
}

static void clearSheet(FontAtlas *atlas)
{
    MemorySize count = (MemorySize)atlas->sheetWidth * (MemorySize)atlas->sheetHeight;
    MemorySize index;

    for (index = 0UL; index < count; index++)
    {
        atlas->sheet[index] = 0U;
    }
}

typedef struct ShelfCursor
{
    Unsigned32 x;
    Unsigned32 y;
    Unsigned32 tallest;
} ShelfCursor;

static Boolean reserveSpace(FontAtlas *atlas, ShelfCursor *cursor, Unsigned32 width,
                            Unsigned32 height, Unsigned32 *placedX, Unsigned32 *placedY)
{
    if (width > atlas->sheetWidth)
    {
        return BOOLEAN_FALSE;
    }
    if (cursor->x + width > atlas->sheetWidth)
    {
        cursor->y += cursor->tallest + GLYPH_MARGIN;
        cursor->x = 0U;
        cursor->tallest = 0U;
    }
    if (cursor->y + height > atlas->sheetHeight)
    {
        return BOOLEAN_FALSE;
    }
    *placedX = cursor->x;
    *placedY = cursor->y;
    cursor->x += width + GLYPH_MARGIN;
    if (height > cursor->tallest)
    {
        cursor->tallest = height;
    }
    if (cursor->y + cursor->tallest > atlas->usedRows)
    {
        atlas->usedRows = cursor->y + cursor->tallest;
    }
    return BOOLEAN_TRUE;
}

static Boolean glyphFitsTheFields(Unsigned32 width, Unsigned32 height, Integer32 leftBearing,
                                  Integer32 topBearing, Integer32 advance)
{
    return (width <= 255U && height <= 255U && leftBearing >= -128 && leftBearing <= 127 &&
            topBearing >= -128 && topBearing <= 127 && advance >= 0 && advance <= 255)
               ? BOOLEAN_TRUE
               : BOOLEAN_FALSE;
}

Boolean fontAtlasBuildFromFont(FontAtlas *atlas, const FontReader *font,
                               GlyphRasterizer *rasterizer, FontGlyphOutline *outline,
                               Unsigned32 pixelSize, Unsigned32 sourceMark)
{
    ShelfCursor cursor;
    Unsigned32 scale;
    Unsigned32 character;

    if (atlas->sheet == NULL_POINTER || pixelSize == 0U)
    {
        return BOOLEAN_FALSE;
    }
    scale = glyphRasterScaleFor(font->unitsPerEm, pixelSize);
    if (scale == 0U)
    {
        return BOOLEAN_FALSE;
    }

    clearSheet(atlas);
    clearGlyphs(atlas);
    atlas->usedRows = 0U;
    atlas->glyphsRefused = 0U;
    atlas->pixelSize = pixelSize;
    atlas->sourceMark = sourceMark;
    atlas->baseline =
        (Unsigned32)((((Integer64)font->ascender * (Integer64)scale) + 65535) >> 16);
    atlas->lineHeight =
        (Unsigned32)(((((Integer64)font->ascender - (Integer64)font->descender +
                        (Integer64)font->lineGap) *
                       (Integer64)scale) +
                      65535) >>
                     16);
    if (atlas->lineHeight == 0U)
    {
        atlas->lineHeight = pixelSize;
    }

    cursor.x = 0U;
    cursor.y = 0U;
    cursor.tallest = 0U;

    for (character = 0U; character < (Unsigned32)FONT_ATLAS_CHARACTER_COUNT; character++)
    {
        Unsigned32 codePoint = (Unsigned32)FONT_ATLAS_FIRST_CHARACTER + character;
        Unsigned32 glyphIndex = fontReaderFindGlyph(font, codePoint);
        FontAtlasGlyph *entry = &atlas->glyphs[character];
        GlyphPlacement placement;
        Integer32 advance;
        Unsigned32 placedX = 0U;
        Unsigned32 placedY = 0U;

        advance = (Integer32)((((Integer64)fontReaderGetAdvanceWidth(font, glyphIndex) *
                                (Integer64)scale) +
                               32768) >>
                              16);
        if (advance < 0)
        {
            advance = 0;
        }
        if (advance > 255)
        {
            advance = 255;
        }
        entry->advanceInPixels = (Unsigned8)advance;

        if (!fontReaderGetGlyphOutline(font, glyphIndex, outline))
        {
            atlas->glyphsRefused++;
            continue;
        }
        if (!glyphRasterPlace(outline, scale, &placement))
        {
            continue;
        }
        if (!glyphFitsTheFields(placement.widthInPixels, placement.heightInPixels,
                                placement.leftBearing, placement.topBearing, advance) ||
            !reserveSpace(atlas, &cursor, placement.widthInPixels, placement.heightInPixels,
                          &placedX, &placedY))
        {
            atlas->glyphsRefused++;
            continue;
        }
        if (!glyphRasterDraw(rasterizer, outline, scale, &placement,
                             &atlas->sheet[((MemorySize)placedY * atlas->sheetWidth) + placedX],
                             atlas->sheetWidth))
        {
            atlas->glyphsRefused++;
            continue;
        }

        entry->sheetX = (Unsigned16)placedX;
        entry->sheetY = (Unsigned16)placedY;
        entry->widthInPixels = (Unsigned8)placement.widthInPixels;
        entry->heightInPixels = (Unsigned8)placement.heightInPixels;
        entry->leftBearing = (Integer8)placement.leftBearing;
        entry->topBearing = (Integer8)placement.topBearing;
    }

    atlas->ready = BOOLEAN_TRUE;
    return BOOLEAN_TRUE;
}

Boolean fontAtlasBuildFromBuiltin(FontAtlas *atlas, Unsigned32 magnification)
{
    ShelfCursor cursor;
    Unsigned32 character;
    Unsigned32 cellWidth;
    Unsigned32 cellHeight;

    if (atlas->sheet == NULL_POINTER)
    {
        return BOOLEAN_FALSE;
    }
    if (magnification == 0U)
    {
        magnification = 1U;
    }
    cellWidth = (Unsigned32)BUILTIN_FONT_WIDTH * magnification;
    cellHeight = (Unsigned32)BUILTIN_FONT_HEIGHT * magnification;

    clearSheet(atlas);
    clearGlyphs(atlas);
    atlas->usedRows = 0U;
    atlas->glyphsRefused = 0U;
    atlas->pixelSize = cellHeight;
    atlas->baseline = (Unsigned32)(BUILTIN_FONT_HEIGHT - 1U) * magnification;
    atlas->lineHeight = cellHeight + (2U * magnification);
    atlas->sourceMark = 0U;

    cursor.x = 0U;
    cursor.y = 0U;
    cursor.tallest = 0U;

    for (character = 0U; character < (Unsigned32)FONT_ATLAS_CHARACTER_COUNT; character++)
    {
        Unsigned32 codePoint = (Unsigned32)FONT_ATLAS_FIRST_CHARACTER + character;
        FontAtlasGlyph *entry = &atlas->glyphs[character];
        Unsigned32 placedX = 0U;
        Unsigned32 placedY = 0U;
        Unsigned32 row;

        entry->advanceInPixels = (Unsigned8)(cellWidth + magnification);
        if (!reserveSpace(atlas, &cursor, cellWidth, cellHeight, &placedX, &placedY))
        {
            atlas->glyphsRefused++;
            continue;
        }
        entry->sheetX = (Unsigned16)placedX;
        entry->sheetY = (Unsigned16)placedY;
        entry->widthInPixels = (Unsigned8)cellWidth;
        entry->heightInPixels = (Unsigned8)cellHeight;
        entry->leftBearing = 0;
        entry->topBearing = (Integer8)atlas->baseline;

        for (row = 0U; row < cellHeight; row++)
        {
            Unsigned8 *destination =
                &atlas->sheet[((MemorySize)(placedY + row) * atlas->sheetWidth) + placedX];
            Unsigned32 column;

            for (column = 0U; column < cellWidth; column++)
            {
                destination[column] = builtinFontHasInk(codePoint, column / magnification,
                                                        row / magnification)
                                          ? 255U
                                          : 0U;
            }
        }
    }

    atlas->ready = BOOLEAN_TRUE;
    return BOOLEAN_TRUE;
}

const FontAtlasGlyph *fontAtlasFind(const FontAtlas *atlas, Unsigned32 codePoint)
{
    if (!atlas->ready || codePoint < (Unsigned32)FONT_ATLAS_FIRST_CHARACTER ||
        codePoint >= (Unsigned32)FONT_ATLAS_FIRST_CHARACTER +
                         (Unsigned32)FONT_ATLAS_CHARACTER_COUNT)
    {
        return NULL_POINTER;
    }
    return &atlas->glyphs[codePoint - (Unsigned32)FONT_ATLAS_FIRST_CHARACTER];
}

Unsigned32 fontAtlasMeasureLine(const FontAtlas *atlas, const char *text)
{
    Unsigned32 width = 0U;

    if (text == NULL_POINTER)
    {
        return 0U;
    }
    while (*text != '\0' && *text != '\n')
    {
        const FontAtlasGlyph *glyph = fontAtlasFind(atlas, (Unsigned32)(Unsigned8)*text);

        if (glyph != NULL_POINTER)
        {
            width += glyph->advanceInPixels;
        }
        text++;
    }
    return width;
}

MemorySize fontAtlasStoredSize(const FontAtlas *atlas)
{
    return ATLAS_HEADER_BYTES +
           ((MemorySize)FONT_ATLAS_CHARACTER_COUNT * ATLAS_GLYPH_BYTES) +
           ((MemorySize)atlas->usedRows * (MemorySize)atlas->sheetWidth) + ATLAS_TRAILER_BYTES;
}

static void writeUnsigned32(Unsigned8 *destination, Unsigned32 value)
{
    destination[0] = (Unsigned8)(value & 0xFFU);
    destination[1] = (Unsigned8)((value >> 8) & 0xFFU);
    destination[2] = (Unsigned8)((value >> 16) & 0xFFU);
    destination[3] = (Unsigned8)((value >> 24) & 0xFFU);
}

static Unsigned32 readUnsigned32(const Unsigned8 *source)
{
    return (Unsigned32)source[0] | ((Unsigned32)source[1] << 8) | ((Unsigned32)source[2] << 16) |
           ((Unsigned32)source[3] << 24);
}

MemorySize fontAtlasStore(const FontAtlas *atlas, Unsigned8 *destination, MemorySize capacity)
{
    MemorySize needed = fontAtlasStoredSize(atlas);
    MemorySize at;
    Unsigned32 index;
    MemorySize pixelBytes = (MemorySize)atlas->usedRows * (MemorySize)atlas->sheetWidth;

    if (!atlas->ready || destination == NULL_POINTER || capacity < needed)
    {
        return 0UL;
    }

    writeUnsigned32(&destination[0], (Unsigned32)ATLAS_MAGIC);
    writeUnsigned32(&destination[4], (Unsigned32)ATLAS_VERSION);
    writeUnsigned32(&destination[8], atlas->sourceMark);
    writeUnsigned32(&destination[12], atlas->pixelSize);
    writeUnsigned32(&destination[16], atlas->sheetWidth);
    writeUnsigned32(&destination[20], atlas->usedRows);
    writeUnsigned32(&destination[24], atlas->lineHeight);
    writeUnsigned32(&destination[28], atlas->baseline);
    writeUnsigned32(&destination[32], (Unsigned32)FONT_ATLAS_FIRST_CHARACTER);
    writeUnsigned32(&destination[36], (Unsigned32)FONT_ATLAS_CHARACTER_COUNT);

    at = ATLAS_HEADER_BYTES;
    for (index = 0U; index < (Unsigned32)FONT_ATLAS_CHARACTER_COUNT; index++)
    {
        const FontAtlasGlyph *glyph = &atlas->glyphs[index];

        destination[at + 0UL] = (Unsigned8)(glyph->sheetX & 0xFFU);
        destination[at + 1UL] = (Unsigned8)((glyph->sheetX >> 8) & 0xFFU);
        destination[at + 2UL] = (Unsigned8)(glyph->sheetY & 0xFFU);
        destination[at + 3UL] = (Unsigned8)((glyph->sheetY >> 8) & 0xFFU);
        destination[at + 4UL] = glyph->widthInPixels;
        destination[at + 5UL] = glyph->heightInPixels;
        destination[at + 6UL] = (Unsigned8)glyph->leftBearing;
        destination[at + 7UL] = (Unsigned8)glyph->topBearing;
        destination[at + 8UL] = glyph->advanceInPixels;
        at += ATLAS_GLYPH_BYTES;
    }

    {
        MemorySize pixel;

        for (pixel = 0UL; pixel < pixelBytes; pixel++)
        {
            destination[at + pixel] = atlas->sheet[pixel];
        }
        at += pixelBytes;
    }

    writeUnsigned32(&destination[at], checksumCrc32(destination, at));
    return needed;
}

Boolean fontAtlasRestore(FontAtlas *atlas, const Unsigned8 *bytes, MemorySize byteCount,
                         Unsigned32 expectedMark)
{
    Unsigned32 sheetWidth;
    Unsigned32 usedRows;
    Unsigned32 firstCharacter;
    Unsigned32 characterCount;
    MemorySize pixelBytes;
    MemorySize needed;
    MemorySize at;
    Unsigned32 index;

    atlas->ready = BOOLEAN_FALSE;
    if (atlas->sheet == NULL_POINTER || bytes == NULL_POINTER || byteCount < ATLAS_HEADER_BYTES)
    {
        return BOOLEAN_FALSE;
    }
    if (readUnsigned32(&bytes[0]) != (Unsigned32)ATLAS_MAGIC ||
        readUnsigned32(&bytes[4]) != (Unsigned32)ATLAS_VERSION)
    {
        return BOOLEAN_FALSE;
    }
    if (readUnsigned32(&bytes[8]) != expectedMark)
    {
        return BOOLEAN_FALSE;
    }

    sheetWidth = readUnsigned32(&bytes[16]);
    usedRows = readUnsigned32(&bytes[20]);
    firstCharacter = readUnsigned32(&bytes[32]);
    characterCount = readUnsigned32(&bytes[36]);
    if (firstCharacter != (Unsigned32)FONT_ATLAS_FIRST_CHARACTER ||
        characterCount != (Unsigned32)FONT_ATLAS_CHARACTER_COUNT)
    {
        return BOOLEAN_FALSE;
    }
    if (sheetWidth != atlas->sheetWidth || usedRows > atlas->sheetHeight)
    {
        return BOOLEAN_FALSE;
    }

    pixelBytes = (MemorySize)usedRows * (MemorySize)sheetWidth;
    needed = ATLAS_HEADER_BYTES + ((MemorySize)characterCount * ATLAS_GLYPH_BYTES) + pixelBytes +
             ATLAS_TRAILER_BYTES;
    if (byteCount < needed)
    {
        return BOOLEAN_FALSE;
    }
    if (readUnsigned32(&bytes[needed - ATLAS_TRAILER_BYTES]) !=
        checksumCrc32(bytes, needed - ATLAS_TRAILER_BYTES))
    {
        return BOOLEAN_FALSE;
    }

    atlas->sourceMark = expectedMark;
    atlas->pixelSize = readUnsigned32(&bytes[12]);
    atlas->usedRows = usedRows;
    atlas->lineHeight = readUnsigned32(&bytes[24]);
    atlas->baseline = readUnsigned32(&bytes[28]);
    atlas->glyphsRefused = 0U;

    at = ATLAS_HEADER_BYTES;
    for (index = 0U; index < characterCount; index++)
    {
        FontAtlasGlyph *glyph = &atlas->glyphs[index];

        glyph->sheetX = (Unsigned16)((Unsigned32)bytes[at + 0UL] |
                                     ((Unsigned32)bytes[at + 1UL] << 8));
        glyph->sheetY = (Unsigned16)((Unsigned32)bytes[at + 2UL] |
                                     ((Unsigned32)bytes[at + 3UL] << 8));
        glyph->widthInPixels = bytes[at + 4UL];
        glyph->heightInPixels = bytes[at + 5UL];
        glyph->leftBearing = (Integer8)bytes[at + 6UL];
        glyph->topBearing = (Integer8)bytes[at + 7UL];
        glyph->advanceInPixels = bytes[at + 8UL];
        at += ATLAS_GLYPH_BYTES;
    }

    clearSheet(atlas);
    {
        MemorySize pixel;

        for (pixel = 0UL; pixel < pixelBytes; pixel++)
        {
            atlas->sheet[pixel] = bytes[at + pixel];
        }
    }

    atlas->ready = BOOLEAN_TRUE;
    return BOOLEAN_TRUE;
}
