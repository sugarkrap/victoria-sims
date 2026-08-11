#include "victoria/interfaceMenu.h"

#include "utils/strings.h"

static const InterfaceColor colorPanel = { 14U, 17U, 25U, 226U };
static const InterfaceColor colorSidebar = { 22U, 27U, 38U, 236U };
static const InterfaceColor colorEdge = { 92U, 108U, 138U, 200U };
static const InterfaceColor colorButton = { 38U, 46U, 62U, 236U };
static const InterfaceColor colorButtonHovered = { 62U, 78U, 104U, 244U };
static const InterfaceColor colorButtonCurrent = { 84U, 112U, 158U, 250U };
static const InterfaceColor colorCursor = { 158U, 196U, 255U, 255U };
static const InterfaceColor colorInEffect = { 126U, 226U, 158U, 255U };
static const InterfaceColor colorLabel = { 224U, 231U, 244U, 255U };
static const InterfaceColor colorLabelDim = { 150U, 162U, 184U, 255U };
static const InterfaceColor colorTileBack = { 8U, 10U, 15U, 200U };

static const char *const pageNames[DEBUG_MENU_PAGE_COUNT] = { "body", "clothing", "animation" };

#define MENU_MARGIN 8U
#define TILE_GAP 8U
#define LABEL_HEIGHT 30U
#define THUMBNAIL_WANTED 56U
#define TILE_EXTRA_WIDTH 32U
#define SIDEBAR_WIDTH 104U
#define HEADER_HEIGHT 26U
#define PAGER_HEIGHT 26U
#define PAGER_BUTTON_WIDTH 64U

void interfaceMenuLayout(InterfaceMenuLayout *layout, Unsigned32 availableWidth,
                         Unsigned32 availableHeight)
{
    Unsigned32 gridWidth;
    Unsigned32 gridHeight;

    layout->sidebarWidth = SIDEBAR_WIDTH;
    layout->headerHeight = HEADER_HEIGHT;
    layout->pageButtonHeight = 26U;
    layout->thumbnailSize = THUMBNAIL_WANTED;
    layout->tileWidth = THUMBNAIL_WANTED + (2U * TILE_GAP) + TILE_EXTRA_WIDTH;
    layout->tileHeight = THUMBNAIL_WANTED + LABEL_HEIGHT + TILE_GAP;
    layout->pagerHeight = PAGER_HEIGHT;
    layout->columns = 0U;
    layout->rows = 0U;
    layout->tooSmall = BOOLEAN_TRUE;

    layout->width = (availableWidth * 3U) / 4U;
    if (layout->width > 740U)
    {
        layout->width = 740U;
    }
    layout->height = (availableHeight * 3U) / 4U;
    if (layout->height > 460U)
    {
        layout->height = 460U;
    }
    if (layout->width < layout->sidebarWidth + layout->tileWidth + (2U * MENU_MARGIN) ||
        layout->height < layout->headerHeight + layout->tileHeight + layout->pagerHeight +
                             (2U * MENU_MARGIN))
    {
        layout->width = (availableWidth < 240U) ? availableWidth : 240U;
        layout->height = (availableHeight < 90U) ? availableHeight : 90U;
        layout->gridLeft = (Integer32)layout->sidebarWidth;
        layout->gridTop = (Integer32)layout->headerHeight;
        layout->pagerTop = (Integer32)layout->height;
        return;
    }

    layout->gridLeft = (Integer32)(layout->sidebarWidth + MENU_MARGIN);
    layout->gridTop = (Integer32)(layout->headerHeight + MENU_MARGIN);
    layout->pagerTop = (Integer32)(layout->height - layout->pagerHeight);

    gridWidth = layout->width - (Unsigned32)layout->gridLeft - MENU_MARGIN;
    gridHeight = (Unsigned32)(layout->pagerTop - layout->gridTop) - MENU_MARGIN;
    layout->columns = gridWidth / layout->tileWidth;
    layout->rows = gridHeight / layout->tileHeight;
    if (layout->columns == 0U)
    {
        layout->columns = 1U;
    }
    if (layout->rows == 0U)
    {
        layout->rows = 1U;
    }
    layout->tooSmall = BOOLEAN_FALSE;
}

