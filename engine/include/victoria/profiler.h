#ifndef VICTORIA_PROFILER_HEADER
#define VICTORIA_PROFILER_HEADER

#include "victoria/coreTypes.h"
#include "victoria/memoryArena.h"

/* Instrumented, hierarchical, fixed-capacity. Sampling profilers need stack
   unwinding and a signal or thread to sample from; WebAssembly gives us
   neither, so scopes are marked by hand and the same numbers come out of every
   target.

   Storage is taken from the global arena at startup, so profiling is inside
   the 128 MiB ceiling rather than beside it, and its cost is visible in the
   report it prints. */

#ifndef VICTORIA_PROFILER_ENABLED
#define VICTORIA_PROFILER_ENABLED 1
#endif

/* Raising these costs arena space and nothing else. All three are checked at
   run time and overflow is reported rather than silently dropped. */
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

/* Work and interval are deliberately separate. Work is what the engine spent
   inside the frame; interval is the wall clock between consecutive frames,
   which also covers whatever the platform does outside our reach — the browser
   waiting to present, or a vertical sync stall. Frames per second is only
   meaningful from the interval, and on the web the two differ by orders of
   magnitude. */
typedef struct ProfilerFrameSummary
{
    Unsigned64 frameIndex;
    Unsigned64 lastMicroseconds;
    Unsigned64 worstMicroseconds;
    Unsigned64 averageMicroseconds;
    Unsigned64 lastIntervalMicroseconds;
    Unsigned64 averageIntervalMicroseconds;
    /* Times ten, so it can be shown to one decimal place without floating
       point. */
    Unsigned32 framesPerSecondTimesTen;
    Unsigned32 zoneCount;
    Unsigned32 overflowCount;
} ProfilerFrameSummary;

Boolean profilerInitialize(MemoryArena *arena);

/* Registration is separated from entry so the string lookup happens once per
   call site rather than once per frame. Returns a negative identifier when the
   zone table is full; that identifier stays safe to pass to profilerBeginZone. */
Integer32 profilerRegisterZone(const char *zoneName);

void profilerBeginZone(Integer32 zoneIdentifier);
void profilerEndZone(void);

void profilerBeginFrame(void);
void profilerEndFrame(void);

void profilerGetFrameSummary(ProfilerFrameSummary *summaryOut);

/* Fills in up to summaryCapacity entries in tree order and returns how many
   were written. */
Unsigned32 profilerGetZoneSummaries(ProfilerZoneSummary *summariesOut, Unsigned32 summaryCapacity);

/* Renders the report as text, including the memory budget. Returns the length
   written, excluding the terminator. */
MemorySize profilerWriteReport(char *destination, MemorySize destinationCapacity);

/* Millisecond history of recent frames, oldest first, for a frame graph.
   Returns how many entries were written. */
Unsigned32 profilerGetFrameHistory(Unsigned64 *historyOut, Unsigned32 historyCapacity);

void profilerReset(void);

#if VICTORIA_PROFILER_ENABLED

/* The registration result is cached in a block-scoped static, so a call site
   pays the lookup once for the life of the process. */
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
