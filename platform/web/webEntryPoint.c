#include "victoria/engineCore.h"
#include "victoria/freestandingRuntime.h"
#include "victoria/memoryBudget.h"
#include "victoria/platformInterface.h"

#define WEB_EXPORT(exportName) __attribute__((export_name(exportName)))

__attribute__((import_module("victoriaPlatform"), import_name("logMessage")))
extern void hostLogMessage(const char *message, Unsigned32 messageLength);

/* Declared as well as defined so the build can keep -Wmissing-prototypes on:
   these are entry points for the host page, not internal linkage. */
Unsigned32 victoriaWebInitialize(Unsigned32 widthInPixels, Unsigned32 heightInPixels);
void victoriaWebResize(Unsigned32 widthInPixels, Unsigned32 heightInPixels);
void victoriaWebRenderFrame(Real32 elapsedSeconds);
void victoriaWebShutdown(void);
Unsigned32 victoriaWebGetBudgetTotalBytes(void);
Unsigned32 victoriaWebGetBudgetUsedBytes(void);

void platformLogMessage(const char *message)
{
    hostLogMessage(message, (Unsigned32)stringLength(message));
}

WEB_EXPORT("victoriaWebInitialize")
Unsigned32 victoriaWebInitialize(Unsigned32 widthInPixels, Unsigned32 heightInPixels)
{
    return (Unsigned32)engineInitialize(widthInPixels, heightInPixels);
}

WEB_EXPORT("victoriaWebResize")
void victoriaWebResize(Unsigned32 widthInPixels, Unsigned32 heightInPixels)
{
    engineResize(widthInPixels, heightInPixels);
}

WEB_EXPORT("victoriaWebRenderFrame")
void victoriaWebRenderFrame(Real32 elapsedSeconds)
{
    engineRenderFrame(elapsedSeconds);
}

WEB_EXPORT("victoriaWebShutdown")
void victoriaWebShutdown(void)
{
    engineShutdown();
}

WEB_EXPORT("victoriaWebGetBudgetTotalBytes")
Unsigned32 victoriaWebGetBudgetTotalBytes(void)
{
    return (Unsigned32)memoryBudgetGetGlobalArena()->totalSizeInBytes;
}

WEB_EXPORT("victoriaWebGetBudgetUsedBytes")
Unsigned32 victoriaWebGetBudgetUsedBytes(void)
{
    return (Unsigned32)memoryBudgetGetGlobalArena()->usedSizeInBytes;
}
