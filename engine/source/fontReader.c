#include "victoria/fontReader.h"

static Unsigned8 readByte(const FontReader *font, MemorySize offset)
{
    if (offset >= font->byteCount)
    {
        return 0U;
    }
    return (Unsigned8)(font->bytes[offset] ^ font->obfuscationKey);
}

static Unsigned32 readUnsigned16(const FontReader *font, MemorySize offset)
{
    return ((Unsigned32)readByte(font, offset) << 8) | (Unsigned32)readByte(font, offset + 1UL);
}

static Integer32 readSigned16(const FontReader *font, MemorySize offset)
{
    return (Integer32)(Integer16)(Unsigned16)readUnsigned16(font, offset);
}

static Unsigned32 readUnsigned32(const FontReader *font, MemorySize offset)
{
    return ((Unsigned32)readByte(font, offset) << 24) |
           ((Unsigned32)readByte(font, offset + 1UL) << 16) |
           ((Unsigned32)readByte(font, offset + 2UL) << 8) |
           (Unsigned32)readByte(font, offset + 3UL);
}

static const char *const wantedTags[FONT_TABLE_COUNT] = { "head", "maxp", "hhea", "hmtx",
                                                          "loca", "glyf", "cmap" };

static Boolean tagMatches(const FontReader *font, MemorySize offset, const char *tag)
{
    Unsigned32 index;

    for (index = 0U; index < 4U; index++)
    {
        if (readByte(font, offset + index) != (Unsigned8)tag[index])
        {
            return BOOLEAN_FALSE;
        }
    }
    return BOOLEAN_TRUE;
}

static Boolean directoryLooksLikeAFont(FontReader *font)
{
    Unsigned32 tableCount;
    Unsigned32 index;
    Unsigned32 slot;

    for (slot = 0U; slot < (Unsigned32)FONT_TABLE_COUNT; slot++)
    {
        font->tableOffset[slot] = 0UL;
        font->tableLength[slot] = 0UL;
    }

    tableCount = readUnsigned16(font, font->payloadOffset + 4UL);
    if (tableCount == 0U || tableCount > 64U)
    {
        return BOOLEAN_FALSE;
    }
    if (font->payloadOffset + 12UL + ((MemorySize)tableCount * 16UL) > font->byteCount)
    {
        return BOOLEAN_FALSE;
    }

    for (index = 0U; index < tableCount; index++)
    {
        MemorySize record = font->payloadOffset + 12UL + ((MemorySize)index * 16UL);
        MemorySize offset = (MemorySize)readUnsigned32(font, record + 8UL);
        MemorySize length = (MemorySize)readUnsigned32(font, record + 12UL);

        for (slot = 0U; slot < (Unsigned32)FONT_TABLE_COUNT; slot++)
        {
            if (!tagMatches(font, record, wantedTags[slot]))
            {
                continue;
            }
            if (font->payloadOffset + offset + length > font->byteCount)
            {
                return BOOLEAN_FALSE;
            }
            font->tableOffset[slot] = font->payloadOffset + offset;
            font->tableLength[slot] = length;
        }
    }

    return (font->tableLength[FONT_TABLE_HEAD] >= 54UL &&
            font->tableLength[FONT_TABLE_MAXIMUM_PROFILE] >= 6UL &&
            font->tableLength[FONT_TABLE_INDEX_TO_LOCATION] > 0UL &&
            font->tableLength[FONT_TABLE_GLYPH_DATA] > 0UL)
               ? BOOLEAN_TRUE
               : BOOLEAN_FALSE;
}

