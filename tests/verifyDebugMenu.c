/* Checks the debug menu's navigation and the text it renders.
 *
 * The menu draws nothing and reads nothing, so everything in it is decidable
 * here — which is the reason it is a module rather than another hundred lines
 * of engineCore.c. What it holds is a cursor over lists that the engine fills
 * from a disc, and a cursor is exactly the kind of thing that is right for
 * every case somebody thought of and wrong at the ends.
 *
 * So the cases below are mostly ends: an empty page, a single row, the top, the
 * bottom, a page step longer than the list, and a list longer than the window. */

#include <stdio.h>

#include "utils/assert.h"
#include "utils/strings.h"
#include "victoria/debugMenu.h"

static Integer32 failureCount = 0;

#define BODY_CAPACITY 4U
#define LONG_CAPACITY 40U

static char bodyRows[BODY_CAPACITY][DEBUG_MENU_NAME_LIMIT];
static char longRows[LONG_CAPACITY][DEBUG_MENU_NAME_LIMIT];
static char text[4096];

static Boolean textHolds(const char *fragment)
{
    return stringContainsIgnoringCase(text, fragment);
}

int main(void)
{
    DebugMenu menu;

    debugMenuInitialize(&menu);

    /* Shut, and saying so. A debug feature nobody can discover is a debug
       feature nobody has, so the closed state is not silent. */
    checkThat(&failureCount, "starts closed", !debugMenuIsOpen(&menu));
    debugMenuWriteText(&menu, text, sizeof(text));
    checkThat(&failureCount, "and still says how to open it", textHolds("press m"));
    checkThat(&failureCount, "a keystroke that is not the toggle does nothing while shut",
              debugMenuHandleKey(&menu, 'j') == DEBUG_MENU_IGNORED && !debugMenuIsOpen(&menu));
    checkThat(&failureCount, "and choosing while shut does nothing at all",
              debugMenuHandleKey(&menu, '\r') == DEBUG_MENU_IGNORED);

    checkThat(&failureCount, "m opens it",
              debugMenuHandleKey(&menu, 'm') == DEBUG_MENU_MOVED && debugMenuIsOpen(&menu));

    /* An empty page. Choosing here must not answer CHOSE: the caller would act
       on row nought of nothing. */
    debugMenuWriteText(&menu, text, sizeof(text));
    checkThat(&failureCount, "an unfilled page says it is empty", textHolds("nothing here yet"));
    checkThat(&failureCount, "and cannot be chosen from",
              debugMenuHandleKey(&menu, '\r') == DEBUG_MENU_IGNORED);
    checkThat(&failureCount, "nor moved within",
              debugMenuHandleKey(&menu, 'j') == DEBUG_MENU_IGNORED &&
                  debugMenuGetCursor(&menu, DEBUG_MENU_PAGE_BODY) == 0U);

    debugMenuBindPage(&menu, DEBUG_MENU_PAGE_BODY, bodyRows, BODY_CAPACITY);
    checkThat(&failureCount, "a bound page starts empty too",
              debugMenuGetCount(&menu, DEBUG_MENU_PAGE_BODY) == 0U);

    checkThat(&failureCount, "rows are numbered as they are added",
              debugMenuAddRow(&menu, DEBUG_MENU_PAGE_BODY, "am") == 0U &&
                  debugMenuAddRow(&menu, DEBUG_MENU_PAGE_BODY, "af") == 1U);
    (void)debugMenuAddRow(&menu, DEBUG_MENU_PAGE_BODY, "cu");
    (void)debugMenuAddRow(&menu, DEBUG_MENU_PAGE_BODY, "tf");
    checkThat(&failureCount, "and read back by number",
              stringEquals(debugMenuGetRow(&menu, DEBUG_MENU_PAGE_BODY, 2), "cu"));
    checkThat(&failureCount, "reading past the end gives an empty name, not a stray one",
              stringEquals(debugMenuGetRow(&menu, DEBUG_MENU_PAGE_BODY, 9), ""));

    /* Beyond room is counted, not dropped in silence. A list that stops at its
       capacity and says nothing looks exactly like a disc holding no more. */
    checkThat(&failureCount, "a row past the capacity does not fit",
              debugMenuAddRow(&menu, DEBUG_MENU_PAGE_BODY, "em") ==
                  (Unsigned32)DEBUG_MENU_NONE);
    checkThat(&failureCount, "and the count stays at the capacity",
              debugMenuGetCount(&menu, DEBUG_MENU_PAGE_BODY) == BODY_CAPACITY);
    debugMenuWriteText(&menu, text, sizeof(text));
    checkThat(&failureCount, "and the text says so out loud",
              textHolds("1 more than there is room to list"));

    /* The cursor stops at the ends rather than wrapping: a list of eleven
       thousand that jumps top to bottom on one keystroke has lost its reader,
       and a wrap cannot be told from a redraw by looking. */
    checkThat(&failureCount, "up at the top does nothing",
              debugMenuHandleKey(&menu, 'k') == DEBUG_MENU_IGNORED &&
                  debugMenuGetCursor(&menu, DEBUG_MENU_PAGE_BODY) == 0U);
    checkThat(&failureCount, "down moves one",
              debugMenuHandleKey(&menu, 'j') == DEBUG_MENU_MOVED &&
                  debugMenuGetCursor(&menu, DEBUG_MENU_PAGE_BODY) == 1U);
    (void)debugMenuHandleKey(&menu, 'j');
    (void)debugMenuHandleKey(&menu, 'j');
    checkThat(&failureCount, "and stops at the last row",
              debugMenuGetCursor(&menu, DEBUG_MENU_PAGE_BODY) == BODY_CAPACITY - 1U &&
                  debugMenuHandleKey(&menu, 'j') == DEBUG_MENU_IGNORED);
    checkThat(&failureCount, "a page step longer than the list lands on the first row",
              debugMenuHandleKey(&menu, 'u') == DEBUG_MENU_MOVED &&
                  debugMenuGetCursor(&menu, DEBUG_MENU_PAGE_BODY) == 0U);
    checkThat(&failureCount, "and a page step down lands on the last",
              debugMenuHandleKey(&menu, 'i') == DEBUG_MENU_MOVED &&
                  debugMenuGetCursor(&menu, DEBUG_MENU_PAGE_BODY) == BODY_CAPACITY - 1U);

    checkThat(&failureCount, "a filled page can be chosen from",
              debugMenuHandleKey(&menu, '\r') == DEBUG_MENU_CHOSE);
    checkThat(&failureCount, "and space chooses too, since a terminal and a browser disagree "
                             "about what enter is",
              debugMenuHandleKey(&menu, ' ') == DEBUG_MENU_CHOSE);
    checkThat(&failureCount, "choosing does not move the cursor",
              debugMenuGetCursor(&menu, DEBUG_MENU_PAGE_BODY) == BODY_CAPACITY - 1U);

    /* Each page keeps its own cursor. Sharing one would move the clothing when
       the body was scrolled, which reads as the menu losing its place. */
    checkThat(&failureCount, "digits switch pages",
              debugMenuHandleKey(&menu, '3') == DEBUG_MENU_MOVED &&
                  debugMenuGetPage(&menu) == DEBUG_MENU_PAGE_ANIMATION);
    checkThat(&failureCount, "a digit past the last page is refused",
              debugMenuHandleKey(&menu, '7') == DEBUG_MENU_IGNORED &&
                  debugMenuGetPage(&menu) == DEBUG_MENU_PAGE_ANIMATION);
    checkThat(&failureCount, "and the page switched to has a cursor of its own",
              debugMenuGetCursor(&menu, DEBUG_MENU_PAGE_ANIMATION) == 0U);
    (void)debugMenuHandleKey(&menu, '1');
    checkThat(&failureCount, "so switching back finds the cursor where it was left",
              debugMenuGetCursor(&menu, DEBUG_MENU_PAGE_BODY) == BODY_CAPACITY - 1U);

    /* What is in effect is not where the cursor is. A menu that conflated them
       could not show you what you had before you chose something else. */
    debugMenuSetInEffect(&menu, DEBUG_MENU_PAGE_BODY, 0U);
    (void)debugMenuHandleKey(&menu, 'u');
    debugMenuWriteText(&menu, text, sizeof(text));
    checkThat(&failureCount, "the row in effect is marked", textHolds("(in effect)"));
    checkThat(&failureCount, "and the cursor is marked separately", textHolds("> am"));

    /* A window, so a long list stays readable. */
    {
        Unsigned32 row;

        debugMenuBindPage(&menu, DEBUG_MENU_PAGE_ANIMATION, longRows, LONG_CAPACITY);
        for (row = 0U; row < LONG_CAPACITY; row++)
        {
            char name[DEBUG_MENU_NAME_LIMIT];
            char number[16];

            name[0] = '\0';
            stringAppend(name, sizeof(name), "anim");
            stringWriteUnsigned(number, sizeof(number), row);
            stringAppend(name, sizeof(name), number);
            (void)debugMenuAddRow(&menu, DEBUG_MENU_PAGE_ANIMATION, name);
        }
        (void)debugMenuHandleKey(&menu, '3');
        debugMenuWriteText(&menu, text, sizeof(text));
        checkThat(&failureCount, "a long list shows the top when the cursor is there",
                  textHolds("anim0") && !textHolds("anim39"));

        for (row = 0U; row < LONG_CAPACITY; row++)
        {
            (void)debugMenuHandleKey(&menu, 'j');
        }
        debugMenuWriteText(&menu, text, sizeof(text));
        checkThat(&failureCount, "and the bottom when the cursor is there",
                  textHolds("anim39") && !textHolds("anim0\n"));
        checkThat(&failureCount, "with the cursor inside the window either way",
                  textHolds("> anim39"));
        checkThat(&failureCount, "and the position stated",
                  textHolds("40 of 40"));
    }

    /* Rebuilding a page — the clothing changes the moment the Sim does. */
    debugMenuClearPage(&menu, DEBUG_MENU_PAGE_BODY);
    checkThat(&failureCount, "clearing empties a page and forgets the cursor",
              debugMenuGetCount(&menu, DEBUG_MENU_PAGE_BODY) == 0U &&
                  debugMenuGetCursor(&menu, DEBUG_MENU_PAGE_BODY) == 0U);
    checkThat(&failureCount, "and it can be filled again afterwards",
              debugMenuAddRow(&menu, DEBUG_MENU_PAGE_BODY, "ef") == 0U);

    checkThat(&failureCount, "q closes it",
              debugMenuHandleKey(&menu, 'q') == DEBUG_MENU_MOVED && !debugMenuIsOpen(&menu));
    checkThat(&failureCount, "and m closes it as well as opening it",
              debugMenuHandleKey(&menu, 'm') == DEBUG_MENU_MOVED &&
                  debugMenuHandleKey(&menu, 'm') == DEBUG_MENU_MOVED &&
                  !debugMenuIsOpen(&menu));

    /* An unbound page must not be written to. The engine binds arena storage,
       and a menu that wrote through a null would take the process with it. */
    {
        DebugMenu fresh;

        debugMenuInitialize(&fresh);
        checkThat(&failureCount, "adding to an unbound page is refused rather than fatal",
                  debugMenuAddRow(&fresh, DEBUG_MENU_PAGE_CLOTHING, "x") ==
                      (Unsigned32)DEBUG_MENU_NONE);
        fresh.isOpen = BOOLEAN_TRUE;
        debugMenuWriteText(&fresh, text, sizeof(text));
        checkThat(&failureCount, "and it renders as empty", textHolds("nothing here yet"));
    }

    return checkSummarize(failureCount, "debug menu");
}
