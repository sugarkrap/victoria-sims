#include "victoria/profiler.h"

#include "victoria/freestandingRuntime.h"
#include "victoria/memoryBudget.h"
#include "victoria/platformInterface.h"

#if VICTORIA_PROFILER_ENABLED

typedef struct ProfilerZone
{
    const char *zoneName;
    Unsigned64 currentFrameMicroseconds;
    Unsigned64 lastMicroseconds;
    Unsigned64 worstMicroseconds;
    Unsigned64 accumulatedMicroseconds;
    Unsigned64 measuredFrameCount;
    Unsigned32 currentFrameCallCount;
    Unsigned32 lastCallCount;
    Unsigned8 displayDepth;
    Unsigned8 displayDepthIsKnown;
} ProfilerZone;

typedef struct ProfilerStackEntry
{
    Integer32 zoneIdentifier;
    Unsigned64 startMicroseconds;
} ProfilerStackEntry;

typedef struct ProfilerState
{
    ProfilerZone *zones;
    ProfilerStackEntry *stack;
    Unsigned64 *frameHistory;

    Unsigned32 zoneCount;
    Unsigned32 stackDepth;
    Unsigned32 historyCount;
    Unsigned32 historyWriteIndex;
    Unsigned32 overflowCount;

    Unsigned64 frameIndex;
    Unsigned64 frameStartMicroseconds;
    Unsigned64 lastFrameMicroseconds;
    Unsigned64 worstFrameMicroseconds;
    Unsigned64 accumulatedFrameMicroseconds;

    Unsigned64 previousFrameStartMicroseconds;
    Unsigned64 lastIntervalMicroseconds;
    Unsigned64 accumulatedIntervalMicroseconds;
    Unsigned64 intervalSampleCount;
    /* An explicit flag rather than testing the timestamp against zero: a clock
       whose origin really is zero, which is what performance.now() gives on a
       fresh page, would otherwise keep looking like "no previous frame". */
    Boolean previousFrameStartIsValid;

    Boolean isInitialized;
    Boolean frameIsOpen;
} ProfilerState;

static ProfilerState profilerState;

Boolean profilerInitialize(MemoryArena *arena)
{
    if (profilerState.isInitialized == BOOLEAN_TRUE)
    {
        return BOOLEAN_TRUE;
    }

    profilerState.zones = (ProfilerZone *)memoryArenaAllocate(
        arena, sizeof(ProfilerZone) * VICTORIA_PROFILER_MAXIMUM_ZONES, 16UL);
    profilerState.stack = (ProfilerStackEntry *)memoryArenaAllocate(
        arena, sizeof(ProfilerStackEntry) * VICTORIA_PROFILER_MAXIMUM_DEPTH, 16UL);
    profilerState.frameHistory = (Unsigned64 *)memoryArenaAllocate(
        arena, sizeof(Unsigned64) * VICTORIA_PROFILER_FRAME_HISTORY, 16UL);

    if (profilerState.zones == NULL_POINTER ||
        profilerState.stack == NULL_POINTER ||
        profilerState.frameHistory == NULL_POINTER)
    {
        platformLogMessage("profiler: not enough arena space, profiling disabled");
        return BOOLEAN_FALSE;
    }

    memoryFill(profilerState.zones, 0, sizeof(ProfilerZone) * VICTORIA_PROFILER_MAXIMUM_ZONES);
    memoryFill(profilerState.stack, 0, sizeof(ProfilerStackEntry) * VICTORIA_PROFILER_MAXIMUM_DEPTH);
    memoryFill(profilerState.frameHistory, 0, sizeof(Unsigned64) * VICTORIA_PROFILER_FRAME_HISTORY);

    profilerState.isInitialized = BOOLEAN_TRUE;
    return BOOLEAN_TRUE;
}

