#ifndef VICTORIA_FONT_READER_HEADER
#define VICTORIA_FONT_READER_HEADER

#include "victoria/coreTypes.h"

/* The game's own fonts, read the same way its meshes and textures are.
 *
 * The Sims 2 keeps them under TSData/Res/UI/Fonts as files with an .mxf
 * extension, and an .mxf is a TrueType font behind a very thin door: four bytes
 * of magic, five bytes this reader does not claim to understand, and then the
 * whole sfnt with every byte exclusive-ored against one constant. The container
 * is padded to a fixed length with what looks like a run of one repeated byte
 * and is in fact a run of zeroes wearing the same disguise — which is the clue
 * that gave the key away, since a constant applied to a constant is a constant.
 *
 * Nothing here hardcodes that key. A TrueType file begins 00 01 00 00, so both
 * the key and where the payload starts fall out of looking for those four bytes
 * under a single-byte mask: the key is whatever turns the first byte into
 * nought, and the offset is wherever the other three then agree. That is a
 * documented invariant of the format rather than a guess about the people who
 * shipped it, so a disc that obfuscates differently — or not at all — reads
 * without anybody editing a constant.
 *
 * These are outlines and not bitmaps, which is a cost and worth being plain
 * about: turning one into pixels is a rasterizer, and the floor of the device
 * ladder has no floating point unit to run the usual kind. See glyphRaster.h
 * for what is done about that, and fontAtlas.h for why it is only ever paid
 * once.
 *
 * Nothing in here allocates and nothing in here copies the file. The bytes stay
 * where the caller put them and every read reaches through the mask, which
 * costs an exclusive-or per byte and saves holding a second copy of a hundred
 * and eighty kilobytes. */

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

    /* Where the sfnt starts inside the container, and what every byte of it is
       masked with. Both are found rather than assumed; a plain TrueType file
       comes out as offset nought and key nought. */
    MemorySize payloadOffset;
    Unsigned8 obfuscationKey;

    /* Absolute, so callers never have to remember to add the payload offset —
       which is exactly the kind of thing that is right in nine places and wrong
       in the tenth. Length nought means the table is absent. */
    MemorySize tableOffset[FONT_TABLE_COUNT];
    MemorySize tableLength[FONT_TABLE_COUNT];

    Unsigned32 unitsPerEm;
    Unsigned32 glyphCount;
    /* Whether loca holds shorts (0) or longs (1). Reading it the wrong way
       gives every glyph a plausible and completely wrong extent, which draws as
       confetti rather than as nothing. */
    Unsigned32 locationsAreLong;

    Integer32 ascender;
    Integer32 descender;
    Integer32 lineGap;
    Unsigned32 horizontalMetricCount;

    /* Which of the character map's subtables to look characters up in, chosen
       once when the font is opened. A font carries several — the same font
       addressed as Unicode, as Macintosh Roman, as whatever a printer wanted —
       and picking per lookup would either mean choosing again for every
       character or choosing differently in two places. Nought when the font has
       no subtable this reader understands, and then every character maps to the
       missing glyph rather than to a wrong one. */
    MemorySize characterMapSubtable;
} FontReader;

/* An outline, in font units, written into arrays the caller owns.
 *
 * Contours are given by the index of their last point, which is how the format
 * stores them and is one fewer conversion to get wrong. A point that is not on
 * the curve is the control point of a quadratic; two off-curve points in a row
 * imply an on-curve point halfway between them, which the format leaves out to
 * save space and every reader has to put back. */
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

    /* The glyph had more points or contours than there was room for, so what is
       here is a prefix of it. Said rather than silently truncated: a glyph
       missing its last contour is a letter missing its counter, which reads as
       a rasterizer bug and is not one. */
    Boolean overflowed;
} FontGlyphOutline;

void fontOutlineBind(FontGlyphOutline *outline, Integer32 *pointX, Integer32 *pointY,
                     Unsigned8 *pointIsOnCurve, Unsigned32 *contourLastPoint,
                     Unsigned32 pointCapacity, Unsigned32 contourCapacity);

/* Opens a font in place. False when the bytes are not a font this can read:
   no sfnt signature under any single-byte mask, or a signature with no glyph
   outlines behind it. */
Boolean fontReaderOpen(FontReader *font, const Unsigned8 *bytes, MemorySize byteCount);

/* The glyph a character maps to, or nought — which is the format's own answer
   for "not in this font" and is drawn as the missing-glyph box. */
Unsigned32 fontReaderFindGlyph(const FontReader *font, Unsigned32 codePoint);

/* How far the pen moves after drawing this glyph, in font units. */
Integer32 fontReaderGetAdvanceWidth(const FontReader *font, Unsigned32 glyphIndex);

/* Fills the outline. False when the glyph does not exist; true with no points
   for a glyph that has none, which is what a space is and is not an error. */
Boolean fontReaderGetGlyphOutline(const FontReader *font, Unsigned32 glyphIndex,
                                  FontGlyphOutline *outline);

#endif
