#include "victoria/engineCore.h"
#include "victoria/freestandingRuntime.h"
#include "victoria/graphicsMemoryBudget.h"
#include "victoria/memoryBudget.h"
#include "victoria/platformInterface.h"
#include "victoria/profiler.h"

#define WEB_EXPORT(exportName) __attribute__((export_name(exportName)))

__attribute__((import_module("victoriaPlatform"), import_name("logMessage")))
extern void hostLogMessage(const char *message, Unsigned32 messageLength);

/* performance.now() in milliseconds. WebAssembly converts f64 to i64 with a
   native instruction, so this needs no compiler runtime support. */
__attribute__((import_module("victoriaPlatform"), import_name("getMilliseconds")))
extern double hostGetMilliseconds(void);

/* Declared as well as defined so the build can keep -Wmissing-prototypes on:
   these are entry points for the host page, not internal linkage. */
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
Unsigned32 victoriaWebGetProfilerReportLength(void);
Unsigned32 victoriaWebGetFrameMicroseconds(void);
Unsigned32 victoriaWebGetAverageFrameMicroseconds(void);
Unsigned32 victoriaWebGetWorstFrameMicroseconds(void);
Unsigned32 victoriaWebGetFrameIntervalMicroseconds(void);

void platformLogMessage(const char *message)
{
    hostLogMessage(message, (Unsigned32)stringLength(message));
}

Unsigned64 platformGetMicroseconds(void)
{
    return (Unsigned64)(hostGetMilliseconds() * 1000.0);
}

WEB_EXPORT("victoriaWebInitialize")
Unsigned32 victoriaWebInitialize(Unsigned32 widthInPixels, Unsigned32 heightInPixels,
                                 Unsigned32 graphicsMemoryLimitBytes)
{
    EngineConfiguration configuration;

    configuration.widthInPixels = widthInPixels;
    configuration.heightInPixels = heightInPixels;
    configuration.graphicsMemoryLimitBytes = (MemorySize)graphicsMemoryLimitBytes;

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
    /* The browser owns presentation, so unlike the Linux loop there is no
       platform work to bracket: the frame is exactly the engine's work. */
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

/* The host reads the report straight out of linear memory rather than having
   it pushed across on every frame. */
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

/* Wall clock between frames, which is what a frame graph should plot: engine
   work alone says nothing about whether the frame arrived on time. */
WEB_EXPORT("victoriaWebGetFrameIntervalMicroseconds")
Unsigned32 victoriaWebGetFrameIntervalMicroseconds(void)
{
    ProfilerFrameSummary frameSummary;
    profilerGetFrameSummary(&frameSummary);
    return (Unsigned32)frameSummary.lastIntervalMicroseconds;
}