Integer32 profilerRegisterZone(const char *zoneName)
{
    Unsigned32 index;

    if (profilerState.isInitialized == BOOLEAN_FALSE)
    {
        return -1;
    }

    /* String literals are usually deduplicated within a translation unit but
       not across them, so a pointer match is only a fast path. */
    for (index = 0U; index < profilerState.zoneCount; index += 1U)
    {
        if (profilerState.zones[index].zoneName == zoneName ||
            stringEquals(profilerState.zones[index].zoneName, zoneName) == BOOLEAN_TRUE)
        {
            return (Integer32)index;
        }
    }

    if (profilerState.zoneCount >= (Unsigned32)VICTORIA_PROFILER_MAXIMUM_ZONES)
    {
        profilerState.overflowCount += 1U;
        return -1;
    }

    index = profilerState.zoneCount;
    profilerState.zones[index].zoneName = zoneName;
    profilerState.zoneCount += 1U;
    return (Integer32)index;
}

void profilerBeginZone(Integer32 zoneIdentifier)
{
    ProfilerStackEntry *entry;

    if (profilerState.isInitialized == BOOLEAN_FALSE || zoneIdentifier < 0)
    {
        return;
    }

    if (profilerState.stackDepth >= (Unsigned32)VICTORIA_PROFILER_MAXIMUM_DEPTH)
    {
        profilerState.overflowCount += 1U;
        return;
    }

    /* Depth is fixed the first time a zone is entered. A zone reached at two
       different depths is reported at the first one rather than jumping around
       between frames. */
    if (profilerState.zones[zoneIdentifier].displayDepthIsKnown == BOOLEAN_FALSE)
    {
        profilerState.zones[zoneIdentifier].displayDepth = (Unsigned8)profilerState.stackDepth;
        profilerState.zones[zoneIdentifier].displayDepthIsKnown = BOOLEAN_TRUE;
    }

    entry = &profilerState.stack[profilerState.stackDepth];
    entry->zoneIdentifier = zoneIdentifier;
    entry->startMicroseconds = platformGetMicroseconds();
    profilerState.stackDepth += 1U;
}

void profilerEndZone(void)
{
    const ProfilerStackEntry *entry;
    ProfilerZone *zone;
    Unsigned64 endMicroseconds;

    if (profilerState.isInitialized == BOOLEAN_FALSE || profilerState.stackDepth == 0U)
    {
        return;
    }

    endMicroseconds = platformGetMicroseconds();
    profilerState.stackDepth -= 1U;
    entry = &profilerState.stack[profilerState.stackDepth];
    zone = &profilerState.zones[entry->zoneIdentifier];

    zone->currentFrameMicroseconds += endMicroseconds - entry->startMicroseconds;
    zone->currentFrameCallCount += 1U;
}

void profilerBeginFrame(void)
{
    Unsigned32 index;

    if (profilerState.isInitialized == BOOLEAN_FALSE)
    {
        return;
    }

    for (index = 0U; index < profilerState.zoneCount; index += 1U)
    {
        profilerState.zones[index].currentFrameMicroseconds = 0ULL;
        profilerState.zones[index].currentFrameCallCount = 0U;
    }

    profilerState.stackDepth = 0U;
    profilerState.frameStartMicroseconds = platformGetMicroseconds();
    profilerState.frameIsOpen = BOOLEAN_TRUE;

    /* The first frame has no predecessor, so there is no interval to record. */
    if (profilerState.previousFrameStartIsValid == BOOLEAN_TRUE)
    {
        profilerState.lastIntervalMicroseconds =
            profilerState.frameStartMicroseconds - profilerState.previousFrameStartMicroseconds;
        profilerState.accumulatedIntervalMicroseconds += profilerState.lastIntervalMicroseconds;
        profilerState.intervalSampleCount += 1ULL;
    }
    profilerState.previousFrameStartMicroseconds = profilerState.frameStartMicroseconds;
    profilerState.previousFrameStartIsValid = BOOLEAN_TRUE;
}

