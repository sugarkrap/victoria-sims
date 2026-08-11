#include "victoria/interfaceSurface.h"

void interfaceSurfaceBind(InterfaceSurface *surface, Unsigned8 *pixels, Unsigned32 maximumWidth,
                          Unsigned32 maximumHeight)
{
    surface->pixels = pixels;
    surface->maximumWidth = (pixels != NULL_POINTER) ? maximumWidth : 0U;
    surface->maximumHeight = (pixels != NULL_POINTER) ? maximumHeight : 0U;
    surface->width = 0U;
    surface->height = 0U;
    surface->revision = 0U;
    surface->charactersMissing = 0U;
    surface->drawsClipped = 0U;
}

Boolean interfaceSurfaceBegin(InterfaceSurface *surface, Unsigned32 width, Unsigned32 height)
{
    MemorySize count;
    MemorySize index;

    surface->width = 0U;
    surface->height = 0U;
    surface->charactersMissing = 0U;
    surface->drawsClipped = 0U;
    if (surface->pixels == NULL_POINTER || width == 0U || height == 0U ||
        width > surface->maximumWidth || height > surface->maximumHeight)
    {
        return BOOLEAN_FALSE;
    }
    surface->width = width;
    surface->height = height;
    count = (MemorySize)width * (MemorySize)height * (MemorySize)INTERFACE_BYTES_PER_PIXEL;
    for (index = 0UL; index < count; index++)
    {
        surface->pixels[index] = 0U;
    }
    return BOOLEAN_TRUE;
}

void interfaceSurfaceEnd(InterfaceSurface *surface)
{
    surface->revision++;
}

static Unsigned32 scaleByAlpha(Unsigned32 value, Unsigned32 alpha)
{
    Unsigned32 product = (value * alpha) + 128U;

    return (product + (product >> 8)) >> 8;
}

static void blendPixel(Unsigned8 *destination, Unsigned32 red, Unsigned32 green, Unsigned32 blue,
                       Unsigned32 alpha)
{
    Unsigned32 keep;

    if (alpha == 0U)
    {
        return;
    }
    if (alpha == 255U)
    {
        destination[0] = (Unsigned8)red;
        destination[1] = (Unsigned8)green;
        destination[2] = (Unsigned8)blue;
        destination[3] = 255U;
        return;
    }
    keep = 255U - alpha;
    destination[0] = (Unsigned8)(red + scaleByAlpha(destination[0], keep));
    destination[1] = (Unsigned8)(green + scaleByAlpha(destination[1], keep));
    destination[2] = (Unsigned8)(blue + scaleByAlpha(destination[2], keep));
    destination[3] = (Unsigned8)(alpha + scaleByAlpha(destination[3], keep));
}