static Boolean insideBox(Integer32 x, Integer32 y, Integer32 left, Integer32 top,
                         Unsigned32 width, Unsigned32 height)
{
    return (x >= left && y >= top && x < left + (Integer32)width && y < top + (Integer32)height)
               ? BOOLEAN_TRUE
               : BOOLEAN_FALSE;
}

static void pageButtonBox(const InterfaceMenuLayout *layout, Unsigned32 page, Integer32 *left,
                          Integer32 *top, Unsigned32 *width, Unsigned32 *height)
{
    *left = (Integer32)MENU_MARGIN / 2;
    *top = (Integer32)(layout->headerHeight + MENU_MARGIN +
                       (page * (layout->pageButtonHeight + 6U)));
    *width = layout->sidebarWidth - (Unsigned32)MENU_MARGIN;
    *height = layout->pageButtonHeight;
}

static void tileBox(const InterfaceMenuLayout *layout, Unsigned32 position, Integer32 *left,
                    Integer32 *top)
{
    *left = layout->gridLeft + (Integer32)((position % layout->columns) * layout->tileWidth);
    *top = layout->gridTop + (Integer32)((position / layout->columns) * layout->tileHeight);
}

static void closeBox(const InterfaceMenuLayout *layout, Integer32 *left, Integer32 *top,
                     Unsigned32 *size)
{
    *size = 16U;
    *left = (Integer32)(layout->width - 16U - (MENU_MARGIN / 2U));
    *top = (Integer32)(MENU_MARGIN / 2U);
}

InterfaceMenuHit interfaceMenuHitTest(const InterfaceMenuLayout *layout, const DebugMenu *menu,
                                      Integer32 x, Integer32 y)
{
    InterfaceMenuHit hit;
    Unsigned32 page;
    Unsigned32 position;
    Unsigned32 perPage;
    Unsigned32 start;
    Unsigned32 count;

    hit.target = INTERFACE_MENU_NOTHING;
    hit.value = 0U;
    if (!insideBox(x, y, 0, 0, layout->width, layout->height))
    {
        return hit;
    }

    {
        Integer32 closeLeft;
        Integer32 closeTop;
        Unsigned32 closeSize;

        closeBox(layout, &closeLeft, &closeTop, &closeSize);
        if (insideBox(x, y, closeLeft, closeTop, closeSize, closeSize))
        {
            hit.target = INTERFACE_MENU_CLOSE;
            return hit;
        }
    }

    for (page = 0U; page < (Unsigned32)DEBUG_MENU_PAGE_COUNT; page++)
    {
        Integer32 left;
        Integer32 top;
        Unsigned32 width;
        Unsigned32 height;

        pageButtonBox(layout, page, &left, &top, &width, &height);
        if (insideBox(x, y, left, top, width, height))
        {
            hit.target = INTERFACE_MENU_PAGE;
            hit.value = page;
            return hit;
        }
    }

    if (y >= layout->pagerTop)
    {
        if (insideBox(x, y, layout->gridLeft, layout->pagerTop, PAGER_BUTTON_WIDTH,
                      layout->pagerHeight))
        {
            hit.target = INTERFACE_MENU_PREVIOUS;
        }
        else if (insideBox(x, y, layout->gridLeft + (Integer32)PAGER_BUTTON_WIDTH + 6,
                           layout->pagerTop, PAGER_BUTTON_WIDTH, layout->pagerHeight))
        {
            hit.target = INTERFACE_MENU_NEXT;
        }
        return hit;
    }

    if (layout->tooSmall)
    {
        return hit;
    }
    perPage = INTERFACE_MENU_PER_PAGE(layout);
    start = debugMenuGetPageStart(menu, debugMenuGetPage(menu));
    count = debugMenuGetCount(menu, debugMenuGetPage(menu));
    for (position = 0U; position < perPage; position++)
    {
        Integer32 left;
        Integer32 top;

        if (start + position >= count)
        {
            break;
        }
        tileBox(layout, position, &left, &top);
        if (insideBox(x, y, left, top, layout->tileWidth - TILE_GAP,
                      layout->tileHeight - TILE_GAP))
        {
            hit.target = INTERFACE_MENU_TILE;
            hit.value = start + position;
            return hit;
        }
    }
    return hit;
}

