#include <stdio.h>

#include "utils/assert.h"
#include "utils/strings.h"
#include "victoria/discSurvey.h"
#include "victoria/memoryArena.h"
#include "victoria/virtualFileSystem.h"

#define DISC_CAPACITY (64UL * 1024UL)
#define ARENA_CAPACITY (512UL * 1024UL)
#define FILE_LIMIT 8U
#define LINE_LIMIT 256U
#define LINE_LENGTH 320U

static Unsigned8 discBytes[DISC_CAPACITY];
static MemorySize discSize = 0UL;
static Unsigned8 arenaStorage[ARENA_CAPACITY];
static Unsigned8 scratchStorage[ARENA_CAPACITY];

static char loggedLines[LINE_LIMIT][LINE_LENGTH];
static Unsigned32 loggedCount = 0U;

static Integer32 failureCount = 0;

typedef struct TestStore
{
    Unsigned32 readCount;
    Unsigned32 pendingCount;
    Boolean stutters;
    Boolean withhold;
} TestStore;

static VirtualReadResult testStoreRead(void *context, Unsigned64 offsetInBytes, MemorySize sizeInBytes,
                                       Unsigned8 *destination)
{
    TestStore *store = (TestStore *)context;
    MemorySize index;

    if (store->stutters)
    {
        store->withhold = store->withhold ? BOOLEAN_FALSE : BOOLEAN_TRUE;
        if (store->withhold)
        {
            store->pendingCount++;
            return VIRTUAL_READ_PENDING;
        }
    }
    if (offsetInBytes + (Unsigned64)sizeInBytes > (Unsigned64)discSize)
    {
        return VIRTUAL_READ_OUT_OF_RANGE;
    }
    for (index = 0UL; index < sizeInBytes; index++)
    {
        destination[index] = discBytes[(MemorySize)offsetInBytes + index];
    }
    store->readCount++;
    return VIRTUAL_READ_OK;
}

static void put16(Unsigned8 *at, Unsigned32 value)
{
    at[0] = (Unsigned8)(value & 0xFFU);
    at[1] = (Unsigned8)((value >> 8) & 0xFFU);
}

static void put32(Unsigned8 *at, Unsigned32 value)
{
    put16(at, value & 0xFFFFU);
    put16(at + 2, value >> 16);
}

static MemorySize nodeSizeFor(Unsigned32 format)
{
    if (format <= 0x8002U)
    {
        return 12UL;
    }
    if (format <= 0x8004U)
    {
        return 20UL;
    }
    if (format <= 0x8006U)
    {
        return 21UL;
    }
    return 23UL;
}

static MemorySize buildScript(Unsigned8 *destination, Unsigned32 format, const Unsigned32 *opcodes,
                              Unsigned32 opcodeCount, Unsigned32 claimedCount)
{
    MemorySize at = 76UL;
    MemorySize nodeSize = nodeSizeFor(format);
    Unsigned32 node;
    MemorySize index;

    for (index = 0UL; index < 76UL; index++)
    {
        destination[index] = 0U;
    }
    destination[0] = 't';
    destination[1] = 'r';
    destination[2] = 'e';
    destination[3] = 'e';
    put16(destination + 64, format);
    put16(destination + 66, claimedCount);
    if (format >= 0x8009U)
    {
        destination[at++] = 0U;
    }
    for (node = 0U; node < opcodeCount; node++)
    {
        for (index = 0UL; index < nodeSize; index++)
        {
            destination[at + index] = 0xAAU;
        }
        put16(destination + at, opcodes[node]);
        at += nodeSize;
    }
    return at;
}

static MemorySize refPackLiterally(Unsigned8 *destination, const Unsigned8 *source, MemorySize size)
{
    MemorySize at = 9UL;
    MemorySize read = 0UL;

    destination[4] = 0x10U;
    destination[5] = 0xFBU;
    destination[6] = (Unsigned8)((size >> 16) & 0xFFU);
    destination[7] = (Unsigned8)((size >> 8) & 0xFFU);
    destination[8] = (Unsigned8)(size & 0xFFU);
    while (size - read >= 4UL)
    {
        MemorySize run = size - read;
        MemorySize index;

        if (run > 112UL)
        {
            run = 112UL;
        }
        run &= ~(MemorySize)3UL;
        destination[at++] = (Unsigned8)(0xE0U + (Unsigned8)(run / 4UL - 1UL));
        for (index = 0UL; index < run; index++)
        {
            destination[at++] = source[read++];
        }
    }
    destination[at++] = (Unsigned8)(0xFCU + (Unsigned8)(size - read));
    while (read < size)
    {
        destination[at++] = source[read++];
    }
    put32(destination, (Unsigned32)at);
    return at;
}

