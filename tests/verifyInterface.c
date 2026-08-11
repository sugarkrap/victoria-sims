
#include <stdio.h>

#include "utils/assert.h"
#include "utils/strings.h"
#include "victoria/interfaceMenu.h"

static Integer32 failureCount = 0;

#define SHEET_WIDTH 128U
#define SHEET_HEIGHT 128U
static Unsigned8 sheet[SHEET_WIDTH * SHEET_HEIGHT];

#define SURFACE_WIDTH 800U
#define SURFACE_HEIGHT 480U
static Unsigned8 surfacePixels[SURFACE_WIDTH * SURFACE_HEIGHT * INTERFACE_BYTES_PER_PIXEL];

#define ROW_CAPACITY 64U
static char rows[ROW_CAPACITY][DEBUG_MENU_NAME_LIMIT];

static const Unsigned8 *channelsAt(const InterfaceSurface *surface, Unsigned32 column,
                                   Unsigned32 row)
{
    return &surface->pixels[(((MemorySize)row * surface->width) + column) *
                            INTERFACE_BYTES_PER_PIXEL];
}

static Unsigned32 totalAlpha(const InterfaceSurface *surface)
{
    MemorySize count = (MemorySize)surface->width * (MemorySize)surface->height;
    Unsigned32 total = 0U;
    MemorySize index;

    for (index = 0UL; index < count; index++)
    {
        total += surface->pixels[(index * INTERFACE_BYTES_PER_PIXEL) + 3UL];
    }
    return total;
}

