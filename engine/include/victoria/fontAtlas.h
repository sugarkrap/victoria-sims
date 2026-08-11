#ifndef VICTORIA_FONT_ATLAS_HEADER
#define VICTORIA_FONT_ATLAS_HEADER

#include "victoria/coreTypes.h"
#include "victoria/fontReader.h"
#include "victoria/glyphRaster.h"

#define FONT_ATLAS_FIRST_CHARACTER 32U
#define FONT_ATLAS_CHARACTER_COUNT 95U

typedef struct FontAtlasGlyph
{
    Unsigned16 sheetX;
    Unsigned16 sheetY;
    Unsigned8 widthInPixels;
    Unsigned8 heightInPixels;
    Integer8 leftBearing;
    Integer8 topBearing;
    Unsigned8 advanceInPixels;
} FontAtlasGlyph;

typedef struct FontAtlas
{
    Unsigned8 *sheet;
    Unsigned32 sheetWidth;
    Unsigned32 sheetHeight;
    Unsigned32 usedRows;

    Unsigned32 pixelSize;
    Unsigned32 lineHeight;
    Unsigned32 baseline;

    Unsigned32 sourceMark;

    FontAtlasGlyph glyphs[FONT_ATLAS_CHARACTER_COUNT];

    Unsigned32 glyphsRefused;
    Boolean ready;
} FontAtlas;

void fontAtlasBind(FontAtlas *atlas, Unsigned8 *sheet, Unsigned32 sheetWidth,
                   Unsigned32 sheetHeight);

Boolean fontAtlasBuildFromFont(FontAtlas *atlas, const FontReader *font,
                               GlyphRasterizer *rasterizer, FontGlyphOutline *outline,
                               Unsigned32 pixelSize, Unsigned32 sourceMark);

Boolean fontAtlasBuildFromBuiltin(FontAtlas *atlas, Unsigned32 magnification);

const FontAtlasGlyph *fontAtlasFind(const FontAtlas *atlas, Unsigned32 codePoint);

Unsigned32 fontAtlasMeasureLine(const FontAtlas *atlas, const char *text);

MemorySize fontAtlasStoredSize(const FontAtlas *atlas);

MemorySize fontAtlasStore(const FontAtlas *atlas, Unsigned8 *destination, MemorySize capacity);

Boolean fontAtlasRestore(FontAtlas *atlas, const Unsigned8 *bytes, MemorySize byteCount,
                         Unsigned32 expectedMark);

#endif