void profilerEndFrame(void)
{
    Unsigned64 frameMicroseconds;
    Unsigned32 index;

    if (profilerState.isInitialized == BOOLEAN_FALSE || profilerState.frameIsOpen == BOOLEAN_FALSE)
    {
        return;
    }

    /* An unbalanced begin would otherwise leak into the next frame's stack. */
    if (profilerState.stackDepth != 0U)
    {
        profilerState.overflowCount += 1U;
        profilerState.stackDepth = 0U;
    }

    frameMicroseconds = platformGetMicroseconds() - profilerState.frameStartMicroseconds;
    profilerState.lastFrameMicroseconds = frameMicroseconds;
    profilerState.accumulatedFrameMicroseconds += frameMicroseconds;
    profilerState.frameIndex += 1ULL;

    if (frameMicroseconds > profilerState.worstFrameMicroseconds)
    {
        profilerState.worstFrameMicroseconds = frameMicroseconds;
    }

    profilerState.frameHistory[profilerState.historyWriteIndex] = frameMicroseconds;
    profilerState.historyWriteIndex =
        (profilerState.historyWriteIndex + 1U) % (Unsigned32)VICTORIA_PROFILER_FRAME_HISTORY;
    if (profilerState.historyCount < (Unsigned32)VICTORIA_PROFILER_FRAME_HISTORY)
    {
        profilerState.historyCount += 1U;
    }

    for (index = 0U; index < profilerState.zoneCount; index += 1U)
    {
        ProfilerZone *zone = &profilerState.zones[index];

        zone->lastMicroseconds = zone->currentFrameMicroseconds;
        zone->lastCallCount = zone->currentFrameCallCount;

        if (zone->currentFrameCallCount > 0U)
        {
            zone->accumulatedMicroseconds += zone->currentFrameMicroseconds;
            zone->measuredFrameCount += 1ULL;

            if (zone->currentFrameMicroseconds > zone->worstMicroseconds)
            {
                zone->worstMicroseconds = zone->currentFrameMicroseconds;
            }
        }
    }

    profilerState.frameIsOpen = BOOLEAN_FALSE;
}

void profilerGetFrameSummary(ProfilerFrameSummary *summaryOut)
{
    Unsigned64 averageIntervalMicroseconds = 0ULL;

    memoryFill(summaryOut, 0, sizeof(*summaryOut));

    if (profilerState.isInitialized == BOOLEAN_FALSE || profilerState.frameIndex == 0ULL)
    {
        return;
    }

    if (profilerState.intervalSampleCount > 0ULL)
    {
        averageIntervalMicroseconds =
            profilerState.accumulatedIntervalMicroseconds / profilerState.intervalSampleCount;
    }

    summaryOut->frameIndex = profilerState.frameIndex;
    summaryOut->lastMicroseconds = profilerState.lastFrameMicroseconds;
    summaryOut->worstMicroseconds = profilerState.worstFrameMicroseconds;
    summaryOut->averageMicroseconds =
        profilerState.accumulatedFrameMicroseconds / profilerState.frameIndex;
    summaryOut->lastIntervalMicroseconds = profilerState.lastIntervalMicroseconds;
    summaryOut->averageIntervalMicroseconds = averageIntervalMicroseconds;
    summaryOut->zoneCount = profilerState.zoneCount;
    summaryOut->overflowCount = profilerState.overflowCount;

    /* From the interval, never from the work: the two are the same only when
       the platform does nothing outside the engine. */
    if (averageIntervalMicroseconds > 0ULL)
    {
        summaryOut->framesPerSecondTimesTen = (Unsigned32)(10000000ULL / averageIntervalMicroseconds);
    }
}

Unsigned32 profilerGetZoneSummaries(ProfilerZoneSummary *summariesOut, Unsigned32 summaryCapacity)
{
    Unsigned32 writtenCount = 0U;
    Unsigned32 index;

    if (profilerState.isInitialized == BOOLEAN_FALSE)
    {
        return 0U;
    }

    for (index = 0U; index < profilerState.zoneCount && writtenCount < summaryCapacity; index += 1U)
    {
        const ProfilerZone *zone = &profilerState.zones[index];
        ProfilerZoneSummary *summary = &summariesOut[writtenCount];

        summary->zoneName = zone->zoneName;
        summary->lastMicroseconds = zone->lastMicroseconds;
        summary->worstMicroseconds = zone->worstMicroseconds;
        summary->averageMicroseconds =
            zone->measuredFrameCount > 0ULL ? zone->accumulatedMicroseconds / zone->measuredFrameCount : 0ULL;
        summary->lastCallCount = zone->lastCallCount;
        summary->displayDepth = zone->displayDepth;
        writtenCount += 1U;
    }

    return writtenCount;
}

