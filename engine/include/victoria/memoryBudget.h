#ifndef VICTORIA_MEMORY_BUDGET_HEADER
#define VICTORIA_MEMORY_BUDGET_HEADER

#include "victoria/coreTypes.h"
#include "victoria/memoryArena.h"

#define VICTORIA_MEMORY_BUDGET_BYTES (128UL * 1024UL * 1024UL)

MemoryArena *memoryBudgetGetGlobalArena(void);

#endif