static void chooseCharacterMap(FontReader *font)
{
    MemorySize base = font->tableOffset[FONT_TABLE_CHARACTER_MAP];
    Unsigned32 subtableCount;
    Unsigned32 index;
    Unsigned32 bestScore = 0U;

    font->characterMapSubtable = 0UL;
    if (font->tableLength[FONT_TABLE_CHARACTER_MAP] < 4UL)
    {
        return;
    }
    subtableCount = readUnsigned16(font, base + 2UL);
    for (index = 0U; index < subtableCount; index++)
    {
        MemorySize record = base + 4UL + ((MemorySize)index * 8UL);
        Unsigned32 platform = readUnsigned16(font, record);
        Unsigned32 encoding = readUnsigned16(font, record + 2UL);
        MemorySize where = base + (MemorySize)readUnsigned32(font, record + 4UL);
        Unsigned32 format;
        Unsigned32 score = 0U;

        if (where + 4UL > font->byteCount)
        {
            continue;
        }
        format = readUnsigned16(font, where);
        if (format != 0U && format != 4U && format != 6U && format != 12U)
        {
            continue;
        }
        if (platform == 3U && encoding == 10U)
        {
            score = 4U;
        }
        else if (platform == 3U && encoding == 1U)
        {
            score = 3U;
        }
        else if (platform == 0U)
        {
            score = 2U;
        }
        else if (platform == 1U && encoding == 0U)
        {
            score = 1U;
        }
        if (score > bestScore)
        {
            bestScore = score;
            font->characterMapSubtable = where;
        }
    }
}

void fontOutlineBind(FontGlyphOutline *outline, Integer32 *pointX, Integer32 *pointY,
                     Unsigned8 *pointIsOnCurve, Unsigned32 *contourLastPoint,
                     Unsigned32 pointCapacity, Unsigned32 contourCapacity)
{
    outline->pointX = pointX;
    outline->pointY = pointY;
    outline->pointIsOnCurve = pointIsOnCurve;
    outline->contourLastPoint = contourLastPoint;
    outline->pointCapacity = (pointX != NULL_POINTER) ? pointCapacity : 0U;
    outline->contourCapacity = (contourLastPoint != NULL_POINTER) ? contourCapacity : 0U;
    outline->pointCount = 0U;
    outline->contourCount = 0U;
    outline->minimumX = 0;
    outline->minimumY = 0;
    outline->maximumX = 0;
    outline->maximumY = 0;
    outline->overflowed = BOOLEAN_FALSE;
}

Boolean fontReaderOpen(FontReader *font, const Unsigned8 *bytes, MemorySize byteCount)
{
    MemorySize candidate;

    font->bytes = bytes;
    font->byteCount = byteCount;
    font->payloadOffset = 0UL;
    font->obfuscationKey = 0U;
    font->unitsPerEm = 0U;
    font->glyphCount = 0U;
    font->locationsAreLong = 0U;
    font->ascender = 0;
    font->descender = 0;
    font->lineGap = 0;
    font->horizontalMetricCount = 0U;
    font->characterMapSubtable = 0UL;
    {
        Unsigned32 slot;

        for (slot = 0U; slot < (Unsigned32)FONT_TABLE_COUNT; slot++)
        {
            font->tableOffset[slot] = 0UL;
            font->tableLength[slot] = 0UL;
        }
    }
    if (bytes == NULL_POINTER || byteCount < 16UL)
    {
        return BOOLEAN_FALSE;
    }

    for (candidate = 0UL; candidate + 16UL <= byteCount && candidate < 32UL; candidate++)
    {
        Unsigned8 key = bytes[candidate];

        if ((Unsigned8)(bytes[candidate + 1UL] ^ key) != 0x01U ||
            (Unsigned8)(bytes[candidate + 2UL] ^ key) != 0x00U ||
            (Unsigned8)(bytes[candidate + 3UL] ^ key) != 0x00U)
        {
            continue;
        }
        font->payloadOffset = candidate;
        font->obfuscationKey = key;
        if (directoryLooksLikeAFont(font))
        {
            break;
        }
        font->payloadOffset = 0UL;
        font->obfuscationKey = 0U;
    }
    if (font->tableLength[FONT_TABLE_GLYPH_DATA] == 0UL)
    {
        return BOOLEAN_FALSE;
    }

    font->unitsPerEm = readUnsigned16(font, font->tableOffset[FONT_TABLE_HEAD] + 18UL);
    font->locationsAreLong =
        (readSigned16(font, font->tableOffset[FONT_TABLE_HEAD] + 50UL) != 0) ? 1U : 0U;
    font->glyphCount = readUnsigned16(font, font->tableOffset[FONT_TABLE_MAXIMUM_PROFILE] + 4UL);
    if (font->unitsPerEm == 0U || font->glyphCount == 0U)
    {
        return BOOLEAN_FALSE;
    }

    if (font->tableLength[FONT_TABLE_HORIZONTAL_HEADER] >= 36UL)
    {
        MemorySize header = font->tableOffset[FONT_TABLE_HORIZONTAL_HEADER];

        font->ascender = readSigned16(font, header + 4UL);
        font->descender = readSigned16(font, header + 6UL);
        font->lineGap = readSigned16(font, header + 8UL);
        font->horizontalMetricCount = readUnsigned16(font, header + 34UL);
    }
    if (font->horizontalMetricCount > font->glyphCount)
    {
        font->horizontalMetricCount = font->glyphCount;
    }
    if (font->ascender == 0 && font->descender == 0)
    {
        font->ascender = (Integer32)font->unitsPerEm;
    }

    chooseCharacterMap(font);
    return BOOLEAN_TRUE;
}