Unsigned32 profilerGetFrameHistory(Unsigned64 *historyOut, Unsigned32 historyCapacity)
{
    Unsigned32 writtenCount = 0U;
    Unsigned32 index;

    if (profilerState.isInitialized == BOOLEAN_FALSE)
    {
        return 0U;
    }

    /* Walk the ring from the oldest entry so callers get chronological order. */
    for (index = 0U; index < profilerState.historyCount && writtenCount < historyCapacity; index += 1U)
    {
        Unsigned32 readIndex =
            (profilerState.historyWriteIndex + (Unsigned32)VICTORIA_PROFILER_FRAME_HISTORY -
             profilerState.historyCount + index) %
            (Unsigned32)VICTORIA_PROFILER_FRAME_HISTORY;

        historyOut[writtenCount] = profilerState.frameHistory[readIndex];
        writtenCount += 1U;
    }

    return writtenCount;
}

void profilerReset(void)
{
    if (profilerState.isInitialized == BOOLEAN_FALSE)
    {
        return;
    }

    memoryFill(profilerState.zones, 0, sizeof(ProfilerZone) * VICTORIA_PROFILER_MAXIMUM_ZONES);
    memoryFill(profilerState.frameHistory, 0, sizeof(Unsigned64) * VICTORIA_PROFILER_FRAME_HISTORY);

    profilerState.zoneCount = 0U;
    profilerState.stackDepth = 0U;
    profilerState.historyCount = 0U;
    profilerState.historyWriteIndex = 0U;
    profilerState.overflowCount = 0U;
    profilerState.frameIndex = 0ULL;
    profilerState.lastFrameMicroseconds = 0ULL;
    profilerState.worstFrameMicroseconds = 0ULL;
    profilerState.accumulatedFrameMicroseconds = 0ULL;
    profilerState.previousFrameStartMicroseconds = 0ULL;
    profilerState.previousFrameStartIsValid = BOOLEAN_FALSE;
    profilerState.lastIntervalMicroseconds = 0ULL;
    profilerState.accumulatedIntervalMicroseconds = 0ULL;
    profilerState.intervalSampleCount = 0ULL;
    profilerState.frameIsOpen = BOOLEAN_FALSE;
}

/* Report formatting. Every helper appends to a terminated buffer and returns
   the new length, so a full buffer truncates cleanly instead of overrunning. */

/* Column positions are where each field ENDS, so numbers line up on their last
   digit. The header is laid out with the same helper as the rows, which is the
   only way the two stay in step. */
#define REPORT_COLUMN_CALLS 34UL
#define REPORT_COLUMN_LAST 47UL
#define REPORT_COLUMN_AVERAGE 60UL
#define REPORT_COLUMN_WORST 73UL

static void writeMilliseconds(char *destination, MemorySize capacity, Unsigned64 microseconds)
{
    char digits[24];
    Unsigned64 wholeMilliseconds = microseconds / 1000ULL;
    Unsigned64 fractionalPart = microseconds % 1000ULL;

    destination[0] = '\0';

    stringWriteUnsigned(digits, sizeof(digits), wholeMilliseconds);
    stringAppend(destination, capacity, digits);
    stringAppend(destination, capacity, ".");

    if (fractionalPart < 100ULL)
    {
        stringAppend(destination, capacity, "0");
    }
    if (fractionalPart < 10ULL)
    {
        stringAppend(destination, capacity, "0");
    }

    stringWriteUnsigned(digits, sizeof(digits), fractionalPart);
    stringAppend(destination, capacity, digits);
}

static MemorySize appendPadding(char *destination, MemorySize capacity, MemorySize currentLength,
                                MemorySize targetColumn)
{
    while (currentLength < targetColumn)
    {
        MemorySize updatedLength = stringAppend(destination, capacity, " ");
        if (updatedLength == currentLength)
        {
            break; /* Buffer is full. */
        }
        currentLength = updatedLength;
    }
    return currentLength;
}

