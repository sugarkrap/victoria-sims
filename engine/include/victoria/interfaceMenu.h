#ifndef VICTORIA_INTERFACE_MENU_HEADER
#define VICTORIA_INTERFACE_MENU_HEADER

#include "victoria/coreTypes.h"
#include "victoria/debugMenu.h"
#include "victoria/fontAtlas.h"
#include "victoria/interfaceSurface.h"

/* Where the menu's pieces are, and what is under the pointer.
 *
 * Layout and hit testing are the same knowledge looked at from two directions,
 * so they live in one file: a button drawn somewhere the pointer is not tested
 * is a button that does not work, and the only way to be sure they agree is for
 * one calculation to answer both. Everything here is arithmetic on integers
 * with no state of its own — the menu's state is in debugMenu.h and the pixels
 * are in interfaceSurface.h — which means the whole of it can be checked
 * without a window, a font or a disc.
 *
 * The shape is a sidebar of pages down the left and a grid of tiles beside it,
 * with a pager underneath. That is what Create-A-Sim looks like, and it is the
 * arrangement a thumbnail is for: the picture is the thing being chosen and the
 * name is its caption. */

typedef enum InterfaceMenuTarget
{
    INTERFACE_MENU_NOTHING = 0,
    /* A page button in the sidebar. The value is the page. */
    INTERFACE_MENU_PAGE,
    /* A tile in the grid. The value is the row, not the position on screen. */
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

    /* True when the space given was too small to lay a grid out in at all. The
       menu then draws its chrome and says so, rather than drawing nothing and
       looking broken. */
    Boolean tooSmall;
} InterfaceMenuLayout;

/* How many tiles this layout shows. The menu's own page size is set from it, so
   the pager and the grid cannot disagree about what a page is. */
#define INTERFACE_MENU_PER_PAGE(layout) ((layout)->columns * (layout)->rows)

/* Works out where everything goes for the space available. Does not touch the
   menu; call debugMenuSetGrid with the result to keep the keys in step. */
void interfaceMenuLayout(InterfaceMenuLayout *layout, Unsigned32 availableWidth,
                         Unsigned32 availableHeight);

/* What is at this point, in surface coordinates. Anything outside the menu, or
   in the gaps between things, answers NOTHING — which is what stops a click on
   the space beside a tile choosing the tile. */
InterfaceMenuHit interfaceMenuHitTest(const InterfaceMenuLayout *layout, const DebugMenu *menu,
                                      Integer32 x, Integer32 y);

/* A picture for one tile, supplied by whoever knows where pictures come from.
 *
 * A callback rather than a field, because this module has no business knowing
 * what a texture is or which of them belongs to a garment. Answering false is
 * the ordinary case and means "draw the stand-in" — most rows have no picture
 * and some never will. */
typedef Boolean (*InterfaceMenuThumbnail)(void *context, DebugMenuPage page, Unsigned32 row,
                                          const Unsigned8 **rgbaPixels, Unsigned32 *width,
                                          Unsigned32 *height);

/* Draws the whole menu into the surface, which must already have been begun at
   the size the layout was made for. hovered is what the pointer is over, so the
   thing about to be clicked lights up before it is. */
void interfaceMenuDraw(InterfaceSurface *surface, const InterfaceMenuLayout *layout,
                       const DebugMenu *menu, const FontAtlas *atlas, InterfaceMenuHit hovered,
                       InterfaceMenuThumbnail thumbnail, void *thumbnailContext);

/* The one line shown when the menu is shut. Drawn by the same code path so
   there is one answer to "how big is the interface" rather than two. */
void interfaceMenuDrawHint(InterfaceSurface *surface, const FontAtlas *atlas, const char *text);
Boolean interfaceMenuMeasureHint(const FontAtlas *atlas, const char *text, Unsigned32 *width,
                                 Unsigned32 *height);

#endif