static Unsigned32 lookupFormat4(const FontReader *font, MemorySize table, Unsigned32 codePoint)
{
    Unsigned32 segmentCountTwice = readUnsigned16(font, table + 6UL);
    Unsigned32 segmentCount = segmentCountTwice / 2U;
    MemorySize endCodes = table + 14UL;
    MemorySize startCodes = endCodes + segmentCountTwice + 2UL;
    MemorySize deltas = startCodes + segmentCountTwice;
    MemorySize rangeOffsets = deltas + segmentCountTwice;
    Unsigned32 segment;

    if (codePoint > 0xFFFFU)
    {
        return 0U;
    }
    for (segment = 0U; segment < segmentCount; segment++)
    {
        Unsigned32 endCode = readUnsigned16(font, endCodes + ((MemorySize)segment * 2UL));
        Unsigned32 startCode;
        Unsigned32 rangeOffset;
        Unsigned32 glyph;

        if (codePoint > endCode)
        {
            continue;
        }
        startCode = readUnsigned16(font, startCodes + ((MemorySize)segment * 2UL));
        if (codePoint < startCode)
        {
            return 0U;
        }
        rangeOffset = readUnsigned16(font, rangeOffsets + ((MemorySize)segment * 2UL));
        if (rangeOffset == 0U)
        {
            glyph = codePoint;
        }
        else
        {
            MemorySize where = rangeOffsets + ((MemorySize)segment * 2UL) + (MemorySize)rangeOffset +
                               ((MemorySize)(codePoint - startCode) * 2UL);

            glyph = readUnsigned16(font, where);
            if (glyph == 0U)
            {
                return 0U;
            }
        }
        glyph = (glyph + readUnsigned16(font, deltas + ((MemorySize)segment * 2UL))) & 0xFFFFU;
        return glyph;
    }
    return 0U;
}

Unsigned32 fontReaderFindGlyph(const FontReader *font, Unsigned32 codePoint)
{
    MemorySize table = font->characterMapSubtable;
    Unsigned32 format;
    Unsigned32 glyph = 0U;

    if (table == 0UL)
    {
        return 0U;
    }
    format = readUnsigned16(font, table);
    if (format == 0U)
    {
        glyph = (codePoint < 256U) ? readByte(font, table + 6UL + codePoint) : 0U;
    }
    else if (format == 4U)
    {
        glyph = lookupFormat4(font, table, codePoint);
    }
    else if (format == 6U)
    {
        Unsigned32 first = readUnsigned16(font, table + 6UL);
        Unsigned32 count = readUnsigned16(font, table + 8UL);

        if (codePoint >= first && codePoint < first + count)
        {
            glyph = readUnsigned16(font, table + 10UL + ((MemorySize)(codePoint - first) * 2UL));
        }
    }
    else if (format == 12U)
    {
        Unsigned32 groupCount = readUnsigned32(font, table + 12UL);
        Unsigned32 index;

        for (index = 0U; index < groupCount; index++)
        {
            MemorySize group = table + 16UL + ((MemorySize)index * 12UL);
            Unsigned32 first = readUnsigned32(font, group);
            Unsigned32 last = readUnsigned32(font, group + 4UL);

            if (codePoint >= first && codePoint <= last)
            {
                glyph = readUnsigned32(font, group + 8UL) + (codePoint - first);
                break;
            }
        }
    }
    return (glyph < font->glyphCount) ? glyph : 0U;
}

