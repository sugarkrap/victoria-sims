#include "victoria/memoryBudget.h"

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
