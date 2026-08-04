#ifndef VICTORIA_ENGINE_CORE_HEADER
#define VICTORIA_ENGINE_CORE_HEADER

#include "victoria/coreTypes.h"
#include "victoria/memoryArena.h"

typedef struct EngineConfiguration
{
    Unsigned32 widthInPixels;
    Unsigned32 heightInPixels;
    /* Zero asks the backend what it has and falls back to the conservative
       default; anything else overrides both. */
    MemorySize graphicsMemoryLimitBytes;
} EngineConfiguration;

Boolean engineInitialize(const EngineConfiguration *configuration);
void engineResize(Unsigned32 widthInPixels, Unsigned32 heightInPixels);
void engineShutdown(void);

/* The platform layer owns the frame boundary, so work it does outside the
   engine — pumping events, presenting — still lands inside the profiled frame.
   Every frame must be bracketed by exactly one begin and one end. */
void engineBeginFrame(void);
void engineRenderFrame(Real32 elapsedSeconds);
void engineEndFrame(void);

/* Latest profiler report, regenerated a few times a second. Never null;
   returns an empty string when profiling is unavailable. */
const char *engineGetProfilerReportText(void);

MemoryArena *engineGetGlobalArena(void);

#endif
