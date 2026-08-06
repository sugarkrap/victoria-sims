#ifndef VICTORIA_FONT_ATLAS_HEADER
#define VICTORIA_FONT_ATLAS_HEADER

#include "victoria/coreTypes.h"
#include "victoria/fontReader.h"
#include "victoria/glyphRaster.h"

/* Every letter the engine can draw, rasterized once and kept as one sheet.
 *
 * The point of this file is the word once. Turning an outline into pixels is
 * the expensive thing text does, and the device at the floor of the ladder is
 * an ARMv5TE that would rather not do it at all. An atlas turns that cost from
 * per-frame into per-size, and then the two halves of storing it turn per-size
 * into per-install:
 *
 *   natively   the finished sheet is handed to the platform, which writes it
 *              beside the user's other caches. The next run reads it back and
 *              rasterizes nothing whatsoever. A slow machine pays once, ever.
 *   on the web there is no disk to write to, so the sheet stays in an arena for
 *              the session — which is the one lifetime an arena expresses
 *              perfectly, since an atlas is built once and released never.
 *
 * Both halves want the same thing: the sheet as a flat block of bytes with
 * everything needed to use it again. So that is what fontAtlasStore hands back,
 * and the platform decides what to do with it. Nothing in here knows whether
 * there is a disk.
 *
 * What is stored carries a mark saying what it was made from. A cache that
 * cannot tell a stale entry from a fresh one is worse than no cache, because
 * it draws last week's font over this week's and nothing anywhere says so. */

/* Printable ASCII, which is what the engine's own text is written in. A game
   that says more than this will want more than this, and the format below has
   the character range in it for exactly that reason. */
#define FONT_ATLAS_FIRST_CHARACTER 32U
#define FONT_ATLAS_CHARACTER_COUNT 95U

typedef struct FontAtlasGlyph
{
    Unsigned16 sheetX;
    Unsigned16 sheetY;
    Unsigned8 widthInPixels;
    Unsigned8 heightInPixels;
    /* Both relative to the pen on the baseline, so a caller draws at
       (penX + leftBearing, baselineY - topBearing) and never converts. */
    Integer8 leftBearing;
    Integer8 topBearing;
    Unsigned8 advanceInPixels;
} FontAtlasGlyph;

typedef struct FontAtlas
{
    Unsigned8 *sheet;
    Unsigned32 sheetWidth;
    Unsigned32 sheetHeight;
    /* How far down the sheet anything was actually put. Only this much is
       stored, so an atlas given a generous sheet does not write the empty
       remainder of it to disk every time. */
    Unsigned32 usedRows;

    Unsigned32 pixelSize;
    Unsigned32 lineHeight;
    /* Rows from the top of a line of text down to its baseline. */
    Unsigned32 baseline;

    /* What these pixels were made from: which font, at which size, built how.
       Compared on restore, so a cache written for one font is never drawn for
       another. */
    Unsigned32 sourceMark;

    FontAtlasGlyph glyphs[FONT_ATLAS_CHARACTER_COUNT];

    /* Characters that would not fit the sheet or would not fit the fields
       above. Counted: an atlas quietly missing its widest letters draws text
       with holes in it, which reads as a font problem and is not one. */
    Unsigned32 glyphsRefused;
    Boolean ready;
} FontAtlas;

/* Binds the storage the sheet lives in. The caller owns it — from an arena,
   like everything else — and it must outlive the atlas. */
void fontAtlasBind(FontAtlas *atlas, Unsigned8 *sheet, Unsigned32 sheetWidth,
                   Unsigned32 sheetHeight);

/* Rasterizes the character range out of a font. The rasterizer and the outline
 * are the caller's working storage and are finished with when this returns —
 * they are parameters rather than fields because they are large, they are
 * needed only while building, and an atlas that held them would carry a
 * kilobyte of scratch for the rest of the run.
 *
 * sourceMark is whatever the caller can use to recognise these pixels later;
 * a checksum of the font's bytes mixed with the size is the obvious one. */
Boolean fontAtlasBuildFromFont(FontAtlas *atlas, const FontReader *font,
                               GlyphRasterizer *rasterizer, FontGlyphOutline *outline,
                               Unsigned32 pixelSize, Unsigned32 sourceMark);

/* Fills the atlas from the font the engine carries with it, magnified by whole
   pixels. For before a disc is open, for a disc with no font on it, and for
   saying either of those things out loud. */
Boolean fontAtlasBuildFromBuiltin(FontAtlas *atlas, Unsigned32 magnification);

/* Null for a character the atlas does not carry. */
const FontAtlasGlyph *fontAtlasFind(const FontAtlas *atlas, Unsigned32 codePoint);

/* How wide one line of text comes out, in pixels. Stops at a newline or the
   terminator, so a caller measuring the widest line of a block calls it once
   per line. */
Unsigned32 fontAtlasMeasureLine(const FontAtlas *atlas, const char *text);

/* How many bytes fontAtlasStore will write. */
MemorySize fontAtlasStoredSize(const FontAtlas *atlas);

/* Writes the whole atlas — metrics and pixels — as one block, and returns how
   many bytes that took, or nought when it would not fit. Byte order is fixed
   rather than the host's, so a block is a block wherever it was written. */
MemorySize fontAtlasStore(const FontAtlas *atlas, Unsigned8 *destination, MemorySize capacity);

/* Reads one back into the bound sheet. False when the block is not one of
   these, is damaged, was made from something else, or is too big for the sheet
   this atlas has — and in every one of those cases the atlas is left unready
   rather than half filled, so the caller's answer is always to build it. */
Boolean fontAtlasRestore(FontAtlas *atlas, const Unsigned8 *bytes, MemorySize byteCount,
                         Unsigned32 expectedMark);

#endif
