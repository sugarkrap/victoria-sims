#include "victoria/engineCore.h"
#include "utils/strings.h"
#include "victoria/graphicsMemoryBudget.h"
#include "victoria/memoryBudget.h"
#include "victoria/platformInterface.h"
#include "victoria/profiler.h"

#define WEB_EXPORT(exportName) __attribute__((export_name(exportName)))

__attribute__((import_module("victoriaPlatform"), import_name("logMessage")))
extern void hostLogMessage(const char *message, Unsigned32 messageLength);

__attribute__((import_module("victoriaPlatform"), import_name("getMilliseconds")))
extern double hostGetMilliseconds(void);

Unsigned32 victoriaWebInitialize(Unsigned32 widthInPixels, Unsigned32 heightInPixels,
                                 Unsigned32 graphicsMemoryLimitBytes);
Unsigned32 victoriaWebGetGraphicsMemoryLimitBytes(void);
Unsigned32 victoriaWebGetGraphicsMemoryUsedBytes(void);
void victoriaWebResize(Unsigned32 widthInPixels, Unsigned32 heightInPixels);
void victoriaWebRenderFrame(Real32 elapsedSeconds);
void victoriaWebShutdown(void);
Unsigned32 victoriaWebGetBudgetTotalBytes(void);
Unsigned32 victoriaWebGetBudgetUsedBytes(void);
Unsigned32 victoriaWebGetProfilerReportPointer(void);
Unsigned32 victoriaWebGetMenuTextPointer(void);
Unsigned32 victoriaWebGetMenuTextLength(void);
Unsigned32 victoriaWebHandleMenuKey(Unsigned32 character);
Unsigned32 victoriaWebHandlePointer(Unsigned32 action, Integer32 x, Integer32 y);
Unsigned32 victoriaWebGetProfilerReportLength(void);
Unsigned32 victoriaWebGetFrameMicroseconds(void);
Unsigned32 victoriaWebGetAverageFrameMicroseconds(void);
Unsigned32 victoriaWebGetWorstFrameMicroseconds(void);
Unsigned32 victoriaWebGetFrameIntervalMicroseconds(void);
Unsigned32 victoriaWebOpenDisc(double sizeInBytes);
Unsigned32 victoriaWebStepDiscLoad(void);
double victoriaWebGetWantedOffset(void);
Unsigned32 victoriaWebGetWantedLength(void);
Unsigned32 victoriaWebGetDeliveryPointer(void);
void victoriaWebDeliver(void);

Boolean webDiscStoreOpen(VirtualFileSystem *fileSystem, Unsigned64 sizeInBytes, MemoryArena *arena);
Real32 webDiscStoreGetWantedLength(void);
double webDiscStoreGetWantedOffset(void);
Unsigned32 webDiscStoreGetDeliveryPointer(void);
void webDiscStoreDeliver(void);

static VirtualFileSystem discFileSystem;

void platformLogMessage(const char *message)
{
    hostLogMessage(message, (Unsigned32)stringLength(message));
}

Unsigned64 platformGetMicroseconds(void)
{
    return (Unsigned64)(hostGetMilliseconds() * 1000.0);
}

Boolean platformCacheStore(const char *name, const Unsigned8 *bytes, MemorySize byteCount)
{
    (void)name;
    (void)bytes;
    (void)byteCount;
    return BOOLEAN_FALSE;
}

MemorySize platformCacheLoad(const char *name, Unsigned8 *destination, MemorySize capacity)
{
    (void)name;
    (void)destination;
    (void)capacity;
    return 0UL;
}

WEB_EXPORT("victoriaWebInitialize")
Unsigned32 victoriaWebInitialize(Unsigned32 widthInPixels, Unsigned32 heightInPixels,
                                 Unsigned32 graphicsMemoryLimitBytes)
{
    EngineConfiguration configuration;

    configuration.widthInPixels = widthInPixels;
    configuration.heightInPixels = heightInPixels;
    configuration.graphicsMemoryLimitBytes = (MemorySize)graphicsMemoryLimitBytes;
    configuration.cameraIsStill = BOOLEAN_FALSE;
    configuration.cameraAngleDegrees = 0.0f;
    configuration.poseIsHeld = BOOLEAN_FALSE;
    configuration.poseHeldTick = 0.0f;
    configuration.heldMorphChannel = 0U;
    configuration.wornName = NULL_POINTER;
    configuration.simArchetype = NULL_POINTER;
    configuration.menuIsOpen = BOOLEAN_FALSE;
    configuration.menuPage = 0U;

    return (Unsigned32)engineInitialize(&configuration);
}

WEB_EXPORT("victoriaWebGetGraphicsMemoryLimitBytes")
Unsigned32 victoriaWebGetGraphicsMemoryLimitBytes(void)
{
    return (Unsigned32)graphicsMemoryBudgetGetLimit();
}

WEB_EXPORT("victoriaWebGetGraphicsMemoryUsedBytes")
Unsigned32 victoriaWebGetGraphicsMemoryUsedBytes(void)
{
    return (Unsigned32)graphicsMemoryBudgetGetUsedBytes();
}

