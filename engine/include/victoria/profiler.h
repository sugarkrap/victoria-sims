#ifndef VICTORIA_PROFILER_HEADER
#define VICTORIA_PROFILER_HEADER

#include "victoria/coreTypes.h"
#include "victoria/memoryArena.h"

#ifndef VICTORIA_PROFILER_ENABLED
#define VICTORIA_PROFILER_ENABLED 1
#endif

#define VICTORIA_PROFILER_MAXIMUM_ZONES 64
#define VICTORIA_PROFILER_MAXIMUM_DEPTH 32
#define VICTORIA_PROFILER_FRAME_HISTORY 120
#define VICTORIA_PROFILER_REPORT_CAPACITY 4096

typedef struct ProfilerZoneSummary
{
    const char *zoneName;
    Unsigned64 lastMicroseconds;
    Unsigned64 worstMicroseconds;
    Unsigned64 averageMicroseconds;
    Unsigned32 lastCallCount;
    Unsigned8 displayDepth;
} ProfilerZoneSummary;

typedef struct ProfilerFrameSummary
{
    Unsigned64 frameIndex;
    Unsigned64 lastMicroseconds;
    Unsigned64 worstMicroseconds;
    Unsigned64 averageMicroseconds;
    Unsigned64 lastIntervalMicroseconds;
    Unsigned64 averageIntervalMicroseconds;
    Unsigned32 framesPerSecondTimesTen;
    Unsigned32 zoneCount;
    Unsigned32 overflowCount;
} ProfilerFrameSummary;

Boolean profilerInitialize(MemoryArena *arena);

Integer32 profilerRegisterZone(const char *zoneName);

void profilerBeginZone(Integer32 zoneIdentifier);
void profilerEndZone(void);

void profilerBeginFrame(void);
void profilerEndFrame(void);

void profilerGetFrameSummary(ProfilerFrameSummary *summaryOut);

Unsigned32 profilerGetZoneSummaries(ProfilerZoneSummary *summariesOut, Unsigned32 summaryCapacity);

MemorySize profilerWriteReport(char *destination, MemorySize destinationCapacity);

Unsigned32 profilerGetFrameHistory(Unsigned64 *historyOut, Unsigned32 historyCapacity);

void profilerReset(void);

#if VICTORIA_PROFILER_ENABLED

#define VICTORIA_PROFILE_ZONE_BEGIN(zoneName)                       \
    do                                                              \
    {                                                               \
        static Integer32 cachedZoneIdentifier = -1;                 \
        if (cachedZoneIdentifier < 0)                               \
        {                                                           \
            cachedZoneIdentifier = profilerRegisterZone(zoneName);  \
        }                                                           \
        profilerBeginZone(cachedZoneIdentifier);                    \
    } while (0)

#define VICTORIA_PROFILE_ZONE_END() profilerEndZone()

#else

#define VICTORIA_PROFILE_ZONE_BEGIN(zoneName) ((void)0)
#define VICTORIA_PROFILE_ZONE_END() ((void)0)

#endif

#endif
