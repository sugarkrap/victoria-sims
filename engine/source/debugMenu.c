#include "victoria/debugMenu.h"

#include "utils/strings.h"

/* The keys, in one place.
 *
 * Letters and not arrows: an arrow key is an escape sequence on a terminal and
 * a named code in a browser, so it would have to be recognised twice and
 * agreed on. A letter is one character on both, and the platform layer can go
 * on being the dumb pipe it is. */
#define KEY_TOGGLE 'm'
#define KEY_CLOSE 'q'
#define KEY_UP 'k'
#define KEY_DOWN 'j'
#define KEY_PAGE_UP 'u'
#define KEY_PAGE_DOWN 'i'
#define KEY_LEFT 'h'
#define KEY_RIGHT 'l'
#define KEY_CHOOSE '\r'

static const char *const pageNames[DEBUG_MENU_PAGE_COUNT] = { "body", "clothing", "animation" };

void debugMenuInitialize(DebugMenu *menu)
{
    Unsigned32 page;

    menu->isOpen = BOOLEAN_FALSE;
    menu->page = DEBUG_MENU_PAGE_BODY;
    for (page = 0U; page < DEBUG_MENU_PAGE_COUNT; page++)
    {
        menu->lists[page].rows = NULL_POINTER;
        menu->lists[page].capacity = 0U;
        menu->lists[page].count = 0U;
        menu->lists[page].cursor = 0U;
        menu->lists[page].inEffect = (Unsigned32)DEBUG_MENU_NONE;
        menu->lists[page].beyondRoom = 0U;
        /* One column and a text window: the arrangement there was before
           anything was drawn, so a page nobody has laid out behaves exactly as
           it always did. */
        menu->lists[page].columns = 1U;
        menu->lists[page].perPage = (Unsigned32)DEBUG_MENU_WINDOW;
    }
}

void debugMenuSetGrid(DebugMenu *menu, DebugMenuPage page, Unsigned32 columns,
                      Unsigned32 perPage)
{
    if ((Unsigned32)page >= DEBUG_MENU_PAGE_COUNT)
    {
        return;
    }
    menu->lists[page].columns = (columns == 0U) ? 1U : columns;
    menu->lists[page].perPage = (perPage == 0U) ? 1U : perPage;
}

Unsigned32 debugMenuGetColumns(const DebugMenu *menu, DebugMenuPage page)
{
    return ((Unsigned32)page < DEBUG_MENU_PAGE_COUNT) ? menu->lists[page].columns : 1U;
}

Unsigned32 debugMenuGetPerPage(const DebugMenu *menu, DebugMenuPage page)
{
    return ((Unsigned32)page < DEBUG_MENU_PAGE_COUNT) ? menu->lists[page].perPage : 1U;
}

/* Which page of tiles the cursor is on, expressed as the row it starts at.
 *
 * Derived from the cursor rather than stored beside it, so the two cannot
 * disagree — a stored page and a cursor that moved off it is a grid showing one
 * set of tiles with the highlight on none of them. */
Unsigned32 debugMenuGetPageStart(const DebugMenu *menu, DebugMenuPage page)
{
    const DebugMenuList *list;

    if ((Unsigned32)page >= DEBUG_MENU_PAGE_COUNT)
    {
        return 0U;
    }
    list = &menu->lists[page];
    return (list->cursor / list->perPage) * list->perPage;
}

void debugMenuBindPage(DebugMenu *menu, DebugMenuPage page,
                       char (*storage)[DEBUG_MENU_NAME_LIMIT], Unsigned32 capacity)
{
    if ((Unsigned32)page >= DEBUG_MENU_PAGE_COUNT)
    {
        return;
    }
    menu->lists[page].rows = storage;
    menu->lists[page].capacity = (storage != NULL_POINTER) ? capacity : 0U;
    menu->lists[page].count = 0U;
    menu->lists[page].cursor = 0U;
    menu->lists[page].beyondRoom = 0U;
}

