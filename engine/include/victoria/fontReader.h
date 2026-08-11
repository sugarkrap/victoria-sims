#ifndef VICTORIA_FONT_READER_HEADER
#define VICTORIA_FONT_READER_HEADER

#include "victoria/coreTypes.h"

typedef enum FontTable
{
    FONT_TABLE_HEAD = 0,
    FONT_TABLE_MAXIMUM_PROFILE,
    FONT_TABLE_HORIZONTAL_HEADER,
    FONT_TABLE_HORIZONTAL_METRICS,
    FONT_TABLE_INDEX_TO_LOCATION,
    FONT_TABLE_GLYPH_DATA,
    FONT_TABLE_CHARACTER_MAP,
    FONT_TABLE_COUNT
} FontTable;

typedef struct FontReader
{
    const Unsigned8 *bytes;
    MemorySize byteCount;

    MemorySize payloadOffset;
    Unsigned8 obfuscationKey;

    MemorySize tableOffset[FONT_TABLE_COUNT];
    MemorySize tableLength[FONT_TABLE_COUNT];

    Unsigned32 unitsPerEm;
    Unsigned32 glyphCount;
    Unsigned32 locationsAreLong;

    Integer32 ascender;
    Integer32 descender;
    Integer32 lineGap;
    Unsigned32 horizontalMetricCount;

    MemorySize characterMapSubtable;
} FontReader;

typedef struct FontGlyphOutline
{
    Integer32 *pointX;
    Integer32 *pointY;
    Unsigned8 *pointIsOnCurve;
    Unsigned32 *contourLastPoint;
    Unsigned32 pointCapacity;
    Unsigned32 contourCapacity;

    Unsigned32 pointCount;
    Unsigned32 contourCount;

    Integer32 minimumX;
    Integer32 minimumY;
    Integer32 maximumX;
    Integer32 maximumY;

    Boolean overflowed;
} FontGlyphOutline;

void fontOutlineBind(FontGlyphOutline *outline, Integer32 *pointX, Integer32 *pointY,
                     Unsigned8 *pointIsOnCurve, Unsigned32 *contourLastPoint,
                     Unsigned32 pointCapacity, Unsigned32 contourCapacity);

Boolean fontReaderOpen(FontReader *font, const Unsigned8 *bytes, MemorySize byteCount);

Unsigned32 fontReaderFindGlyph(const FontReader *font, Unsigned32 codePoint);

Integer32 fontReaderGetAdvanceWidth(const FontReader *font, Unsigned32 glyphIndex);

Boolean fontReaderGetGlyphOutline(const FontReader *font, Unsigned32 glyphIndex,
                                  FontGlyphOutline *outline);

#endif