typedef struct TestResource
{
    Unsigned32 typeIdentifier;
    Unsigned32 groupIdentifier;
    const Unsigned8 *bytes;
    MemorySize size;
} TestResource;

static MemorySize appendPackage(const TestResource *resources, Unsigned32 count)
{
    MemorySize start = discSize;
    MemorySize at = start + 96UL;
    MemorySize indexAt;
    Unsigned32 offsets[16];
    Unsigned32 which;
    MemorySize index;

    for (index = 0UL; index < 96UL; index++)
    {
        discBytes[start + index] = 0U;
    }
    discBytes[start + 0] = 'D';
    discBytes[start + 1] = 'B';
    discBytes[start + 2] = 'P';
    discBytes[start + 3] = 'F';
    put32(discBytes + start + 4, 1U);
    put32(discBytes + start + 8, 1U);
    put32(discBytes + start + 32, 7U);

    for (which = 0U; which < count; which++)
    {
        offsets[which] = (Unsigned32)(at - start);
        for (index = 0UL; index < resources[which].size; index++)
        {
            discBytes[at + index] = resources[which].bytes[index];
        }
        at += resources[which].size;
    }

    indexAt = at;
    for (which = 0U; which < count; which++)
    {
        put32(discBytes + at, resources[which].typeIdentifier);
        put32(discBytes + at + 4, resources[which].groupIdentifier);
        put32(discBytes + at + 8, 0x1000U + which);
        put32(discBytes + at + 12, 0U);
        put32(discBytes + at + 16, offsets[which]);
        put32(discBytes + at + 20, (Unsigned32)resources[which].size);
        at += 24UL;
    }
    put32(discBytes + start + 36, count);
    put32(discBytes + start + 40, (Unsigned32)(indexAt - start));
    put32(discBytes + start + 44, (Unsigned32)(at - indexAt));
    put32(discBytes + start + 60, 2U);

    discSize = at;
    return start;
}

static void rememberLine(const char *line)
{
    printf("      %s\n", line);
    if (loggedCount < LINE_LIMIT)
    {
        loggedLines[loggedCount][0] = '\0';
        stringAppend(loggedLines[loggedCount], LINE_LENGTH, line);
        loggedCount++;
    }
}

static Boolean someLineSays(const char *first, const char *second)
{
    Unsigned32 which;

    for (which = 0U; which < loggedCount; which++)
    {
        if (stringContainsIgnoringCase(loggedLines[which], first) &&
            stringContainsIgnoringCase(loggedLines[which], second))
        {
            return BOOLEAN_TRUE;
        }
    }
    return BOOLEAN_FALSE;
}

static Unsigned32 runSurvey(DiscSurvey *survey, VirtualFileSystem *fileSystem, MemoryArena *scratch,
                            MemorySize windowBytes)
{
    Unsigned32 steps = 0U;

    memoryArenaInitialize(scratch, scratchStorage, ARENA_CAPACITY);
    discSurveyBegin(survey, fileSystem, scratch);
    if (windowBytes > 0UL)
    {
        survey->windowBytes = windowBytes;
    }
    while (discSurveyStep(survey) == DISC_SURVEY_WORKING && steps < 100000U)
    {
        steps++;
    }
    return steps;
}

static const DiscSurveyTypeTally *findTally(const DiscSurvey *survey, Unsigned32 typeIdentifier)
{
    Unsigned32 which;

    for (which = 0U; which < survey->neighborhoodTypeCount; which++)
    {
        if (survey->neighborhoodTypes[which].typeIdentifier == typeIdentifier)
        {
            return &survey->neighborhoodTypes[which];
        }
    }
    return NULL_POINTER;
}

