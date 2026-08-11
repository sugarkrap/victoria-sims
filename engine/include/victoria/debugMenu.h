#ifndef VICTORIA_DEBUG_MENU_HEADER
#define VICTORIA_DEBUG_MENU_HEADER

#include "victoria/coreTypes.h"

#define DEBUG_MENU_PAGE_COUNT 3U
#define DEBUG_MENU_NAME_LIMIT 64U
#define DEBUG_MENU_WINDOW 12U

typedef enum DebugMenuPage
{
    DEBUG_MENU_PAGE_BODY = 0,
    DEBUG_MENU_PAGE_CLOTHING,
    DEBUG_MENU_PAGE_ANIMATION
} DebugMenuPage;

typedef struct DebugMenuList
{
    char (*rows)[DEBUG_MENU_NAME_LIMIT];
    Unsigned32 capacity;
    Unsigned32 count;
    Unsigned32 cursor;
    Unsigned32 inEffect;
    Unsigned32 beyondRoom;

    Unsigned32 columns;
    Unsigned32 perPage;
} DebugMenuList;

#define DEBUG_MENU_NONE 0xFFFFFFFFUL

typedef struct DebugMenu
{
    Boolean isOpen;
    DebugMenuPage page;
    DebugMenuList lists[DEBUG_MENU_PAGE_COUNT];
} DebugMenu;

typedef enum DebugMenuResult
{
    DEBUG_MENU_IGNORED = 0,
    DEBUG_MENU_MOVED,
    DEBUG_MENU_CHOSE
} DebugMenuResult;

void debugMenuInitialize(DebugMenu *menu);

void debugMenuBindPage(DebugMenu *menu, DebugMenuPage page,
                       char (*storage)[DEBUG_MENU_NAME_LIMIT], Unsigned32 capacity);

Unsigned32 debugMenuAddRow(DebugMenu *menu, DebugMenuPage page, const char *name);

void debugMenuClearPage(DebugMenu *menu, DebugMenuPage page);

void debugMenuSetInEffect(DebugMenu *menu, DebugMenuPage page, Unsigned32 row);

void debugMenuSetGrid(DebugMenu *menu, DebugMenuPage page, Unsigned32 columns,
                      Unsigned32 perPage);

DebugMenuResult debugMenuHandleKey(DebugMenu *menu, char key);

Boolean debugMenuSetCursor(DebugMenu *menu, DebugMenuPage page, Unsigned32 row);

Boolean debugMenuSetPage(DebugMenu *menu, DebugMenuPage page);

void debugMenuSetOpen(DebugMenu *menu, Boolean isOpen);

Boolean debugMenuStepPage(DebugMenu *menu, Integer32 direction);

Unsigned32 debugMenuGetPageStart(const DebugMenu *menu, DebugMenuPage page);
Unsigned32 debugMenuGetPerPage(const DebugMenu *menu, DebugMenuPage page);
Unsigned32 debugMenuGetColumns(const DebugMenu *menu, DebugMenuPage page);

Unsigned32 debugMenuGetCursor(const DebugMenu *menu, DebugMenuPage page);
Unsigned32 debugMenuGetCount(const DebugMenu *menu, DebugMenuPage page);
const char *debugMenuGetRow(const DebugMenu *menu, DebugMenuPage page, Unsigned32 row);
Boolean debugMenuIsOpen(const DebugMenu *menu);
DebugMenuPage debugMenuGetPage(const DebugMenu *menu);

void debugMenuWriteText(const DebugMenu *menu, char *destination, MemorySize capacity);

#endif
