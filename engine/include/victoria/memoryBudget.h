#ifndef VICTORIA_MEMORY_BUDGET_HEADER
#define VICTORIA_MEMORY_BUDGET_HEADER

#include "victoria/coreTypes.h"
#include "victoria/memoryArena.h"

/* The ceiling. 128 MiB of statically reserved storage, identical on every
   target, allocated by the linker rather than at run time. Raising this number
   is a project-level decision, not an implementation detail. */
#define VICTORIA_MEMORY_BUDGET_BYTES (128UL * 1024UL * 1024UL)

MemoryArena *memoryBudgetGetGlobalArena(void);

#endif