Unsigned32 debugMenuAddRow(DebugMenu *menu, DebugMenuPage page, const char *name)
{
    DebugMenuList *list;

    if ((Unsigned32)page >= DEBUG_MENU_PAGE_COUNT)
    {
        return (Unsigned32)DEBUG_MENU_NONE;
    }
    list = &menu->lists[page];
    if (list->rows == NULL_POINTER || list->count >= list->capacity)
    {
        list->beyondRoom++;
        return (Unsigned32)DEBUG_MENU_NONE;
    }
    list->rows[list->count][0] = '\0';
    stringAppend(list->rows[list->count], (MemorySize)DEBUG_MENU_NAME_LIMIT,
                 (name != NULL_POINTER) ? name : "");
    list->count++;
    return list->count - 1U;
}

void debugMenuClearPage(DebugMenu *menu, DebugMenuPage page)
{
    if ((Unsigned32)page >= DEBUG_MENU_PAGE_COUNT)
    {
        return;
    }
    menu->lists[page].count = 0U;
    menu->lists[page].cursor = 0U;
    menu->lists[page].inEffect = (Unsigned32)DEBUG_MENU_NONE;
    menu->lists[page].beyondRoom = 0U;
}

Boolean debugMenuSetCursor(DebugMenu *menu, DebugMenuPage page, Unsigned32 row)
{
    DebugMenuList *list;

    if ((Unsigned32)page >= DEBUG_MENU_PAGE_COUNT)
    {
        return BOOLEAN_FALSE;
    }
    list = &menu->lists[page];
    if (row >= list->count || row == list->cursor)
    {
        return BOOLEAN_FALSE;
    }
    list->cursor = row;
    return BOOLEAN_TRUE;
}

Boolean debugMenuSetPage(DebugMenu *menu, DebugMenuPage page)
{
    if ((Unsigned32)page >= DEBUG_MENU_PAGE_COUNT || menu->page == page)
    {
        return BOOLEAN_FALSE;
    }
    menu->page = page;
    return BOOLEAN_TRUE;
}

void debugMenuSetOpen(DebugMenu *menu, Boolean isOpen)
{
    menu->isOpen = isOpen;
}

void debugMenuSetInEffect(DebugMenu *menu, DebugMenuPage page, Unsigned32 row)
{
    if ((Unsigned32)page >= DEBUG_MENU_PAGE_COUNT)
    {
        return;
    }
    menu->lists[page].inEffect = row;
}

Unsigned32 debugMenuGetCursor(const DebugMenu *menu, DebugMenuPage page)
{
    return ((Unsigned32)page < DEBUG_MENU_PAGE_COUNT) ? menu->lists[page].cursor : 0U;
}

Unsigned32 debugMenuGetCount(const DebugMenu *menu, DebugMenuPage page)
{
    return ((Unsigned32)page < DEBUG_MENU_PAGE_COUNT) ? menu->lists[page].count : 0U;
}

const char *debugMenuGetRow(const DebugMenu *menu, DebugMenuPage page, Unsigned32 row)
{
    if ((Unsigned32)page >= DEBUG_MENU_PAGE_COUNT || row >= menu->lists[page].count ||
        menu->lists[page].rows == NULL_POINTER)
    {
        return "";
    }
    return menu->lists[page].rows[row];
}

Boolean debugMenuIsOpen(const DebugMenu *menu)
{
    return menu->isOpen;
}

DebugMenuPage debugMenuGetPage(const DebugMenu *menu)
{
    return menu->page;
}

/* Moves the cursor by a signed step, stopping at the ends rather than wrapping.
 *
 * Stopping and not wrapping, deliberately: a list of eleven thousand animations
 * that jumps from the top to the bottom on one keystroke has lost the reader,
 * and there is no way to tell a wrap from a redraw by looking. */
