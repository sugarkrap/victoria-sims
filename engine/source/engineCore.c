#include "victoria/engineCore.h"
#include "victoria/freestandingRuntime.h"
#include "victoria/memoryBudget.h"
#include "victoria/platformInterface.h"
#include "victoria/renderInterface.h"

static MemoryArena *globalArena = NULL_POINTER;
static Boolean engineIsRunning = BOOLEAN_FALSE;

static void logMemoryBudget(void)
{
    char message[96];

    message[0] = '\0';
    stringAppend(message, sizeof(message), "memory budget: ");
    stringWriteUnsigned(message + stringLength(message),
                        sizeof(message) - stringLength(message),
                        globalArena->totalSizeInBytes / 1024UL / 1024UL);
    stringAppend(message, sizeof(message), " MiB reserved, ");
    stringWriteUnsigned(message + stringLength(message),
                        sizeof(message) - stringLength(message),
                        globalArena->usedSizeInBytes);
    stringAppend(message, sizeof(message), " bytes used");
    platformLogMessage(message);
}

Boolean engineInitialize(Unsigned32 widthInPixels, Unsigned32 heightInPixels)
{
    if (engineIsRunning == BOOLEAN_TRUE)
    {
        return BOOLEAN_TRUE;
    }

    globalArena = memoryBudgetGetGlobalArena();

    if (renderInitialize(globalArena, widthInPixels, heightInPixels) == BOOLEAN_FALSE)
    {
        platformLogMessage("engine: renderer failed to initialize");
        return BOOLEAN_FALSE;
    }

    engineIsRunning = BOOLEAN_TRUE;
    platformLogMessage("engine: initialized");
    logMemoryBudget();
    return BOOLEAN_TRUE;
}

void engineResize(Unsigned32 widthInPixels, Unsigned32 heightInPixels)
{
    if (engineIsRunning == BOOLEAN_FALSE)
    {
        return;
    }
    renderResize(widthInPixels, heightInPixels);
}

void engineRenderFrame(Real32 elapsedSeconds)
{
    if (engineIsRunning == BOOLEAN_FALSE)
    {
        return;
    }
    renderDrawFrame(elapsedSeconds);
}

void engineShutdown(void)
{
    if (engineIsRunning == BOOLEAN_FALSE)
    {
        return;
    }

    renderShutdown();
    engineIsRunning = BOOLEAN_FALSE;
    platformLogMessage("engine: shut down");
}

MemoryArena *engineGetGlobalArena(void)
{
    return globalArena;
}
