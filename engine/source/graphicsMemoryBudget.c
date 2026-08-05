#include "victoria/graphicsMemoryBudget.h"

#include "utils/strings.h"
#include "victoria/freestandingRuntime.h"
#include "victoria/platformInterface.h"

typedef struct GraphicsMemoryBudgetState
{
    MemorySize limitInBytes;
    MemorySize usedInBytes;
    MemorySize peakInBytes;
    MemorySize categoryBytes[GRAPHICS_MEMORY_CATEGORY_COUNT];
    Unsigned32 refusalCount;
    GraphicsMemoryLimitSource limitSource;
} GraphicsMemoryBudgetState;

static GraphicsMemoryBudgetState budgetState;

static const char *categoryNames[GRAPHICS_MEMORY_CATEGORY_COUNT] = {
    "texture",
    "buffer",
    "renderTarget"
};

void graphicsMemoryBudgetInitialize(MemorySize limitInBytes)
{
    memoryFill(&budgetState, 0, sizeof(budgetState));
    budgetState.limitInBytes =
        limitInBytes > 0UL ? limitInBytes : (MemorySize)VICTORIA_GRAPHICS_MEMORY_DEFAULT_BYTES;
}

void graphicsMemoryBudgetSetLimit(MemorySize limitInBytes)
{
    if (limitInBytes == 0UL)
    {
        return;
    }
    budgetState.limitInBytes = limitInBytes;
}

MemorySize graphicsMemoryBudgetGetLimit(void)
{
    return budgetState.limitInBytes;
}

Boolean graphicsMemoryBudgetRequest(GraphicsMemoryCategory category, MemorySize sizeInBytes)
{
    MemorySize updatedUsage;

    if (category >= GRAPHICS_MEMORY_CATEGORY_COUNT)
    {
        return BOOLEAN_FALSE;
    }

    updatedUsage = budgetState.usedInBytes + sizeInBytes;

    /* Ordered so an overflowing sum cannot slip past the ceiling test. */
    if (updatedUsage < budgetState.usedInBytes || updatedUsage > budgetState.limitInBytes)
    {
        budgetState.refusalCount += 1U;
        platformLogMessage("graphics memory: request refused, ceiling reached");
        return BOOLEAN_FALSE;
    }

    budgetState.usedInBytes = updatedUsage;
    budgetState.categoryBytes[category] += sizeInBytes;

    if (updatedUsage > budgetState.peakInBytes)
    {
        budgetState.peakInBytes = updatedUsage;
    }

    return BOOLEAN_TRUE;
}

void graphicsMemoryBudgetRelease(GraphicsMemoryCategory category, MemorySize sizeInBytes)
{
    if (category >= GRAPHICS_MEMORY_CATEGORY_COUNT)
    {
        return;
    }

    /* Clamped rather than trusted: a double release would otherwise wrap the
       counter and make the ledger read as almost entirely free. */
    if (sizeInBytes > budgetState.categoryBytes[category])
    {
        sizeInBytes = budgetState.categoryBytes[category];
    }
    if (sizeInBytes > budgetState.usedInBytes)
    {
        sizeInBytes = budgetState.usedInBytes;
    }

    budgetState.categoryBytes[category] -= sizeInBytes;
    budgetState.usedInBytes -= sizeInBytes;
}

MemorySize graphicsMemoryBudgetGetUsedBytes(void)
{
    return budgetState.usedInBytes;
}

MemorySize graphicsMemoryBudgetGetPeakBytes(void)
{
    return budgetState.peakInBytes;
}

MemorySize graphicsMemoryBudgetGetCategoryBytes(GraphicsMemoryCategory category)
{
    if (category >= GRAPHICS_MEMORY_CATEGORY_COUNT)
    {
        return 0UL;
    }
    return budgetState.categoryBytes[category];
}

Unsigned32 graphicsMemoryBudgetGetRefusalCount(void)
{
    return budgetState.refusalCount;
}

const char *graphicsMemoryCategoryGetName(GraphicsMemoryCategory category)
{
    if (category >= GRAPHICS_MEMORY_CATEGORY_COUNT)
    {
        return "unknown";
    }
    return categoryNames[category];
}

GraphicsMemoryLimitSource graphicsMemoryBudgetGetLimitSource(void)
{
    return budgetState.limitSource;
}

void graphicsMemoryBudgetSetLimitSource(GraphicsMemoryLimitSource source)
{
    budgetState.limitSource = source;
}

static const char *limitSourceGetName(GraphicsMemoryLimitSource source)
{
    switch (source)
    {
    case GRAPHICS_MEMORY_LIMIT_SOURCE_DETECTED:
        return "detected";
    case GRAPHICS_MEMORY_LIMIT_SOURCE_OVERRIDE:
        return "override";
    case GRAPHICS_MEMORY_LIMIT_SOURCE_DEFAULT:
    default:
        return "assumed";
    }
}

MemorySize graphicsMemoryBudgetWriteReport(char *destination, MemorySize destinationCapacity)
{
    char digits[24];
    MemorySize length;
    Unsigned32 categoryIndex;

    length = stringAppend(destination, destinationCapacity, "graphics memory ");
    stringWriteUnsigned(digits, sizeof(digits), budgetState.limitInBytes / 1024ULL);
    stringAppend(destination, destinationCapacity, digits);
    stringAppend(destination, destinationCapacity, " KiB limit (");
    stringAppend(destination, destinationCapacity, limitSourceGetName(budgetState.limitSource));
    stringAppend(destination, destinationCapacity, "), ");

    stringWriteUnsigned(digits, sizeof(digits), budgetState.usedInBytes);
    stringAppend(destination, destinationCapacity, digits);
    stringAppend(destination, destinationCapacity, " bytes used, ");
    stringWriteUnsigned(digits, sizeof(digits), budgetState.peakInBytes);
    stringAppend(destination, destinationCapacity, digits);
    length = stringAppend(destination, destinationCapacity, " bytes peak\n");

    length = stringAppend(destination, destinationCapacity, " ");
    for (categoryIndex = 0U; categoryIndex < (Unsigned32)GRAPHICS_MEMORY_CATEGORY_COUNT;
         categoryIndex += 1U)
    {
        stringAppend(destination, destinationCapacity, " ");
        stringAppend(destination, destinationCapacity,
                     graphicsMemoryCategoryGetName((GraphicsMemoryCategory)categoryIndex));
        stringAppend(destination, destinationCapacity, " ");
        stringWriteUnsigned(digits, sizeof(digits), budgetState.categoryBytes[categoryIndex]);
        length = stringAppend(destination, destinationCapacity, digits);
    }
    length = stringAppend(destination, destinationCapacity, "\n");

    if (budgetState.refusalCount > 0U)
    {
        stringAppend(destination, destinationCapacity, "warning: ");
        stringWriteUnsigned(digits, sizeof(digits), budgetState.refusalCount);
        stringAppend(destination, destinationCapacity, digits);
        length = stringAppend(destination, destinationCapacity,
                              " graphics allocation(s) refused at the ceiling\n");
    }

    return length;
}