WEB_EXPORT("victoriaWebResize")
void victoriaWebResize(Unsigned32 widthInPixels, Unsigned32 heightInPixels)
{
    engineResize(widthInPixels, heightInPixels);
}

WEB_EXPORT("victoriaWebRenderFrame")
void victoriaWebRenderFrame(Real32 elapsedSeconds)
{
    engineBeginFrame();
    engineRenderFrame(elapsedSeconds);
    engineEndFrame();
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

WEB_EXPORT("victoriaWebGetProfilerReportPointer")
Unsigned32 victoriaWebGetProfilerReportPointer(void)
{
    return (Unsigned32)(MemorySize)engineGetProfilerReportText();
}

WEB_EXPORT("victoriaWebGetProfilerReportLength")
Unsigned32 victoriaWebGetProfilerReportLength(void)
{
    return (Unsigned32)stringLength(engineGetProfilerReportText());
}

WEB_EXPORT("victoriaWebGetMenuTextPointer")
Unsigned32 victoriaWebGetMenuTextPointer(void)
{
    return (Unsigned32)(MemorySize)engineGetMenuText();
}

WEB_EXPORT("victoriaWebGetMenuTextLength")
Unsigned32 victoriaWebGetMenuTextLength(void)
{
    return (Unsigned32)stringLength(engineGetMenuText());
}

WEB_EXPORT("victoriaWebHandleMenuKey")
Unsigned32 victoriaWebHandleMenuKey(Unsigned32 character)
{
    return (engineHandleMenuKey((char)(character & 0x7FU)) == BOOLEAN_TRUE) ? 1U : 0U;
}

WEB_EXPORT("victoriaWebHandlePointer")
Unsigned32 victoriaWebHandlePointer(Unsigned32 action, Integer32 x, Integer32 y)
{
    EnginePointerAction which = ENGINE_POINTER_MOVED;

    if (action == 1U)
    {
        which = ENGINE_POINTER_PRESSED;
    }
    else if (action == 2U)
    {
        which = ENGINE_POINTER_LEFT;
    }
    return (engineHandlePointer(which, x, y) == BOOLEAN_TRUE) ? 1U : 0U;
}

WEB_EXPORT("victoriaWebGetFrameMicroseconds")
Unsigned32 victoriaWebGetFrameMicroseconds(void)
{
    ProfilerFrameSummary frameSummary;
    profilerGetFrameSummary(&frameSummary);
    return (Unsigned32)frameSummary.lastMicroseconds;
}

WEB_EXPORT("victoriaWebGetAverageFrameMicroseconds")
Unsigned32 victoriaWebGetAverageFrameMicroseconds(void)
{
    ProfilerFrameSummary frameSummary;
    profilerGetFrameSummary(&frameSummary);
    return (Unsigned32)frameSummary.averageMicroseconds;
}

WEB_EXPORT("victoriaWebGetWorstFrameMicroseconds")
Unsigned32 victoriaWebGetWorstFrameMicroseconds(void)
{
    ProfilerFrameSummary frameSummary;
    profilerGetFrameSummary(&frameSummary);
    return (Unsigned32)frameSummary.worstMicroseconds;
}

WEB_EXPORT("victoriaWebGetFrameIntervalMicroseconds")
Unsigned32 victoriaWebGetFrameIntervalMicroseconds(void)
{
    ProfilerFrameSummary frameSummary;
    profilerGetFrameSummary(&frameSummary);
    return (Unsigned32)frameSummary.lastIntervalMicroseconds;
}

WEB_EXPORT("victoriaWebOpenDisc")
Unsigned32 victoriaWebOpenDisc(double sizeInBytes)
{
    if (sizeInBytes <= 0.0)
    {
        return 0U;
    }
    if (webDiscStoreOpen(&discFileSystem, (Unsigned64)sizeInBytes, engineGetGlobalArena()) ==
        BOOLEAN_FALSE)
    {
        return 0U;
    }
    engineBeginDiscLoad(&discFileSystem);
    return 1U;
}

WEB_EXPORT("victoriaWebStepDiscLoad")
Unsigned32 victoriaWebStepDiscLoad(void)
{
    return (Unsigned32)engineStepDiscLoad();
}

WEB_EXPORT("victoriaWebGetWantedOffset")
double victoriaWebGetWantedOffset(void)
{
    return webDiscStoreGetWantedOffset();
}

WEB_EXPORT("victoriaWebGetWantedLength")
Unsigned32 victoriaWebGetWantedLength(void)
{
    return (Unsigned32)webDiscStoreGetWantedLength();
}

WEB_EXPORT("victoriaWebGetDeliveryPointer")
Unsigned32 victoriaWebGetDeliveryPointer(void)
{
    return webDiscStoreGetDeliveryPointer();
}

WEB_EXPORT("victoriaWebDeliver")
void victoriaWebDeliver(void)
{
    webDiscStoreDeliver();
}