int main(void)
{
    static const Unsigned32 newOpcodes[] = { 0x0002U, 0x0002U, 0x0100U, 0x1001U, 0x001AU };
    static const Unsigned32 oldOpcodes[] = { 0x0000U, 0x2000U };
    static const Unsigned32 packedOpcodes[] = { 0x0002U, 0x0013U, 0x007EU };
    static const Unsigned8 record[8] = { 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U };
    static Unsigned8 newScript[256];
    static Unsigned8 oldScript[256];
    static Unsigned8 brokenScript[256];
    static Unsigned8 plainPacked[256];
    static Unsigned8 packedScript[320];
    MemorySize newSize;
    MemorySize oldSize;
    MemorySize brokenSize;
    MemorySize packedSize;
    MemorySize neighborhoodStart;
    MemorySize characterStart;
    MemorySize objectsStart;
    MemoryArena arena;
    MemoryArena scratch;
    VirtualFileSystem fileSystem;
    TestStore store;
    DiscSurvey survey;

    newSize = buildScript(newScript, 0x8009U, newOpcodes, 5U, 5U);
    oldSize = buildScript(oldScript, 0x8002U, oldOpcodes, 2U, 2U);
    brokenSize = buildScript(brokenScript, 0x8007U, oldOpcodes, 2U, 40U);
    packedSize = refPackLiterally(packedScript, plainPacked,
                                  buildScript(plainPacked, 0x8005U, packedOpcodes, 3U, 3U));

    {
        const TestResource neighborhood[] = {
            { 0xCC364C2AUL, 1U, record, sizeof(record) },
            { 0xCC364C2AUL, 1U, record, sizeof(record) },
            { 0x46414D49UL, 1U, record, sizeof(record) },
            { 0x12345678UL, 1U, record, sizeof(record) },
            { 0x4E474248UL, 1U, record, sizeof(record) }
        };
        const TestResource character[] = {
            { 0xCC364C2AUL, 1U, record, sizeof(record) },
            { 0xEBCF3E27UL, 1U, record, sizeof(record) }
        };
        const TestResource objects[] = {
            { 0x42484156UL, 0x7FD46CD0UL, newScript, newSize },
            { 0x4F424A44UL, 0x7F000001UL, record, sizeof(record) },
            { 0x42484156UL, 0x7F000001UL, oldScript, oldSize },
            { 0x42484156UL, 0x7F000001UL, brokenScript, brokenSize },
            { 0x42484156UL, 0x7F000001UL, packedScript, packedSize },
            { 0x54544142UL, 0x7F000001UL, record, sizeof(record) }
        };

        neighborhoodStart = appendPackage(neighborhood, 5U);
        characterStart = appendPackage(character, 2U);
        objectsStart = appendPackage(objects, 6U);
    }

    memoryArenaInitialize(&arena, arenaStorage, ARENA_CAPACITY);
    store.readCount = 0U;
    store.pendingCount = 0U;
    store.stutters = BOOLEAN_FALSE;
    store.withhold = BOOLEAN_FALSE;
    virtualFileSystemInitialize(&fileSystem, testStoreRead, &store, (Unsigned64)discSize);
    fileSystem.entries = (VirtualFileEntry *)memoryArenaAllocate(
        &arena, (MemorySize)FILE_LIMIT * sizeof(VirtualFileEntry), 8UL);
    fileSystem.entryCapacity = FILE_LIMIT;
    virtualFileSystemAddEntry(&fileSystem, "TSData/Res/UserData/Neighborhoods/N001/N001_Neighborhood.package",
                              (Unsigned64)neighborhoodStart,
                              (Unsigned64)(characterStart - neighborhoodStart));
    virtualFileSystemAddEntry(&fileSystem, "TSData/readme.txt", 0U, 16U);
    virtualFileSystemAddEntry(&fileSystem,
                              "TSData/Res/UserData/Neighborhoods/N001/Characters/N001_User00000.package",
                              (Unsigned64)characterStart, (Unsigned64)(objectsStart - characterStart));
    virtualFileSystemAddEntry(&fileSystem, "TSData/Res/Objects/objects.package", (Unsigned64)objectsStart,
                              (Unsigned64)(discSize - objectsStart));

    printf("-- surveying a disc built for the purpose --\n");
    {
        Unsigned32 steps = runSurvey(&survey, &fileSystem, &scratch, 0UL);
        const DiscSurveyTypeTally *relationships = findTally(&survey, 0xCC364C2AUL);

        checkThat(&failureCount, "the survey finishes", steps > 0U && steps < 100000U);
        checkThat(&failureCount, "three packages are surveyed, the text file is not",
                  survey.packagesSeen == 3U && survey.packagesRefused == 0U);
        checkThat(&failureCount, "every resource on the disc is seen", survey.resourcesSeen == 13U);

        checkThat(&failureCount, "two neighborhood packages, one main and one character",
                  survey.neighborhoodPackages[DISC_SURVEY_NEIGHBORHOOD_MAIN] == 1U &&
                      survey.neighborhoodPackages[DISC_SURVEY_NEIGHBORHOOD_CHARACTER] == 1U &&
                      survey.neighborhoodPackages[DISC_SURVEY_NEIGHBORHOOD_LOT] == 0U);
        checkThat(&failureCount, "seven neighborhood resources of five types",
                  survey.neighborhoodResources == 7U && survey.neighborhoodTypeCount == 5U);
        checkThat(&failureCount, "relationships: three records across two packages",
                  relationships != NULL_POINTER && relationships->resourceCount == 3U &&
                      relationships->packageCount == 2U);
        checkThat(&failureCount, "the objects package is not counted as a neighborhood",
                  findTally(&survey, 0x42484156UL) == NULL_POINTER);

        checkThat(&failureCount, "four trees are listed", survey.scriptsListed == 4U);
        checkThat(&failureCount, "three decode and one is refused as malformed",
                  survey.scriptsRead == 3U && survey.scriptsMalformed == 1U &&
                      survey.scriptsUnreadable == 0U);
        checkThat(&failureCount, "one came through RefPack", survey.scriptsCompressed == 1U);
        checkThat(&failureCount, "one sits in the global group", survey.scriptsInGlobalGroup == 1U);
        checkThat(&failureCount, "each format is counted where it belongs",
                  survey.formatCounts[9] == 1U && survey.formatCounts[2] == 1U &&
                      survey.formatCounts[5] == 1U && survey.formatCounts[7] == 0U);
        checkThat(&failureCount, "ten instructions in the trees that decoded",
                  survey.instructions == 10U && survey.largestScript == 5U);
        checkThat(&failureCount, "Expression is used three times, in two trees",
                  survey.primitiveUses[0x02] == 3U && survey.primitiveScripts[0x02] == 2U);
        checkThat(&failureCount, "the rarer primitives are each counted once",
                  survey.primitiveUses[0x00] == 1U && survey.primitiveUses[0x13] == 1U &&
                      survey.primitiveUses[0x1A] == 1U && survey.primitiveUses[0x7E] == 1U);
        checkThat(&failureCount, "a call of each kind is told apart",
                  survey.globalCalls == 1U && survey.privateCalls == 1U &&
                      survey.semiGlobalCalls == 1U);
        checkThat(&failureCount, "the other machine resources are counted",
                  survey.familyResources[0] == 4U && survey.familyResources[1] == 1U &&
                      survey.familyResources[3] == 1U && survey.familyPackages[0] == 1U);
        checkThat(&failureCount, "neighbouring trees are read in a single request",
                  survey.scriptReads == 1U);
        checkThat(&failureCount, "and the scratch memory is all given back",
                  memoryArenaGetMarker(&scratch) == 0UL);
    }

    printf("\n-- the report it logs --\n");
    {
        discSurveyReport(&survey, rememberLine);
        checkThat(&failureCount, "a report is logged", loggedCount > 10U);
        checkThat(&failureCount, "relationships are named, and marked as understood by name only",
                  someLineSays("SREL sim relationship", "name only"));
        checkThat(&failureCount, "a type nobody knows is called unknown",
                  someLineSays("0x12345678", "UNKNOWN"));
        checkThat(&failureCount, "the most used primitive is reported with its name",
                  someLineSays("0x02  x3", "Expression"));
        checkThat(&failureCount, "the later primitives are named too",
                  someLineSays("0x7E", "Lua"));
        checkThat(&failureCount, "and the report says it is done", someLineSays("survey: done", ""));
    }

    printf("\n-- again, one tree per request and through a store that stutters --\n");
    {
        DiscSurvey stuttered;
        Unsigned32 steps;

        store.stutters = BOOLEAN_TRUE;
        store.withhold = BOOLEAN_FALSE;
        store.pendingCount = 0U;
        steps = runSurvey(&stuttered, &fileSystem, &scratch, 64UL);

        checkThat(&failureCount, "it finishes", steps < 100000U);
        checkThat(&failureCount, "the store did withhold", store.pendingCount > 0U);
        checkThat(&failureCount, "the trees are read one at a time", stuttered.scriptReads == 4U);
        checkThat(&failureCount, "and the counts are the same",
                  stuttered.scriptsRead == survey.scriptsRead &&
                      stuttered.instructions == survey.instructions &&
                      stuttered.primitiveUses[0x02] == survey.primitiveUses[0x02] &&
                      stuttered.neighborhoodResources == survey.neighborhoodResources);
        store.stutters = BOOLEAN_FALSE;
    }

    printf("\n-- a scratch arena too small for an index --\n");
    {
        DiscSurvey cramped;
        MemoryArena tiny;
        Unsigned32 steps = 0U;

        memoryArenaInitialize(&tiny, scratchStorage, 64UL);
        discSurveyBegin(&cramped, &fileSystem, &tiny);
        while (discSurveyStep(&cramped) == DISC_SURVEY_WORKING && steps < 100000U)
        {
            steps++;
        }
        checkThat(&failureCount, "it still finishes", steps < 100000U);
        checkThat(&failureCount, "and says which packages it could not hold",
                  cramped.packagesTooLarge == 3U && cramped.packagesSeen == 0U);
    }

    return (int)checkSummarize(failureCount, "disc survey");
}
