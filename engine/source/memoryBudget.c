#include "victoria/memoryBudget.h"

/* Uninitialised, so it costs nothing in the shipped binary and nothing at
   startup beyond the operating system handing over zeroed pages. */
static Unsigned8 budgetStorage[VICTORIA_MEMORY_BUDGET_BYTES];
static MemoryArena globalArena;
static Boolean globalArenaIsInitialized = BOOLEAN_FALSE;

MemoryArena *memoryBudgetGetGlobalArena(void)
{
    if (globalArenaIsInitialized == BOOLEAN_FALSE)
    {
        memoryArenaInitialize(&globalArena, budgetStorage, VICTORIA_MEMORY_BUDGET_BYTES);
        globalArenaIsInitialized = BOOLEAN_TRUE;
    }
    return &globalArena;
}