int main(void)
{
    FontAtlas atlas;
    InterfaceSurface surface;
    InterfaceMenuLayout layout;
    DebugMenu menu;
    InterfaceMenuHit hit;
    const InterfaceColor opaqueRed = { 255U, 0U, 0U, 255U };
    const InterfaceColor halfWhite = { 255U, 255U, 255U, 128U };

    fontAtlasBind(&atlas, sheet, SHEET_WIDTH, SHEET_HEIGHT);
    if (!checkThat(&failureCount, "the built-in font gives us something to draw with",
                   fontAtlasBuildFromBuiltin(&atlas, 1U)))
    {
        return checkSummarize(failureCount, "interface");
    }
    interfaceSurfaceBind(&surface, surfacePixels, SURFACE_WIDTH, SURFACE_HEIGHT);

    checkThat(&failureCount, "a surface starts empty",
              surface.width == 0U && surface.height == 0U);
    checkThat(&failureCount, "and refuses a frame bigger than it is",
              !interfaceSurfaceBegin(&surface, SURFACE_WIDTH + 1U, 10U) && surface.width == 0U);
    checkThat(&failureCount, "a frame begins at the size asked for",
              interfaceSurfaceBegin(&surface, 100U, 60U) && surface.width == 100U &&
                  surface.height == 60U);
    checkThat(&failureCount, "cleared to nothing at all", totalAlpha(&surface) == 0U);

    interfaceSurfaceFill(&surface, 10, 10, 20U, 20U, opaqueRed);
    checkThat(&failureCount, "an opaque fill lands where it was put",
              channelsAt(&surface, 15U, 15U)[0] == 255U && channelsAt(&surface, 15U, 15U)[3] == 255U);
    checkThat(&failureCount, "and nowhere else",
              channelsAt(&surface, 9U, 15U)[3] == 0U && channelsAt(&surface, 30U, 15U)[3] == 0U);

    (void)interfaceSurfaceBegin(&surface, 40U, 40U);
    interfaceSurfaceFill(&surface, 0, 0, 40U, 40U, halfWhite);
    checkThat(&failureCount, "a half-opaque white is stored already multiplied by its alpha",
              channelsAt(&surface, 5U, 5U)[0] == 128U && channelsAt(&surface, 5U, 5U)[3] == 128U);
    interfaceSurfaceFill(&surface, 0, 0, 40U, 40U, halfWhite);
    checkThat(&failureCount, "and two of them together are more opaque than one",
              channelsAt(&surface, 5U, 5U)[3] > 180U && channelsAt(&surface, 5U, 5U)[3] < 200U);

    (void)interfaceSurfaceBegin(&surface, 40U, 40U);
    interfaceSurfaceFill(&surface, -10, -10, 20U, 20U, opaqueRed);
    checkThat(&failureCount, "a fill starting off the edge draws the part that is on it",
              channelsAt(&surface, 0U, 0U)[3] == 255U && channelsAt(&surface, 9U, 9U)[3] == 255U &&
                  channelsAt(&surface, 10U, 10U)[3] == 0U);
    interfaceSurfaceFill(&surface, 100, 100, 20U, 20U, opaqueRed);
    checkThat(&failureCount, "and one wholly off it draws nothing and says so",
              surface.drawsClipped == 1U);

    (void)interfaceSurfaceBegin(&surface, 40U, 40U);
    interfaceSurfaceBorder(&surface, 0, 0, 40U, 40U, 2U, halfWhite);
    checkThat(&failureCount, "a border is drawn on the edge and not in the middle",
              channelsAt(&surface, 0U, 0U)[3] == 128U && channelsAt(&surface, 20U, 20U)[3] == 0U);
    checkThat(&failureCount, "and its corners are no darker than its sides",
              channelsAt(&surface, 0U, 0U)[3] == channelsAt(&surface, 20U, 0U)[3]);

    {
        Integer32 pen;

        (void)interfaceSurfaceBegin(&surface, 200U, 40U);
        pen = interfaceSurfaceText(&surface, &atlas, 4, 20, "menu", opaqueRed);
        checkThat(&failureCount, "text advances the pen by what it measures",
                  pen == 4 + (Integer32)fontAtlasMeasureLine(&atlas, "menu"));
        checkThat(&failureCount, "and puts ink on the surface", totalAlpha(&surface) > 0U);
        (void)interfaceSurfaceText(&surface, &atlas, 4, 20, "a\tb", opaqueRed);
        checkThat(&failureCount, "a character the atlas has no glyph for is counted",
                  surface.charactersMissing == 1U);
    }

    {
        static Unsigned8 picture[2 * 2 * 4];

        picture[0] = 255U; picture[1] = 0U;   picture[2] = 0U;   picture[3] = 255U;
        picture[4] = 0U;   picture[5] = 255U; picture[6] = 0U;   picture[7] = 255U;
        picture[8] = 0U;   picture[9] = 0U;   picture[10] = 255U; picture[11] = 255U;
        picture[12] = 255U; picture[13] = 255U; picture[14] = 255U; picture[15] = 255U;

        (void)interfaceSurfaceBegin(&surface, 40U, 40U);
        interfaceSurfaceImage(&surface, 0, 0, 40U, 40U, picture, 2U, 2U);
        checkThat(&failureCount, "an image's top left corner comes from its top left",
                  channelsAt(&surface, 2U, 2U)[0] == 255U && channelsAt(&surface, 2U, 2U)[1] == 0U);
        checkThat(&failureCount, "and its bottom right from its bottom right",
                  channelsAt(&surface, 38U, 38U)[0] == 255U &&
                      channelsAt(&surface, 38U, 38U)[2] == 255U);
        checkThat(&failureCount, "and its top right from its top right",
                  channelsAt(&surface, 38U, 2U)[1] == 255U &&
                      channelsAt(&surface, 38U, 2U)[0] == 0U);
    }

    debugMenuInitialize(&menu);
    debugMenuBindPage(&menu, DEBUG_MENU_PAGE_BODY, rows, ROW_CAPACITY);
    {
        Unsigned32 index;

        for (index = 0U; index < ROW_CAPACITY; index++)
        {
            char name[8];
            char number[8];

            name[0] = '\0';
            (void)stringAppend(name, sizeof(name), "row");
            (void)stringWriteUnsigned(number, sizeof(number), index);
            (void)stringAppend(name, sizeof(name), number);
            (void)debugMenuAddRow(&menu, DEBUG_MENU_PAGE_BODY, name);
        }
    }
    debugMenuSetOpen(&menu, BOOLEAN_TRUE);

    interfaceMenuLayout(&layout, 1280U, 720U);
    checkThat(&failureCount, "a menu on a big window lays out a grid",
              !layout.tooSmall && layout.columns > 1U && layout.rows > 1U);
    checkThat(&failureCount, "and does not fill the window it is describing",
              layout.width < 1280U && layout.height < 720U);
    debugMenuSetGrid(&menu, DEBUG_MENU_PAGE_BODY, layout.columns,
                     INTERFACE_MENU_PER_PAGE(&layout));

    checkThat(&failureCount, "a point outside the menu is nothing",
              interfaceMenuHitTest(&layout, &menu, (Integer32)layout.width + 5, 10).target ==
                  INTERFACE_MENU_NOTHING);

    {
        Unsigned32 page;
        Boolean allFound = BOOLEAN_TRUE;

        for (page = 0U; page < (Unsigned32)DEBUG_MENU_PAGE_COUNT; page++)
        {
            Integer32 y = (Integer32)(layout.headerHeight + 8U +
                                      (page * (layout.pageButtonHeight + 6U)));

            hit = interfaceMenuHitTest(&layout, &menu, 6, y);
            if (hit.target != INTERFACE_MENU_PAGE || hit.value != page)
            {
                allFound = BOOLEAN_FALSE;
            }
        }
        checkThat(&failureCount, "each page button is where the sidebar draws it", allFound);
    }

    hit = interfaceMenuHitTest(&layout, &menu, layout.gridLeft + 4, layout.gridTop + 4);
    checkThat(&failureCount, "the first tile is the first row",
              hit.target == INTERFACE_MENU_TILE && hit.value == 0U);
    hit = interfaceMenuHitTest(&layout, &menu, layout.gridLeft + (Integer32)layout.tileWidth - 2,
                               layout.gridTop + 4);
    checkThat(&failureCount, "and the gap beside it belongs to nothing",
              hit.target == INTERFACE_MENU_NOTHING);
    hit = interfaceMenuHitTest(&layout, &menu, layout.gridLeft + (Integer32)layout.tileWidth + 4,
                               layout.gridTop + 4);
    checkThat(&failureCount, "and the tile after the gap is the second row",
              hit.target == INTERFACE_MENU_TILE && hit.value == 1U);
    hit = interfaceMenuHitTest(&layout, &menu, layout.gridLeft + 4,
                               layout.gridTop + (Integer32)layout.tileHeight + 4);
    checkThat(&failureCount, "and the tile below the first is a whole row on",
              hit.target == INTERFACE_MENU_TILE && hit.value == layout.columns);

    checkThat(&failureCount, "the pager moves a whole page", debugMenuStepPage(&menu, 1));
    hit = interfaceMenuHitTest(&layout, &menu, layout.gridLeft + 4, layout.gridTop + 4);
    checkThat(&failureCount, "and the first tile of the second page is not the first row",
              hit.target == INTERFACE_MENU_TILE &&
                  hit.value == INTERFACE_MENU_PER_PAGE(&layout));
    (void)debugMenuStepPage(&menu, -1);

    hit = interfaceMenuHitTest(&layout, &menu, layout.gridLeft + 4,
                               layout.pagerTop + (Integer32)(layout.pagerHeight / 2U));
    checkThat(&failureCount, "the previous button is under the grid",
              hit.target == INTERFACE_MENU_PREVIOUS);
    hit = interfaceMenuHitTest(&layout, &menu, layout.gridLeft + 70,
                               layout.pagerTop + (Integer32)(layout.pagerHeight / 2U));
    checkThat(&failureCount, "and the next button beside it", hit.target == INTERFACE_MENU_NEXT);
    hit = interfaceMenuHitTest(&layout, &menu, (Integer32)layout.width - 8, 8);
    checkThat(&failureCount, "and the close button is in the corner",
              hit.target == INTERFACE_MENU_CLOSE);

    {
        InterfaceMenuLayout cramped;

        interfaceMenuLayout(&cramped, 200U, 80U);
        checkThat(&failureCount, "a window with no room for a grid says so rather than vanishing",
                  cramped.tooSmall && cramped.width > 0U && cramped.height > 0U);
        checkThat(&failureCount, "and nothing in its grid can be hit",
                  interfaceMenuHitTest(&cramped, &menu, (Integer32)cramped.width / 2,
                                       (Integer32)cramped.height / 2)
                          .target != INTERFACE_MENU_TILE);
    }

    interfaceMenuLayout(&layout, 1280U, 720U);
    if (checkThat(&failureCount, "the menu fits the surface",
                  interfaceSurfaceBegin(&surface, layout.width, layout.height)))
    {
        InterfaceMenuHit nothing;
        Unsigned32 plain;
        Unsigned32 hovered;

        nothing.target = INTERFACE_MENU_NOTHING;
        nothing.value = 0U;
        debugMenuSetInEffect(&menu, DEBUG_MENU_PAGE_BODY, 0U);
        interfaceMenuDraw(&surface, &layout, &menu, &atlas, nothing, NULL_POINTER, NULL_POINTER);
        plain = totalAlpha(&surface);
        checkThat(&failureCount, "drawing it fills the surface", plain > 0U);
        checkThat(&failureCount, "every pixel of the panel is covered",
                  channelsAt(&surface, 1U, 1U)[3] > 0U &&
                      channelsAt(&surface, layout.width - 2U, layout.height - 2U)[3] > 0U);
        checkThat(&failureCount, "and every label found a glyph", surface.charactersMissing == 0U);

        hit.target = INTERFACE_MENU_TILE;
        hit.value = 0U;
        (void)interfaceSurfaceBegin(&surface, layout.width, layout.height);
        interfaceMenuDraw(&surface, &layout, &menu, &atlas, hit, NULL_POINTER, NULL_POINTER);
        hovered = totalAlpha(&surface);
        checkThat(&failureCount, "a tile under the pointer is drawn differently from one that is not",
                  hovered != plain);
    }

    {
        Unsigned32 width = 0U;
        Unsigned32 height = 0U;

        checkThat(&failureCount, "the shut menu measures its one line",
                  interfaceMenuMeasureHint(&atlas, "menu: press m", &width, &height) &&
                      width > 0U && height > 0U);
        (void)interfaceSurfaceBegin(&surface, width, height);
        interfaceMenuDrawHint(&surface, &atlas, "menu: press m");
        checkThat(&failureCount, "and draws it", totalAlpha(&surface) > 0U);
    }

    {
        InterfaceSurface unbound;

        interfaceSurfaceBind(&unbound, NULL_POINTER, 64U, 64U);
        checkThat(&failureCount, "a surface bound to nothing draws nothing rather than crashing",
                  !interfaceSurfaceBegin(&unbound, 10U, 10U));
        interfaceSurfaceFill(&unbound, 0, 0, 10U, 10U, opaqueRed);
        checkThat(&failureCount, "and its draws are refused too",
                  interfaceSurfaceText(&unbound, &atlas, 0, 0, "x", opaqueRed) == 0);
    }

    return checkSummarize(failureCount, "interface");
}