/* Right-aligns text so it ends at targetEndColumn, measured from lineStart. */
static MemorySize appendColumn(char *destination, MemorySize capacity, MemorySize currentLength,
                               MemorySize lineStart, MemorySize targetEndColumn, const char *text)
{
    MemorySize textLength = stringLength(text);
    MemorySize absoluteEnd = lineStart + targetEndColumn;

    if (absoluteEnd > textLength)
    {
        currentLength = appendPadding(destination, capacity, currentLength, absoluteEnd - textLength);
    }
    return stringAppend(destination, capacity, text);
}

MemorySize profilerWriteReport(char *destination, MemorySize destinationCapacity)
{
    ProfilerZoneSummary summaries[VICTORIA_PROFILER_MAXIMUM_ZONES];
    ProfilerFrameSummary frameSummary;
    const MemoryArena *arena = memoryBudgetGetGlobalArena();
    char digits[24];
    MemorySize length;
    MemorySize lineStart;
    Unsigned32 summaryCount;
    Unsigned32 index;
    Unsigned8 depthIndex;

    if (destinationCapacity == 0UL)
    {
        return 0UL;
    }

    destination[0] = '\0';

    if (profilerState.isInitialized == BOOLEAN_FALSE)
    {
        return stringAppend(destination, destinationCapacity, "profiler: not initialized\n");
    }

    profilerGetFrameSummary(&frameSummary);
    summaryCount = profilerGetZoneSummaries(summaries, (Unsigned32)VICTORIA_ARRAY_LENGTH(summaries));

    stringAppend(destination, destinationCapacity, "frame ");
    stringWriteUnsigned(digits, sizeof(digits), frameSummary.frameIndex);
    stringAppend(destination, destinationCapacity, digits);

    stringAppend(destination, destinationCapacity, "   work ");
    writeMilliseconds(digits, sizeof(digits), frameSummary.lastMicroseconds);
    stringAppend(destination, destinationCapacity, digits);
    stringAppend(destination, destinationCapacity, " ms   average ");
    writeMilliseconds(digits, sizeof(digits), frameSummary.averageMicroseconds);
    stringAppend(destination, destinationCapacity, digits);
    stringAppend(destination, destinationCapacity, " ms   worst ");
    writeMilliseconds(digits, sizeof(digits), frameSummary.worstMicroseconds);
    stringAppend(destination, destinationCapacity, digits);
    stringAppend(destination, destinationCapacity, " ms\n");

    stringAppend(destination, destinationCapacity, "interval ");
    writeMilliseconds(digits, sizeof(digits), frameSummary.lastIntervalMicroseconds);
    stringAppend(destination, destinationCapacity, digits);
    stringAppend(destination, destinationCapacity, " ms   average ");
    writeMilliseconds(digits, sizeof(digits), frameSummary.averageIntervalMicroseconds);
    stringAppend(destination, destinationCapacity, digits);
    stringAppend(destination, destinationCapacity, " ms   ");

    stringWriteUnsigned(digits, sizeof(digits), frameSummary.framesPerSecondTimesTen / 10U);
    stringAppend(destination, destinationCapacity, digits);
    stringAppend(destination, destinationCapacity, ".");
    stringWriteUnsigned(digits, sizeof(digits), frameSummary.framesPerSecondTimesTen % 10U);
    stringAppend(destination, destinationCapacity, digits);
    stringAppend(destination, destinationCapacity, " fps\n");

    stringAppend(destination, destinationCapacity, "memory ");
    stringWriteUnsigned(digits, sizeof(digits), arena->totalSizeInBytes / 1024ULL / 1024ULL);
    stringAppend(destination, destinationCapacity, digits);
    stringAppend(destination, destinationCapacity, " MiB budget, ");
    stringWriteUnsigned(digits, sizeof(digits), arena->usedSizeInBytes);
    stringAppend(destination, destinationCapacity, digits);
    stringAppend(destination, destinationCapacity, " bytes used, ");
    stringWriteUnsigned(digits, sizeof(digits), arena->highWaterMarkInBytes);
    stringAppend(destination, destinationCapacity, digits);
    length = stringAppend(destination, destinationCapacity, " bytes peak\n");

    if (frameSummary.overflowCount > 0U)
    {
        stringAppend(destination, destinationCapacity, "warning: ");
        stringWriteUnsigned(digits, sizeof(digits), frameSummary.overflowCount);
        stringAppend(destination, destinationCapacity, digits);
        length = stringAppend(destination, destinationCapacity,
                              " profiler overflow(s): raise the zone or depth limit\n");
    }

    length = stringAppend(destination, destinationCapacity, "\nzone");
    lineStart = length - 4UL;
    length = appendColumn(destination, destinationCapacity, length, lineStart, REPORT_COLUMN_CALLS, "calls");
    length = appendColumn(destination, destinationCapacity, length, lineStart, REPORT_COLUMN_LAST, "last ms");
    length = appendColumn(destination, destinationCapacity, length, lineStart, REPORT_COLUMN_AVERAGE,
                          "average ms");
    length = appendColumn(destination, destinationCapacity, length, lineStart, REPORT_COLUMN_WORST, "worst ms");
    length = stringAppend(destination, destinationCapacity, "\n");

    for (index = 0U; index < summaryCount; index += 1U)
    {
        const ProfilerZoneSummary *summary = &summaries[index];
        char formatted[32];

        lineStart = length;

        for (depthIndex = 0U; depthIndex < summary->displayDepth; depthIndex += 1U)
        {
            length = stringAppend(destination, destinationCapacity, "  ");
        }
        length = stringAppend(destination, destinationCapacity, summary->zoneName);

        stringWriteUnsigned(digits, sizeof(digits), summary->lastCallCount);
        length = appendColumn(destination, destinationCapacity, length, lineStart, REPORT_COLUMN_CALLS, digits);

        writeMilliseconds(formatted, sizeof(formatted), summary->lastMicroseconds);
        length = appendColumn(destination, destinationCapacity, length, lineStart, REPORT_COLUMN_LAST, formatted);

        writeMilliseconds(formatted, sizeof(formatted), summary->averageMicroseconds);
        length = appendColumn(destination, destinationCapacity, length, lineStart, REPORT_COLUMN_AVERAGE,
                              formatted);

        writeMilliseconds(formatted, sizeof(formatted), summary->worstMicroseconds);
        length = appendColumn(destination, destinationCapacity, length, lineStart, REPORT_COLUMN_WORST, formatted);

        length = stringAppend(destination, destinationCapacity, "\n");
    }

    return length;
}