static Boolean moveCursor(DebugMenuList *list, Integer32 step)
{
    Unsigned32 before = list->cursor;

    if (list->count == 0U)
    {
        list->cursor = 0U;
        return BOOLEAN_FALSE;
    }
    if (step < 0)
    {
        Unsigned32 back = (Unsigned32)(-step);

        list->cursor = (list->cursor > back) ? (list->cursor - back) : 0U;
    }
    else
    {
        list->cursor += (Unsigned32)step;
        if (list->cursor >= list->count)
        {
            list->cursor = list->count - 1U;
        }
    }
    return (list->cursor != before) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

DebugMenuResult debugMenuHandleKey(DebugMenu *menu, char key)
{
    DebugMenuList *list;
    char folded = characterToLowerCase(key);

    /* Opening is the only thing a keystroke does while it is shut, so nothing
       else can be triggered by accident by a Sim walking past the keyboard. */
    if (!menu->isOpen)
    {
        if (folded == KEY_TOGGLE)
        {
            menu->isOpen = BOOLEAN_TRUE;
            return DEBUG_MENU_MOVED;
        }
        return DEBUG_MENU_IGNORED;
    }

    if (folded == KEY_TOGGLE || folded == KEY_CLOSE)
    {
        menu->isOpen = BOOLEAN_FALSE;
        return DEBUG_MENU_MOVED;
    }
    if (key >= '1' && key <= '9')
    {
        Unsigned32 wanted = (Unsigned32)(key - '1');

        if (wanted < DEBUG_MENU_PAGE_COUNT)
        {
            menu->page = (DebugMenuPage)wanted;
            return DEBUG_MENU_MOVED;
        }
        return DEBUG_MENU_IGNORED;
    }

    list = &menu->lists[menu->page];
    /* Up and down move by a whole row of however the page is arranged, so on a
       grid of four columns down is four tiles on. On a plain list the column
       count is one and this is the single step it always was. */
    if (folded == KEY_UP)
    {
        return moveCursor(list, -(Integer32)list->columns) ? DEBUG_MENU_MOVED
                                                           : DEBUG_MENU_IGNORED;
    }
    if (folded == KEY_DOWN)
    {
        return moveCursor(list, (Integer32)list->columns) ? DEBUG_MENU_MOVED : DEBUG_MENU_IGNORED;
    }
    if (folded == KEY_LEFT)
    {
        return moveCursor(list, -1) ? DEBUG_MENU_MOVED : DEBUG_MENU_IGNORED;
    }
    if (folded == KEY_RIGHT)
    {
        return moveCursor(list, 1) ? DEBUG_MENU_MOVED : DEBUG_MENU_IGNORED;
    }
    if (folded == KEY_PAGE_UP)
    {
        return moveCursor(list, -(Integer32)list->perPage) ? DEBUG_MENU_MOVED
                                                           : DEBUG_MENU_IGNORED;
    }
    if (folded == KEY_PAGE_DOWN)
    {
        return moveCursor(list, (Integer32)list->perPage) ? DEBUG_MENU_MOVED
                                                          : DEBUG_MENU_IGNORED;
    }
    if (key == KEY_CHOOSE || key == '\n' || key == ' ')
    {
        /* An empty page has nothing to choose. Answering CHOSE there would have
           the caller act on row nought of nothing. */
        return (list->count > 0U) ? DEBUG_MENU_CHOSE : DEBUG_MENU_IGNORED;
    }
    return DEBUG_MENU_IGNORED;
}

/* Which rows to show, so the cursor stays inside the window rather than the
   window starting at the top of eleven thousand rows. */
static Unsigned32 windowStart(const DebugMenuList *list)
{
    Unsigned32 half = (Unsigned32)DEBUG_MENU_WINDOW / 2U;

    if (list->count <= (Unsigned32)DEBUG_MENU_WINDOW || list->cursor < half)
    {
        return 0U;
    }
    if (list->cursor + half >= list->count)
    {
        return list->count - (Unsigned32)DEBUG_MENU_WINDOW;
    }
    return list->cursor - half;
}

void debugMenuWriteText(const DebugMenu *menu, char *destination, MemorySize capacity)
{
    const DebugMenuList *list;
    Unsigned32 first;
    Unsigned32 row;
    Unsigned32 page;
    char number[16];

    if (destination == NULL_POINTER || capacity == 0UL)
    {
        return;
    }
    destination[0] = '\0';
    if (!menu->isOpen)
    {
        /* Always said, even shut. A debug feature nobody can discover is a
           debug feature nobody has. */
        stringAppend(destination, capacity, "menu: press m\n");
        return;
    }

    stringAppend(destination, capacity, "menu:");
    for (page = 0U; page < DEBUG_MENU_PAGE_COUNT; page++)
    {
        stringAppend(destination, capacity, " ");
        stringWriteUnsigned(number, sizeof(number), page + 1U);
        stringAppend(destination, capacity, number);
        stringAppend(destination, capacity, (page == (Unsigned32)menu->page) ? "[" : " ");
        stringAppend(destination, capacity, pageNames[page]);
        stringAppend(destination, capacity, (page == (Unsigned32)menu->page) ? "]" : " ");
    }
    stringAppend(destination, capacity,
                 "   h/l j/k move  u/i page  enter choose  q close\n");

    list = &menu->lists[menu->page];
    if (list->count == 0U)
    {
        stringAppend(destination, capacity, "  (nothing here yet)\n");
        return;
    }

    first = windowStart(list);
    for (row = first; row < list->count && row < first + (Unsigned32)DEBUG_MENU_WINDOW; row++)
    {
        stringAppend(destination, capacity, (row == list->cursor) ? "> " : "  ");
        stringAppend(destination, capacity, list->rows[row]);
        if (row == list->inEffect)
        {
            stringAppend(destination, capacity, "   (in effect)");
        }
        stringAppend(destination, capacity, "\n");
    }

    stringAppend(destination, capacity, "  ");
    stringWriteUnsigned(number, sizeof(number), list->cursor + 1U);
    stringAppend(destination, capacity, number);
    stringAppend(destination, capacity, " of ");
    stringWriteUnsigned(number, sizeof(number), list->count);
    stringAppend(destination, capacity, number);
    if (list->beyondRoom > 0U)
    {
        stringAppend(destination, capacity, ", and ");
        stringWriteUnsigned(number, sizeof(number), list->beyondRoom);
        stringAppend(destination, capacity, number);
        stringAppend(destination, capacity, " more than there is room to list");
    }
    stringAppend(destination, capacity, "\n");
}

/* A whole page forwards or backwards, which is what the pager buttons under the
 * grid do.
 *
 * Landing on the first tile of the new page rather than keeping the position
 * within it. Keeping the offset reads better in theory and worse in practice:
 * the pager is used to sweep through eleven thousand animations looking for
 * one, and a highlight that stays in the middle of the grid while the tiles
 * change under it is a highlight nobody is looking at. */
Boolean debugMenuStepPage(DebugMenu *menu, Integer32 direction)
{
    DebugMenuList *list = &menu->lists[menu->page];
    Unsigned32 start = debugMenuGetPageStart(menu, menu->page);
    Unsigned32 wanted;

    if (list->count == 0U)
    {
        return BOOLEAN_FALSE;
    }
    if (direction < 0)
    {
        if (start == 0U)
        {
            return BOOLEAN_FALSE;
        }
        wanted = (start >= list->perPage) ? (start - list->perPage) : 0U;
    }
    else
    {
        wanted = start + list->perPage;
        if (wanted >= list->count)
        {
            return BOOLEAN_FALSE;
        }
    }
    return debugMenuSetCursor(menu, menu->page, wanted);
}
