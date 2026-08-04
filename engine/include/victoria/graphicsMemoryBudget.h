#ifndef VICTORIA_GRAPHICS_MEMORY_BUDGET_HEADER
#define VICTORIA_GRAPHICS_MEMORY_BUDGET_HEADER

#include "victoria/coreTypes.h"

/* Unlike system memory, graphics memory is handed out by the driver, not by
   us: we cannot reserve it up front and we cannot see what the driver adds on
   top. So this is a ledger and a gate, not an allocator. Every resource the
   engine asks a backend to create is declared here first, and a request that
   would cross the ceiling is refused before the driver is ever called.

   The ceiling is dynamic because the hardware varies enormously — an ATI
   Imageon w100 has a few megabytes soldered next to the display controller,
   a desktop adapter has gigabytes. It is detected where the backend can tell
   us, overridden where the caller knows better, and falls back to a
   deliberately conservative default. */

#ifndef VICTORIA_GRAPHICS_MEMORY_DEFAULT_BYTES
#define VICTORIA_GRAPHICS_MEMORY_DEFAULT_BYTES (64UL * 1024UL * 1024UL)
#endif

typedef enum GraphicsMemoryCategory
{
    GRAPHICS_MEMORY_CATEGORY_TEXTURE = 0,
    GRAPHICS_MEMORY_CATEGORY_BUFFER,
    GRAPHICS_MEMORY_CATEGORY_RENDER_TARGET,
    GRAPHICS_MEMORY_CATEGORY_COUNT
} GraphicsMemoryCategory;

void graphicsMemoryBudgetInitialize(MemorySize limitInBytes);
void graphicsMemoryBudgetSetLimit(MemorySize limitInBytes);
MemorySize graphicsMemoryBudgetGetLimit(void);

/* Returns BOOLEAN_FALSE when the request would cross the ceiling, in which
   case nothing is recorded and the caller must not create the resource. */
Boolean graphicsMemoryBudgetRequest(GraphicsMemoryCategory category, MemorySize sizeInBytes);
void graphicsMemoryBudgetRelease(GraphicsMemoryCategory category, MemorySize sizeInBytes);

MemorySize graphicsMemoryBudgetGetUsedBytes(void);
MemorySize graphicsMemoryBudgetGetPeakBytes(void);
MemorySize graphicsMemoryBudgetGetCategoryBytes(GraphicsMemoryCategory category);
Unsigned32 graphicsMemoryBudgetGetRefusalCount(void);
const char *graphicsMemoryCategoryGetName(GraphicsMemoryCategory category);

/* Where the ceiling came from. Worth reporting: a number the backend told us
   and a number we guessed deserve very different amounts of trust. */
typedef enum GraphicsMemoryLimitSource
{
    GRAPHICS_MEMORY_LIMIT_SOURCE_DEFAULT = 0,
    GRAPHICS_MEMORY_LIMIT_SOURCE_DETECTED,
    GRAPHICS_MEMORY_LIMIT_SOURCE_OVERRIDE
} GraphicsMemoryLimitSource;

GraphicsMemoryLimitSource graphicsMemoryBudgetGetLimitSource(void);
void graphicsMemoryBudgetSetLimitSource(GraphicsMemoryLimitSource source);

MemorySize graphicsMemoryBudgetWriteReport(char *destination, MemorySize destinationCapacity);

#endif
