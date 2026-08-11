#ifndef VICTORIA_INTERFACE_MENU_HEADER
#define VICTORIA_INTERFACE_MENU_HEADER

#include "victoria/coreTypes.h"
#include "victoria/debugMenu.h"
#include "victoria/fontAtlas.h"
#include "victoria/interfaceSurface.h"

typedef enum InterfaceMenuTarget
{
    INTERFACE_MENU_NOTHING = 0,
    INTERFACE_MENU_PAGE,
    INTERFACE_MENU_TILE,
    INTERFACE_MENU_PREVIOUS,
    INTERFACE_MENU_NEXT,
    INTERFACE_MENU_CLOSE
} InterfaceMenuTarget;

typedef struct InterfaceMenuHit
{
    InterfaceMenuTarget target;
    Unsigned32 value;
} InterfaceMenuHit;

typedef struct InterfaceMenuLayout
{
    Unsigned32 width;
    Unsigned32 height;

    Unsigned32 sidebarWidth;
    Unsigned32 headerHeight;
    Unsigned32 pageButtonHeight;

    Integer32 gridLeft;
    Integer32 gridTop;
    Unsigned32 columns;
    Unsigned32 rows;
    Unsigned32 tileWidth;
    Unsigned32 tileHeight;
    Unsigned32 thumbnailSize;

    Integer32 pagerTop;
    Unsigned32 pagerHeight;

    Boolean tooSmall;
} InterfaceMenuLayout;

#define INTERFACE_MENU_PER_PAGE(layout) ((layout)->columns * (layout)->rows)

void interfaceMenuLayout(InterfaceMenuLayout *layout, Unsigned32 availableWidth,
                         Unsigned32 availableHeight);

InterfaceMenuHit interfaceMenuHitTest(const InterfaceMenuLayout *layout, const DebugMenu *menu,
                                      Integer32 x, Integer32 y);

typedef Boolean (*InterfaceMenuThumbnail)(void *context, DebugMenuPage page, Unsigned32 row,
                                          const Unsigned8 **rgbaPixels, Unsigned32 *width,
                                          Unsigned32 *height);

void interfaceMenuDraw(InterfaceSurface *surface, const InterfaceMenuLayout *layout,
                       const DebugMenu *menu, const FontAtlas *atlas, InterfaceMenuHit hovered,
                       InterfaceMenuThumbnail thumbnail, void *thumbnailContext);

void interfaceMenuDrawHint(InterfaceSurface *surface, const FontAtlas *atlas, const char *text);
Boolean interfaceMenuMeasureHint(const FontAtlas *atlas, const char *text, Unsigned32 *width,
                                 Unsigned32 *height);

#endif