Integer32 fontReaderGetAdvanceWidth(const FontReader *font, Unsigned32 glyphIndex)
{
    Unsigned32 which = glyphIndex;

    if (font->horizontalMetricCount == 0U ||
        font->tableLength[FONT_TABLE_HORIZONTAL_METRICS] == 0UL)
    {
        return (Integer32)font->unitsPerEm / 2;
    }
    if (which >= font->horizontalMetricCount)
    {
        which = font->horizontalMetricCount - 1U;
    }
    return (Integer32)readUnsigned16(
        font, font->tableOffset[FONT_TABLE_HORIZONTAL_METRICS] + ((MemorySize)which * 4UL));
}

static Boolean locateGlyph(const FontReader *font, Unsigned32 glyphIndex, MemorySize *offset,
                           MemorySize *length)
{
    MemorySize locations = font->tableOffset[FONT_TABLE_INDEX_TO_LOCATION];
    MemorySize first;
    MemorySize next;

    if (glyphIndex >= font->glyphCount)
    {
        return BOOLEAN_FALSE;
    }
    if (font->locationsAreLong != 0U)
    {
        first = (MemorySize)readUnsigned32(font, locations + ((MemorySize)glyphIndex * 4UL));
        next = (MemorySize)readUnsigned32(font, locations + ((MemorySize)glyphIndex * 4UL) + 4UL);
    }
    else
    {
        first = (MemorySize)readUnsigned16(font, locations + ((MemorySize)glyphIndex * 2UL)) * 2UL;
        next = (MemorySize)readUnsigned16(font, locations + ((MemorySize)glyphIndex * 2UL) + 2UL) * 2UL;
    }
    if (next < first)
    {
        return BOOLEAN_FALSE;
    }
    *offset = font->tableOffset[FONT_TABLE_GLYPH_DATA] + first;
    *length = next - first;
    return BOOLEAN_TRUE;
}

static MemorySize readCoordinates(const FontReader *font, MemorySize at, Unsigned32 pointCount,
                                  const Unsigned8 *flags, Unsigned32 shortBit, Unsigned32 sameBit,
                                  Integer32 *destination)
{
    Integer32 value = 0;
    Unsigned32 index;

    for (index = 0U; index < pointCount; index++)
    {
        Unsigned32 flag = flags[index];

        if ((flag & shortBit) != 0U)
        {
            Integer32 delta = (Integer32)readByte(font, at);

            at += 1UL;
            value += ((flag & sameBit) != 0U) ? delta : -delta;
        }
        else if ((flag & sameBit) == 0U)
        {
            value += readSigned16(font, at);
            at += 2UL;
        }
        destination[index] = value;
    }
    return at;
}

static void noteExtent(FontGlyphOutline *outline, Integer32 x, Integer32 y)
{
    if (outline->pointCount == 1U)
    {
        outline->minimumX = x;
        outline->maximumX = x;
        outline->minimumY = y;
        outline->maximumY = y;
        return;
    }
    if (x < outline->minimumX)
    {
        outline->minimumX = x;
    }
    if (x > outline->maximumX)
    {
        outline->maximumX = x;
    }
    if (y < outline->minimumY)
    {
        outline->minimumY = y;
    }
    if (y > outline->maximumY)
    {
        outline->maximumY = y;
    }
}

typedef struct ComponentTransform
{
    Integer32 scaleXX;
    Integer32 scaleXY;
    Integer32 scaleYX;
    Integer32 scaleYY;
    Integer32 offsetX;
    Integer32 offsetY;
} ComponentTransform;

static Integer32 applyScale(Integer32 first, Integer32 firstScale, Integer32 second,
                            Integer32 secondScale)
{
    Integer32 total = (first * firstScale) + (second * secondScale);

    return (total >= 0) ? ((total + 8192) >> 14) : -(((-total) + 8192) >> 14);
}

