#ifndef VICTORIA_DEBUG_MENU_HEADER
#define VICTORIA_DEBUG_MENU_HEADER

#include "victoria/coreTypes.h"

/* A menu for choosing what to look at: who the Sim is, what it wears, what it
 * is doing.
 *
 * All three were command-line flags, which means every question costs a restart
 * and a four-minute disc load. There are twelve archetypes on the tested disc,
 * a few hundred garments per slot and eleven thousand animations, and a
 * question you can only ask once per load is a question nobody asks.
 *
 * It draws NOTHING. The engine formats it as plain text and the platform
 * displays it, exactly as the profiler report already works — printed to the
 * terminal on Linux, put in an element on the web. That is not a shortcut: the
 * floor of the device ladder has no programmable shading, so anything drawn on
 * screen would need a bitmap font and a 2D path in three backends, one of which
 * has no shaders to draw it with. Reusing the report path costs none of that
 * and works everywhere for free.
 *
 * This module is the state: which page, where the cursor is, what the rows say
 * and which keys do what. It holds no resources and reads nothing — what a row
 * MEANS is the engine's business, kept in arrays beside these and indexed by
 * the same row number. So the part with the fiddly rules in it can be checked
 * against a claim rather than against a disc. */

#define DEBUG_MENU_PAGE_COUNT 3U
#define DEBUG_MENU_NAME_LIMIT 64U
/* How many rows of a page are shown at once. A terminal scrolls and a browser
   element does not, so the window is what keeps a page of eleven thousand
   animations readable in both. */
#define DEBUG_MENU_WINDOW 12U

typedef enum DebugMenuPage
{
    DEBUG_MENU_PAGE_BODY = 0,
    DEBUG_MENU_PAGE_CLOTHING,
    DEBUG_MENU_PAGE_ANIMATION
} DebugMenuPage;

/* One page's rows. The storage belongs to the caller — the arena, not a static
   here — because eleven thousand animation names is three quarters of a
   megabyte and this engine counts kilobytes. */
typedef struct DebugMenuList
{
    char (*rows)[DEBUG_MENU_NAME_LIMIT];
    Unsigned32 capacity;
    Unsigned32 count;
    Unsigned32 cursor;
    /* Which row is currently in effect, or DEBUG_MENU_NONE when the engine has
       not said. Shown apart from the cursor: the row you are looking at and the
       row you are wearing are different things, and a menu that conflated them
       would make it impossible to see what you had before choosing. */
    Unsigned32 inEffect;
    /* Rows that would not fit. Counted and reported rather than dropped in
       silence — a list that stops at its capacity and says nothing looks
       exactly like a disc that holds no more. */
    Unsigned32 beyondRoom;
} DebugMenuList;

#define DEBUG_MENU_NONE 0xFFFFFFFFUL

typedef struct DebugMenu
{
    Boolean isOpen;
    DebugMenuPage page;
    DebugMenuList lists[DEBUG_MENU_PAGE_COUNT];
} DebugMenu;

/* What a keystroke did, so the caller knows whether to act and whether to
   redraw. Anything that changes what is on screen returns MOVED at least. */
typedef enum DebugMenuResult
{
    DEBUG_MENU_IGNORED = 0,
    DEBUG_MENU_MOVED,
    /* The row under the cursor was chosen. The caller reads the cursor of the
       current page and does whatever that means. */
    DEBUG_MENU_CHOSE
} DebugMenuResult;

void debugMenuInitialize(DebugMenu *menu);

/* Gives a page somewhere to keep its rows. Until this is called the page is
   empty, which is what a list nothing has filled in yet should look like. */
void debugMenuBindPage(DebugMenu *menu, DebugMenuPage page,
                       char (*storage)[DEBUG_MENU_NAME_LIMIT], Unsigned32 capacity);

/* Appends a row, or counts it as beyond room. Returns the row number, or
   DEBUG_MENU_NONE if it did not fit. */
Unsigned32 debugMenuAddRow(DebugMenu *menu, DebugMenuPage page, const char *name);

/* Empties a page, keeping its storage. For a list that has to be rebuilt — the
   clothing changes the moment the Sim does. */
void debugMenuClearPage(DebugMenu *menu, DebugMenuPage page);

/* Says which row is in effect now, which is not the same as where the cursor
   is. DEBUG_MENU_NONE for none. */
void debugMenuSetInEffect(DebugMenu *menu, DebugMenuPage page, Unsigned32 row);

DebugMenuResult debugMenuHandleKey(DebugMenu *menu, char key);

Unsigned32 debugMenuGetCursor(const DebugMenu *menu, DebugMenuPage page);
Unsigned32 debugMenuGetCount(const DebugMenu *menu, DebugMenuPage page);
const char *debugMenuGetRow(const DebugMenu *menu, DebugMenuPage page, Unsigned32 row);
Boolean debugMenuIsOpen(const DebugMenu *menu);
DebugMenuPage debugMenuGetPage(const DebugMenu *menu);

/* Renders the whole thing into destination. Always terminates. Writes the help
   line when closed, so there is always something saying the menu exists — a
   debug feature nobody can discover is a debug feature nobody has. */
void debugMenuWriteText(const DebugMenu *menu, char *destination, MemorySize capacity);

#endif
