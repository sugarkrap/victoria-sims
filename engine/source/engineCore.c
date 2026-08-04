#include "victoria/engineCore.h"
#include "victoria/freestandingRuntime.h"
#include "victoria/graphicsMemoryBudget.h"
#include "victoria/memoryBudget.h"
#include "victoria/platformInterface.h"
#include "victoria/profiler.h"
#include "victoria/renderInterface.h"

/* How often the text report is regenerated. Formatting is cheap but not free,
   and nothing reads it faster than a human can. */
#define ENGINE_REPORT_INTERVAL_MICROSECONDS 250000ULL

static MemoryArena *globalArena = NULL_POINTER;
static Boolean engineIsRunning = BOOLEAN_FALSE;
static char *profilerReportText = NULL_POINTER;
static Unsigned64 lastReportMicroseconds = 0ULL;

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

static void establishGraphicsMemoryLimit(MemorySize overrideBytes)
{
    MemorySize detectedBytes;

    if (overrideBytes > 0UL)
    {
        graphicsMemoryBudgetInitialize(overrideBytes);
        graphicsMemoryBudgetSetLimitSource(GRAPHICS_MEMORY_LIMIT_SOURCE_OVERRIDE);
        platformLogMessage("graphics memory: limit taken from override");
        return;
    }

    detectedBytes = renderQueryGraphicsMemoryBytes();
    graphicsMemoryBudgetInitialize(detectedBytes);
    graphicsMemoryBudgetSetLimitSource(detectedBytes > 0UL ? GRAPHICS_MEMORY_LIMIT_SOURCE_DETECTED
                                                           : GRAPHICS_MEMORY_LIMIT_SOURCE_DEFAULT);

    if (detectedBytes == 0UL)
    {
        /* Neither OpenGL ES 2.0 nor WebGPU can be asked portably, so this is
           the common path rather than an error. */
        platformLogMessage("graphics memory: backend did not report a size, assuming the default");
    }
}

Boolean engineInitialize(const EngineConfiguration *configuration)
{
    Boolean renderIsReady;

    if (engineIsRunning == BOOLEAN_TRUE)
    {
        return BOOLEAN_TRUE;
    }

    globalArena = memoryBudgetGetGlobalArena();

    if (profilerInitialize(globalArena) == BOOLEAN_FALSE)
    {
        platformLogMessage("engine: profiler unavailable, continuing without it");
    }

    profilerReportText = (char *)memoryArenaAllocate(globalArena, VICTORIA_PROFILER_REPORT_CAPACITY, 16UL);
    if (profilerReportText != NULL_POINTER)
    {
        profilerReportText[0] = '\0';
    }

    /* Before the renderer, so the very first resource it creates is already
       counted against the ceiling. */
    establishGraphicsMemoryLimit(configuration->graphicsMemoryLimitBytes);

    /* Bracketed as a frame of its own. Shader compilation and warm-up are the
       most expensive things the engine ever does, and zones entered outside a
       frame accumulate nothing — they would have reported as zero, hiding
       exactly the cost this is here to expose. Startup therefore shows up as
       frame one, and as the worst frame until something beats it. */
    profilerBeginFrame();
    renderIsReady = renderInitialize(globalArena, configuration->widthInPixels,
                                     configuration->heightInPixels);
    profilerEndFrame();

    if (renderIsReady == BOOLEAN_FALSE)
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

void engineBeginFrame(void)
{
    if (engineIsRunning == BOOLEAN_FALSE)
    {
        return;
    }
    profilerBeginFrame();
}

void engineRenderFrame(Real32 elapsedSeconds)
{
    if (engineIsRunning == BOOLEAN_FALSE)
    {
        return;
    }

    VICTORIA_PROFILE_ZONE_BEGIN("engineRenderFrame");
    renderDrawFrame(elapsedSeconds);
    VICTORIA_PROFILE_ZONE_END();
}

void engineEndFrame(void)
{
    Unsigned64 nowMicroseconds;

    if (engineIsRunning == BOOLEAN_FALSE)
    {
        return;
    }

    profilerEndFrame();

    if (profilerReportText == NULL_POINTER)
    {
        return;
    }

    /* The first frame always produces a report. Waiting for the interval would
       otherwise leave readers with nothing for a quarter second, and on a clock
       whose origin is near zero it would look like the profiler was dead. */
    nowMicroseconds = platformGetMicroseconds();
    if (profilerReportText[0] == '\0' ||
        nowMicroseconds - lastReportMicroseconds >= ENGINE_REPORT_INTERVAL_MICROSECONDS)
    {
        MemorySize reportLength =
            profilerWriteReport(profilerReportText, VICTORIA_PROFILER_REPORT_CAPACITY);

        /* Composed here rather than inside the profiler: system memory and
           graphics memory are tracked by different modules and neither should
           have to know about the other. */
        graphicsMemoryBudgetWriteReport(profilerReportText + reportLength,
                                        VICTORIA_PROFILER_REPORT_CAPACITY - reportLength);
        lastReportMicroseconds = nowMicroseconds;
    }
}

const char *engineGetProfilerReportText(void)
{
    if (profilerReportText == NULL_POINTER)
    {
        return "";
    }
    return profilerReportText;
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