static Boolean gatherGlyph(const FontReader *font, Unsigned32 glyphIndex,
                           FontGlyphOutline *outline, const ComponentTransform *transform,
                           Unsigned32 depth);

static void gatherSimpleGlyph(const FontReader *font, MemorySize at, Integer32 contourCount,
                              FontGlyphOutline *outline, const ComponentTransform *transform)
{
    Unsigned32 firstPoint = outline->pointCount;
    Unsigned32 pointCount;
    Unsigned32 index;
    MemorySize endPoints = at + 10UL;
    MemorySize flagsAt;
    MemorySize xAt;
    Unsigned8 *flags;

    pointCount = readUnsigned16(font, endPoints + ((MemorySize)(contourCount - 1) * 2UL)) + 1U;

    if (firstPoint + pointCount > outline->pointCapacity ||
        outline->contourCount + (Unsigned32)contourCount > outline->contourCapacity)
    {
        outline->overflowed = BOOLEAN_TRUE;
        return;
    }
    flags = &outline->pointIsOnCurve[firstPoint];

    flagsAt = endPoints + ((MemorySize)contourCount * 2UL);
    flagsAt += 2UL + (MemorySize)readUnsigned16(font, flagsAt);
    {
        Unsigned32 written = 0U;

        while (written < pointCount)
        {
            Unsigned8 flag = readByte(font, flagsAt);
            Unsigned32 repeat = 0U;

            flagsAt += 1UL;
            if ((flag & 0x08U) != 0U)
            {
                repeat = readByte(font, flagsAt);
                flagsAt += 1UL;
            }
            do
            {
                flags[written] = flag;
                written++;
            } while (repeat-- > 0U && written < pointCount);
        }
    }

    xAt = readCoordinates(font, flagsAt, pointCount, flags, 0x02U, 0x10U,
                          &outline->pointX[firstPoint]);
    (void)readCoordinates(font, xAt, pointCount, flags, 0x04U, 0x20U,
                          &outline->pointY[firstPoint]);

    for (index = 0U; index < pointCount; index++)
    {
        Integer32 x = outline->pointX[firstPoint + index];
        Integer32 y = outline->pointY[firstPoint + index];

        outline->pointX[firstPoint + index] =
            applyScale(x, transform->scaleXX, y, transform->scaleYX) + transform->offsetX;
        outline->pointY[firstPoint + index] =
            applyScale(x, transform->scaleXY, y, transform->scaleYY) + transform->offsetY;
        outline->pointIsOnCurve[firstPoint + index] = (Unsigned8)(flags[index] & 0x01U);
        outline->pointCount++;
        noteExtent(outline, outline->pointX[firstPoint + index],
                   outline->pointY[firstPoint + index]);
    }

    for (index = 0U; index < (Unsigned32)contourCount; index++)
    {
        Unsigned32 last = readUnsigned16(font, endPoints + ((MemorySize)index * 2UL));

        if (last >= pointCount)
        {
            outline->overflowed = BOOLEAN_TRUE;
            return;
        }
        outline->contourLastPoint[outline->contourCount] = firstPoint + last;
        outline->contourCount++;
    }
}