static void drawStandIn(InterfaceSurface *surface, Integer32 left, Integer32 top, Unsigned32 size,
                        const char *name)
{
    Unsigned32 hash = 2166136261U;
    InterfaceColor first;
    InterfaceColor second;
    Unsigned32 cell = size / 4U;
    Unsigned32 row;

    while (name != NULL_POINTER && *name != '\0')
    {
        hash = (hash ^ (Unsigned32)(Unsigned8)*name) * 16777619U;
        name++;
    }
    first.red = (Unsigned8)(70U + (hash & 0x3FU));
    first.green = (Unsigned8)(70U + ((hash >> 6) & 0x3FU));
    first.blue = (Unsigned8)(70U + ((hash >> 12) & 0x3FU));
    first.alpha = 255U;
    second.red = (Unsigned8)(first.red / 2U);
    second.green = (Unsigned8)(first.green / 2U);
    second.blue = (Unsigned8)(first.blue / 2U);
    second.alpha = 255U;

    if (cell == 0U)
    {
        cell = 1U;
    }
    interfaceSurfaceFill(surface, left, top, size, size, second);
    for (row = 0U; row * cell < size; row++)
    {
        Unsigned32 column;

        for (column = 0U; column * cell < size; column++)
        {
            if (((row + column) & 1U) == 0U)
            {
                continue;
            }
            interfaceSurfaceFill(surface, left + (Integer32)(column * cell),
                                 top + (Integer32)(row * cell), cell, cell, first);
        }
    }
}

static MemorySize charactersThatFit(const FontAtlas *atlas, const char *name, MemorySize from,
                                    Unsigned32 room)
{
    char attempt[DEBUG_MENU_NAME_LIMIT + 4U];
    MemorySize taken = 0UL;

    attempt[0] = '\0';
    while (name[from + taken] != '\0' && taken + 1UL < sizeof(attempt))
    {
        char one[2];

        one[0] = name[from + taken];
        one[1] = '\0';
        (void)stringAppend(attempt, sizeof(attempt), one);
        if (fontAtlasMeasureLine(atlas, attempt) > room)
        {
            return taken;
        }
        taken++;
    }
    return taken;
}

static void fitLabel(const FontAtlas *atlas, const char *name, Unsigned32 room, char *first,
                     char *second, MemorySize capacity)
{
    MemorySize length = stringLength(name);
    MemorySize onFirst;
    MemorySize index;

    first[0] = '\0';
    second[0] = '\0';
    onFirst = charactersThatFit(atlas, name, 0UL, room);
    for (index = 0UL; index < onFirst; index++)
    {
        char one[2];

        one[0] = name[index];
        one[1] = '\0';
        (void)stringAppend(first, capacity, one);
    }
    if (onFirst >= length)
    {
        return;
    }

    {
        MemorySize remaining = length - onFirst;
        MemorySize onSecond = charactersThatFit(atlas, name, onFirst, room);

        if (onSecond >= remaining)
        {
            for (index = 0UL; index < remaining; index++)
            {
                char one[2];

                one[0] = name[onFirst + index];
                one[1] = '\0';
                (void)stringAppend(second, capacity, one);
            }
            return;
        }
        (void)stringAppend(second, capacity, "..");
        {
            MemorySize keep = (onSecond > 2UL) ? (onSecond - 2UL) : 0UL;

            for (index = 0UL; index < keep; index++)
            {
                char one[2];

                one[0] = name[length - keep + index];
                one[1] = '\0';
                (void)stringAppend(second, capacity, one);
            }
        }
    }
}

static void drawButton(InterfaceSurface *surface, const FontAtlas *atlas, Integer32 left,
                       Integer32 top, Unsigned32 width, Unsigned32 height, const char *label,
                       Boolean isHovered, Boolean isCurrent)
{
    InterfaceColor fill = isCurrent ? colorButtonCurrent
                                    : (isHovered ? colorButtonHovered : colorButton);

    interfaceSurfaceFill(surface, left, top, width, height, fill);
    interfaceSurfaceBorder(surface, left, top, width, height, 1U, colorEdge);
    if (label != NULL_POINTER)
    {
        Unsigned32 textWidth = fontAtlasMeasureLine(atlas, label);
        Integer32 textLeft = left + (Integer32)((width > textWidth) ? ((width - textWidth) / 2U)
                                                                    : 2U);

        (void)interfaceSurfaceText(surface, atlas, textLeft,
                                   top + (Integer32)((height + atlas->pixelSize) / 2U) - 2,
                                   label, colorLabel);
    }
}

