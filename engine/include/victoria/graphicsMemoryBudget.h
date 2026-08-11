#ifndef VICTORIA_GRAPHICS_MEMORY_BUDGET_HEADER
#define VICTORIA_GRAPHICS_MEMORY_BUDGET_HEADER

#include "victoria/coreTypes.h"

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

Boolean graphicsMemoryBudgetRequest(GraphicsMemoryCategory category, MemorySize sizeInBytes);
void graphicsMemoryBudgetRelease(GraphicsMemoryCategory category, MemorySize sizeInBytes);

MemorySize graphicsMemoryBudgetGetUsedBytes(void);
MemorySize graphicsMemoryBudgetGetPeakBytes(void);
MemorySize graphicsMemoryBudgetGetCategoryBytes(GraphicsMemoryCategory category);
Unsigned32 graphicsMemoryBudgetGetRefusalCount(void);
const char *graphicsMemoryCategoryGetName(GraphicsMemoryCategory category);

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