static void gatherCompositeGlyph(const FontReader *font, MemorySize at, FontGlyphOutline *outline,
                                 const ComponentTransform *transform, Unsigned32 depth)
{
    Unsigned32 flags;
    Unsigned32 remaining = 16U;

    do
    {
        Unsigned32 componentGlyph;
        ComponentTransform component;
        Integer32 firstArgument;
        Integer32 secondArgument;

        flags = readUnsigned16(font, at);
        componentGlyph = readUnsigned16(font, at + 2UL);
        at += 4UL;

        if ((flags & 0x0001U) != 0U)
        {
            firstArgument = readSigned16(font, at);
            secondArgument = readSigned16(font, at + 2UL);
            at += 4UL;
        }
        else
        {
            firstArgument = (Integer32)(Integer8)readByte(font, at);
            secondArgument = (Integer32)(Integer8)readByte(font, at + 1UL);
            at += 2UL;
        }

        component.scaleXX = 0x4000;
        component.scaleXY = 0;
        component.scaleYX = 0;
        component.scaleYY = 0x4000;
        if ((flags & 0x0008U) != 0U)
        {
            component.scaleXX = readSigned16(font, at);
            component.scaleYY = component.scaleXX;
            at += 2UL;
        }
        else if ((flags & 0x0040U) != 0U)
        {
            component.scaleXX = readSigned16(font, at);
            component.scaleYY = readSigned16(font, at + 2UL);
            at += 4UL;
        }
        else if ((flags & 0x0080U) != 0U)
        {
            component.scaleXX = readSigned16(font, at);
            component.scaleXY = readSigned16(font, at + 2UL);
            component.scaleYX = readSigned16(font, at + 4UL);
            component.scaleYY = readSigned16(font, at + 6UL);
            at += 8UL;
        }

        component.offsetX = ((flags & 0x0002U) != 0U) ? firstArgument : 0;
        component.offsetY = ((flags & 0x0002U) != 0U) ? secondArgument : 0;

        {
            ComponentTransform combined;

            combined.scaleXX = applyScale(component.scaleXX, transform->scaleXX, component.scaleXY,
                                          transform->scaleYX);
            combined.scaleXY = applyScale(component.scaleXX, transform->scaleXY, component.scaleXY,
                                          transform->scaleYY);
            combined.scaleYX = applyScale(component.scaleYX, transform->scaleXX, component.scaleYY,
                                          transform->scaleYX);
            combined.scaleYY = applyScale(component.scaleYX, transform->scaleXY, component.scaleYY,
                                          transform->scaleYY);
            combined.offsetX =
                applyScale(component.offsetX, transform->scaleXX, component.offsetY,
                           transform->scaleYX) +
                transform->offsetX;
            combined.offsetY =
                applyScale(component.offsetX, transform->scaleXY, component.offsetY,
                           transform->scaleYY) +
                transform->offsetY;
            (void)gatherGlyph(font, componentGlyph, outline, &combined, depth + 1U);
        }
        remaining--;
    } while ((flags & 0x0020U) != 0U && remaining > 0U);
}

#define COMPONENT_DEPTH_LIMIT 5U

static Boolean gatherGlyph(const FontReader *font, Unsigned32 glyphIndex,
                           FontGlyphOutline *outline, const ComponentTransform *transform,
                           Unsigned32 depth)
{
    MemorySize at;
    MemorySize length;
    Integer32 contourCount;

    if (depth >= COMPONENT_DEPTH_LIMIT)
    {
        outline->overflowed = BOOLEAN_TRUE;
        return BOOLEAN_FALSE;
    }
    if (!locateGlyph(font, glyphIndex, &at, &length))
    {
        return BOOLEAN_FALSE;
    }
    if (length == 0UL)
    {
        return BOOLEAN_TRUE;
    }
    contourCount = readSigned16(font, at);
    if (contourCount > 0)
    {
        gatherSimpleGlyph(font, at, contourCount, outline, transform);
    }
    else if (contourCount < 0)
    {
        gatherCompositeGlyph(font, at + 10UL, outline, transform, depth);
    }
    return BOOLEAN_TRUE;
}

Boolean fontReaderGetGlyphOutline(const FontReader *font, Unsigned32 glyphIndex,
                                  FontGlyphOutline *outline)
{
    ComponentTransform identity;

    outline->pointCount = 0U;
    outline->contourCount = 0U;
    outline->minimumX = 0;
    outline->minimumY = 0;
    outline->maximumX = 0;
    outline->maximumY = 0;
    outline->overflowed = BOOLEAN_FALSE;
    if (outline->pointX == NULL_POINTER || outline->pointY == NULL_POINTER ||
        outline->pointIsOnCurve == NULL_POINTER || outline->contourLastPoint == NULL_POINTER)
    {
        return BOOLEAN_FALSE;
    }

    identity.scaleXX = 0x4000;
    identity.scaleXY = 0;
    identity.scaleYX = 0;
    identity.scaleYY = 0x4000;
    identity.offsetX = 0;
    identity.offsetY = 0;
    return gatherGlyph(font, glyphIndex, outline, &identity, 0U);
}