void interfaceMenuDraw(InterfaceSurface *surface, const InterfaceMenuLayout *layout,
                       const DebugMenu *menu, const FontAtlas *atlas, InterfaceMenuHit hovered,
                       InterfaceMenuThumbnail thumbnail, void *thumbnailContext)
{
    DebugMenuPage page = debugMenuGetPage(menu);
    Unsigned32 perPage = INTERFACE_MENU_PER_PAGE(layout);
    Unsigned32 start = debugMenuGetPageStart(menu, page);
    Unsigned32 count = debugMenuGetCount(menu, page);
    Unsigned32 cursor = debugMenuGetCursor(menu, page);
    Unsigned32 index;
    char number[24];
    char line[DEBUG_MENU_NAME_LIMIT + 8U];
    char secondLine[DEBUG_MENU_NAME_LIMIT + 8U];

    interfaceSurfaceFill(surface, 0, 0, layout->width, layout->height, colorPanel);
    interfaceSurfaceFill(surface, 0, 0, layout->sidebarWidth, layout->height, colorSidebar);
    interfaceSurfaceBorder(surface, 0, 0, layout->width, layout->height, 1U, colorEdge);

    (void)interfaceSurfaceText(surface, atlas, (Integer32)MENU_MARGIN,
                               (Integer32)(layout->headerHeight - 8U), "victoria", colorLabel);
    {
        Integer32 closeLeft;
        Integer32 closeTop;
        Unsigned32 closeSize;

        closeBox(layout, &closeLeft, &closeTop, &closeSize);
        drawButton(surface, atlas, closeLeft, closeTop, closeSize, closeSize, "x",
                   (Boolean)(hovered.target == INTERFACE_MENU_CLOSE), BOOLEAN_FALSE);
    }

    for (index = 0U; index < (Unsigned32)DEBUG_MENU_PAGE_COUNT; index++)
    {
        Integer32 left;
        Integer32 top;
        Unsigned32 width;
        Unsigned32 height;

        pageButtonBox(layout, index, &left, &top, &width, &height);
        drawButton(surface, atlas, left, top, width, height, pageNames[index],
                   (Boolean)(hovered.target == INTERFACE_MENU_PAGE && hovered.value == index),
                   (Boolean)(index == (Unsigned32)page));
    }

    if (layout->tooSmall)
    {
        (void)interfaceSurfaceText(surface, atlas, (Integer32)layout->sidebarWidth + 4,
                                   (Integer32)layout->headerHeight + 16, "window too small",
                                   colorLabelDim);
        return;
    }

    if (count == 0U)
    {
        (void)interfaceSurfaceText(surface, atlas, layout->gridLeft, layout->gridTop + 16,
                                   "nothing here yet", colorLabelDim);
    }

    for (index = 0U; index < perPage; index++)
    {
        Unsigned32 row = start + index;
        Integer32 left;
        Integer32 top;
        const Unsigned8 *picture = NULL_POINTER;
        Unsigned32 pictureWidth = 0U;
        Unsigned32 pictureHeight = 0U;
        Boolean isCursor;
        Unsigned32 textWidth;
        Integer32 thumbnailLeft;

        if (row >= count)
        {
            break;
        }
        tileBox(layout, index, &left, &top);
        isCursor = (Boolean)(row == cursor);
        thumbnailLeft = left + (Integer32)(((layout->tileWidth - TILE_GAP) -
                                            layout->thumbnailSize) / 2U);

        interfaceSurfaceFill(surface, left, top, layout->tileWidth - TILE_GAP,
                             layout->tileHeight - TILE_GAP,
                             (hovered.target == INTERFACE_MENU_TILE && hovered.value == row)
                                 ? colorButtonHovered
                                 : colorTileBack);

        if (thumbnail != NULL_POINTER &&
            thumbnail(thumbnailContext, page, row, &picture, &pictureWidth, &pictureHeight) &&
            picture != NULL_POINTER)
        {
            interfaceSurfaceImage(surface, thumbnailLeft, top + 2, layout->thumbnailSize,
                                  layout->thumbnailSize, picture, pictureWidth, pictureHeight);
        }
        else
        {
            drawStandIn(surface, thumbnailLeft, top + 2, layout->thumbnailSize,
                        debugMenuGetRow(menu, page, row));
        }

        if (menu->lists[page].inEffect == row)
        {
            interfaceSurfaceBorder(surface, thumbnailLeft, top + 2, layout->thumbnailSize,
                                   layout->thumbnailSize, 2U, colorInEffect);
        }
        interfaceSurfaceBorder(surface, left, top, layout->tileWidth - TILE_GAP,
                               layout->tileHeight - TILE_GAP, isCursor ? 2U : 1U,
                               isCursor ? colorCursor : colorEdge);

        fitLabel(atlas, debugMenuGetRow(menu, page, row), layout->tileWidth - TILE_GAP - 4U, line,
                 secondLine, sizeof(line));
        textWidth = fontAtlasMeasureLine(atlas, line);
        (void)interfaceSurfaceText(
            surface, atlas,
            left + (Integer32)(((layout->tileWidth - TILE_GAP) > textWidth)
                                   ? (((layout->tileWidth - TILE_GAP) - textWidth) / 2U)
                                   : 2U),
            top + (Integer32)(layout->thumbnailSize + 14U), line,
            isCursor ? colorLabel : colorLabelDim);
        if (secondLine[0] != '\0')
        {
            textWidth = fontAtlasMeasureLine(atlas, secondLine);
            (void)interfaceSurfaceText(
                surface, atlas,
                left + (Integer32)(((layout->tileWidth - TILE_GAP) > textWidth)
                                       ? (((layout->tileWidth - TILE_GAP) - textWidth) / 2U)
                                       : 2U),
                top + (Integer32)(layout->thumbnailSize + 26U), secondLine,
                isCursor ? colorLabel : colorLabelDim);
        }
    }

    drawButton(surface, atlas, layout->gridLeft, layout->pagerTop, PAGER_BUTTON_WIDTH,
               layout->pagerHeight, "prev",
               (Boolean)(hovered.target == INTERFACE_MENU_PREVIOUS), BOOLEAN_FALSE);
    drawButton(surface, atlas, layout->gridLeft + (Integer32)PAGER_BUTTON_WIDTH + 6,
               layout->pagerTop, PAGER_BUTTON_WIDTH, layout->pagerHeight, "next",
               (Boolean)(hovered.target == INTERFACE_MENU_NEXT), BOOLEAN_FALSE);

    line[0] = '\0';
    if (count > 0U)
    {
        (void)stringWriteUnsigned(number, sizeof(number), start + 1U);
        (void)stringAppend(line, sizeof(line), number);
        (void)stringAppend(line, sizeof(line), "-");
        (void)stringWriteUnsigned(number, sizeof(number),
                                  ((start + perPage) < count) ? (start + perPage) : count);
        (void)stringAppend(line, sizeof(line), number);
        (void)stringAppend(line, sizeof(line), " of ");
        (void)stringWriteUnsigned(number, sizeof(number), count);
        (void)stringAppend(line, sizeof(line), number);
        if (menu->lists[page].beyondRoom > 0U)
        {
            (void)stringAppend(line, sizeof(line), "+");
        }
    }
    (void)interfaceSurfaceText(surface, atlas,
                               layout->gridLeft + (Integer32)(2U * PAGER_BUTTON_WIDTH) + 18,
                               layout->pagerTop + (Integer32)(layout->pagerHeight / 2U) + 4, line,
                               colorLabelDim);
}

Boolean interfaceMenuMeasureHint(const FontAtlas *atlas, const char *text, Unsigned32 *width,
                                 Unsigned32 *height)
{
    if (atlas == NULL_POINTER || !atlas->ready || text == NULL_POINTER || text[0] == '\0')
    {
        return BOOLEAN_FALSE;
    }
    *width = fontAtlasMeasureLine(atlas, text) + 16U;
    *height = atlas->lineHeight + 8U;
    return BOOLEAN_TRUE;
}

void interfaceMenuDrawHint(InterfaceSurface *surface, const FontAtlas *atlas, const char *text)
{
    interfaceSurfaceFill(surface, 0, 0, surface->width, surface->height, colorPanel);
    interfaceSurfaceBorder(surface, 0, 0, surface->width, surface->height, 1U, colorEdge);
    (void)interfaceSurfaceText(surface, atlas, 8, (Integer32)(4U + atlas->baseline), text,
                               colorLabelDim);
}