#else /* VICTORIA_PROFILER_ENABLED */

Boolean profilerInitialize(MemoryArena *arena)
{
    (void)arena;
    return BOOLEAN_TRUE;
}

Integer32 profilerRegisterZone(const char *zoneName)
{
    (void)zoneName;
    return -1;
}

void profilerBeginZone(Integer32 zoneIdentifier)
{
    (void)zoneIdentifier;
}

void profilerEndZone(void)
{
}

void profilerBeginFrame(void)
{
}

void profilerEndFrame(void)
{
}

void profilerGetFrameSummary(ProfilerFrameSummary *summaryOut)
{
    memoryFill(summaryOut, 0, sizeof(*summaryOut));
}

Unsigned32 profilerGetZoneSummaries(ProfilerZoneSummary *summariesOut, Unsigned32 summaryCapacity)
{
    (void)summariesOut;
    (void)summaryCapacity;
    return 0U;
}

Unsigned32 profilerGetFrameHistory(Unsigned64 *historyOut, Unsigned32 historyCapacity)
{
    (void)historyOut;
    (void)historyCapacity;
    return 0U;
}

void profilerReset(void)
{
}

MemorySize profilerWriteReport(char *destination, MemorySize destinationCapacity)
{
    if (destinationCapacity == 0UL)
    {
        return 0UL;
    }
    destination[0] = '\0';
    return stringAppend(destination, destinationCapacity, "profiler: compiled out\n");
}

#endif