static Boolean clipSpan(Integer32 start, Unsigned32 length, Unsigned32 limit, Integer32 *from,
                        Integer32 *to)
{
    Integer32 end = start + (Integer32)length;

    if (start < 0)
    {
        start = 0;
    }
    if (end > (Integer32)limit)
    {
        end = (Integer32)limit;
    }
    *from = start;
    *to = end;
    return (end > start) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

static Unsigned8 *pixelAt(InterfaceSurface *surface, Integer32 column, Integer32 row)
{
    return &surface->pixels[((((MemorySize)row * surface->width) + (MemorySize)column) *
                             INTERFACE_BYTES_PER_PIXEL)];
}

void interfaceSurfaceFill(InterfaceSurface *surface, Integer32 left, Integer32 top,
                          Unsigned32 width, Unsigned32 height, InterfaceColor color)
{
    Integer32 fromX;
    Integer32 toX;
    Integer32 fromY;
    Integer32 toY;
    Unsigned32 red;
    Unsigned32 green;
    Unsigned32 blue;
    Integer32 row;

    if (surface->pixels == NULL_POINTER || color.alpha == 0U)
    {
        return;
    }
    if (!clipSpan(left, width, surface->width, &fromX, &toX) ||
        !clipSpan(top, height, surface->height, &fromY, &toY))
    {
        surface->drawsClipped++;
        return;
    }
    red = scaleByAlpha(color.red, color.alpha);
    green = scaleByAlpha(color.green, color.alpha);
    blue = scaleByAlpha(color.blue, color.alpha);

    for (row = fromY; row < toY; row++)
    {
        Integer32 column;

        for (column = fromX; column < toX; column++)
        {
            blendPixel(pixelAt(surface, column, row), red, green, blue, color.alpha);
        }
    }
}

void interfaceSurfaceBorder(InterfaceSurface *surface, Integer32 left, Integer32 top,
                            Unsigned32 width, Unsigned32 height, Unsigned32 thickness,
                            InterfaceColor color)
{
    if (thickness == 0U || width == 0U || height == 0U)
    {
        return;
    }
    if (thickness * 2U >= width || thickness * 2U >= height)
    {
        interfaceSurfaceFill(surface, left, top, width, height, color);
        return;
    }
    interfaceSurfaceFill(surface, left, top, width, thickness, color);
    interfaceSurfaceFill(surface, left, top + (Integer32)(height - thickness), width, thickness,
                         color);
    interfaceSurfaceFill(surface, left, top + (Integer32)thickness, thickness,
                         height - (2U * thickness), color);
    interfaceSurfaceFill(surface, left + (Integer32)(width - thickness),
                         top + (Integer32)thickness, thickness, height - (2U * thickness), color);
}

Integer32 interfaceSurfaceText(InterfaceSurface *surface, const FontAtlas *atlas, Integer32 left,
                               Integer32 baselineY, const char *text, InterfaceColor color)
{
    Integer32 pen = left;

    if (surface->pixels == NULL_POINTER || atlas == NULL_POINTER || !atlas->ready ||
        text == NULL_POINTER)
    {
        return pen;
    }
    while (*text != '\0' && *text != '\n')
    {
        const FontAtlasGlyph *glyph = fontAtlasFind(atlas, (Unsigned32)(Unsigned8)*text);

        if (glyph == NULL_POINTER)
        {
            surface->charactersMissing++;
            text++;
            continue;
        }
        if (glyph->widthInPixels > 0U && glyph->heightInPixels > 0U)
        {
            Integer32 glyphLeft = pen + glyph->leftBearing;
            Integer32 glyphTop = baselineY - glyph->topBearing;
            Unsigned32 row;

            for (row = 0U; row < glyph->heightInPixels; row++)
            {
                Integer32 targetRow = glyphTop + (Integer32)row;
                const Unsigned8 *coverage;
                Unsigned32 column;

                if (targetRow < 0 || (Unsigned32)targetRow >= surface->height)
                {
                    continue;
                }
                coverage = &atlas->sheet[(((MemorySize)glyph->sheetY + row) * atlas->sheetWidth) +
                                         glyph->sheetX];
                for (column = 0U; column < glyph->widthInPixels; column++)
                {
                    Integer32 targetColumn = glyphLeft + (Integer32)column;
                    Unsigned32 alpha;

                    if (targetColumn < 0 || (Unsigned32)targetColumn >= surface->width ||
                        coverage[column] == 0U)
                    {
                        continue;
                    }
                    alpha = scaleByAlpha(coverage[column], color.alpha);
                    blendPixel(pixelAt(surface, targetColumn, targetRow),
                               scaleByAlpha(color.red, alpha), scaleByAlpha(color.green, alpha),
                               scaleByAlpha(color.blue, alpha), alpha);
                }
            }
        }
        pen += (Integer32)glyph->advanceInPixels;
        text++;
    }
    return pen;
}

void interfaceSurfaceImage(InterfaceSurface *surface, Integer32 left, Integer32 top,
                           Unsigned32 width, Unsigned32 height, const Unsigned8 *rgbaPixels,
                           Unsigned32 sourceWidth, Unsigned32 sourceHeight)
{
    Integer32 fromX;
    Integer32 toX;
    Integer32 fromY;
    Integer32 toY;
    Integer32 row;

    if (surface->pixels == NULL_POINTER || rgbaPixels == NULL_POINTER || width == 0U ||
        height == 0U || sourceWidth == 0U || sourceHeight == 0U)
    {
        return;
    }
    if (!clipSpan(left, width, surface->width, &fromX, &toX) ||
        !clipSpan(top, height, surface->height, &fromY, &toY))
    {
        surface->drawsClipped++;
        return;
    }

    for (row = fromY; row < toY; row++)
    {
        Unsigned32 sourceRow =
            (Unsigned32)(((MemorySize)(row - top) * sourceHeight) / (MemorySize)height);
        const Unsigned8 *sourceLine;
        Integer32 column;

        if (sourceRow >= sourceHeight)
        {
            sourceRow = sourceHeight - 1U;
        }
        sourceLine = &rgbaPixels[(MemorySize)sourceRow * sourceWidth * 4UL];
        for (column = fromX; column < toX; column++)
        {
            Unsigned32 sourceColumn =
                (Unsigned32)(((MemorySize)(column - left) * sourceWidth) / (MemorySize)width);
            const Unsigned8 *source;
            Unsigned32 alpha;

            if (sourceColumn >= sourceWidth)
            {
                sourceColumn = sourceWidth - 1U;
            }
            source = &sourceLine[(MemorySize)sourceColumn * 4UL];
            alpha = source[3];
            blendPixel(pixelAt(surface, column, row), scaleByAlpha(source[0], alpha),
                       scaleByAlpha(source[1], alpha), scaleByAlpha(source[2], alpha), alpha);
        }
    }
}
