#include <stdio.h>

#include "utils/assert.h"
#include "utils/strings.h"
#include "victoria/memoryArena.h"
#include "victoria/simanticsMachine.h"
#include "victoria/simanticsRehearsal.h"
#include "victoria/virtualFileSystem.h"

#define DISC_CAPACITY (64UL * 1024UL)
#define KEEP_CAPACITY (1024UL * 1024UL)
#define SCRATCH_CAPACITY (512UL * 1024UL)
#define SCRIPT_CAPACITY 512UL
#define NODE_BYTES 23UL
#define FILE_LIMIT 4U
#define RESOURCE_LIMIT 16U
#define LINE_LIMIT 64U
#define LINE_LENGTH 320U

static Unsigned8 discBytes[DISC_CAPACITY];
static MemorySize discSize = 0UL;
static Unsigned8 keepStorage[KEEP_CAPACITY];
static Unsigned8 scratchStorage[SCRATCH_CAPACITY];
static VirtualFileEntry fileTable[FILE_LIMIT];
static SimanticsRehearsal rehearsal;

static char loggedLines[LINE_LIMIT][LINE_LENGTH];
static Unsigned32 loggedCount = 0U;

static Integer32 failureCount = 0;

typedef struct Script
{
    Unsigned8 bytes[SCRIPT_CAPACITY];
    MemorySize size;
} Script;

typedef struct TestResource
{
    Unsigned32 typeIdentifier;
    Unsigned32 groupIdentifier;
    Unsigned32 instance;
    const Unsigned8 *bytes;
    MemorySize size;
} TestResource;

static VirtualReadResult discRead(void *context, Unsigned64 offsetInBytes, MemorySize sizeInBytes,
                                  Unsigned8 *destination)
{
    MemorySize index;

    (void)context;
    if (offsetInBytes + (Unsigned64)sizeInBytes > (Unsigned64)discSize)
    {
        return VIRTUAL_READ_OUT_OF_RANGE;
    }
    for (index = 0UL; index < sizeInBytes; index++)
    {
        destination[index] = discBytes[(MemorySize)offsetInBytes + index];
    }
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

static void beginScript(Script *script, const char *name)
{
    MemorySize which;

    for (which = 0UL; which < SCRIPT_CAPACITY; which++)
    {
        script->bytes[which] = 0U;
    }
    for (which = 0UL; name[which] != '\0'; which++)
    {
        script->bytes[which] = (Unsigned8)name[which];
    }
    put16(script->bytes + 64, 0x8007U);
    script->size = 76UL;
}

static Unsigned8 *addNode(Script *script, Unsigned16 opcode, Unsigned16 trueTarget, Unsigned16 falseTarget)
{
    Unsigned8 *node = script->bytes + script->size;

    put16(node, opcode);
    put16(node + 2, trueTarget);
    put16(node + 4, falseTarget);
    script->size += NODE_BYTES;
    put16(script->bytes + 66, (Unsigned32)((script->size - 76UL) / NODE_BYTES));
    return node + 7;
}

static void addExpression(Script *script, Unsigned8 leftOwner, Unsigned16 leftData, Unsigned8 operator,
                          Unsigned8 rightOwner, Unsigned16 rightData, Unsigned16 trueTarget, Unsigned16 falseTarget)
{
    Unsigned8 *operands = addNode(script, SIMANTICS_PRIMITIVE_EXPRESSION, trueTarget, falseTarget);

    put16(operands, leftData);
    put16(operands + 2, rightData);
    operands[5] = operator;
    operands[6] = leftOwner;
    operands[7] = rightOwner;
}

static void appendPackage(const char *path, VirtualFileSystem *fileSystem, const TestResource *resources,
                          Unsigned32 count)
{
    MemorySize start = discSize;
    MemorySize at = start + 96UL;
    MemorySize indexAt;
    Unsigned32 offsets[RESOURCE_LIMIT];
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
        put32(discBytes + at + 8, resources[which].instance);
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
    (void)virtualFileSystemAddEntry(fileSystem, path, (Unsigned64)start, (Unsigned64)(at - start));
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

static const SimanticsRehearsalStop *findStop(SimanticsFailure failure, Unsigned32 detail)
{
    Unsigned32 which;

    for (which = 0U; which < rehearsal.stopCount; which++)
    {
        if (rehearsal.stops[which].failure == failure && rehearsal.stops[which].detail == detail)
        {
            return &rehearsal.stops[which];
        }
    }
    return NULL_POINTER;
}

int main(void)
{
    static Script checksConstant;
    static Script setsToNext;
    static Script answersNo;
    static Script readsMotive;
    static Script callsPrivate;
    static Script sleepsForever;
    static Script stranger;
    static Unsigned8 table[64 + 2 + 4];
    Unsigned8 *operands;
    VirtualFileSystem fileSystem;
    MemoryArena keep;
    MemoryArena scratch;
    Unsigned32 steps = 0U;

    beginScript(&checksConstant, "checks a constant, then looks for someone");
    addExpression(&checksConstant, SIMANTICS_OWNER_TEMP, 0U, SIMANTICS_OPERATOR_ASSIGN, SIMANTICS_OWNER_CONSTANT,
                  (Unsigned16)((2U << 13) | 1U), 1U, SIMANTICS_TARGET_ERROR);
    addExpression(&checksConstant, SIMANTICS_OWNER_TEMP, 0U, SIMANTICS_OPERATOR_EQUAL, SIMANTICS_OWNER_LITERAL, 7U,
                  2U, SIMANTICS_TARGET_FALSE);
    (void)addNode(&checksConstant, 0x0101U, SIMANTICS_TARGET_TRUE, SIMANTICS_TARGET_FALSE);

    beginScript(&setsToNext, "looks for the closest person");
    operands = addNode(&setsToNext, SIMANTICS_PRIMITIVE_SET_TO_NEXT, SIMANTICS_TARGET_TRUE, SIMANTICS_TARGET_FALSE);
    operands[4] = 0x0DU;

    beginScript(&answersNo, "answers no");
    addExpression(&answersNo, SIMANTICS_OWNER_TEMP, 0U, SIMANTICS_OPERATOR_EQUAL, SIMANTICS_OWNER_LITERAL, 1U,
                  SIMANTICS_TARGET_TRUE, SIMANTICS_TARGET_FALSE);

    beginScript(&readsMotive, "reads the stack object's temp");
    addExpression(&readsMotive, SIMANTICS_OWNER_TEMP, 0U, SIMANTICS_OPERATOR_ASSIGN, 0x0DU, 3U,
                  SIMANTICS_TARGET_TRUE, SIMANTICS_TARGET_ERROR);

    beginScript(&callsPrivate, "calls a private tree");
    (void)addNode(&callsPrivate, 0x1001U, SIMANTICS_TARGET_TRUE, SIMANTICS_TARGET_FALSE);

    beginScript(&sleepsForever, "sleeps forever");
    operands = addNode(&sleepsForever, SIMANTICS_PRIMITIVE_SLEEP, 0U, SIMANTICS_TARGET_ERROR);
    put16(operands, 0U);

    beginScript(&stranger, "belongs to an object");
    (void)addNode(&stranger, 0x0013U, SIMANTICS_TARGET_TRUE, SIMANTICS_TARGET_FALSE);

    put16(table + 64, 2U);
    put16(table + 66, 5U);
    put16(table + 68, 7U);

    virtualFileSystemInitialize(&fileSystem, discRead, NULL_POINTER, 0U);
    fileSystem.entries = fileTable;
    fileSystem.entryCapacity = FILE_LIMIT;
    {
        const TestResource globals[] = {
            { 0x42484156UL, SIMANTICS_GLOBAL_GROUP, 0x0100U, checksConstant.bytes, checksConstant.size },
            { 0x42484156UL, SIMANTICS_GLOBAL_GROUP, 0x0101U, setsToNext.bytes, setsToNext.size },
            { 0x42434F4EUL, SIMANTICS_GLOBAL_GROUP, 0x0100U, table, sizeof(table) },
            { 0x42484156UL, SIMANTICS_GLOBAL_GROUP, 0x0102U, answersNo.bytes, answersNo.size },
            { 0x42484156UL, SIMANTICS_GLOBAL_GROUP, 0x0103U, readsMotive.bytes, readsMotive.size },
            { 0x42484156UL, SIMANTICS_GLOBAL_GROUP, 0x0104U, callsPrivate.bytes, callsPrivate.size },
            { 0x42484156UL, SIMANTICS_GLOBAL_GROUP, 0x0105U, sleepsForever.bytes, sleepsForever.size }
        };
        const TestResource objects[] = {
            { 0x42484156UL, 0x7F000001UL, 0x1000U, stranger.bytes, stranger.size }
        };

        appendPackage("TSData/Res/Objects/objects.package", &fileSystem, objects, 1U);
        appendPackage("TSData/Res/Objects/globals.package", &fileSystem, globals, 7U);
    }
    fileSystem.storeSizeInBytes = (Unsigned64)discSize;

    memoryArenaInitialize(&keep, keepStorage, KEEP_CAPACITY);
    memoryArenaInitialize(&scratch, scratchStorage, SCRATCH_CAPACITY);

    printf("-- rehearsing the global trees of a disc built for the purpose --\n");
    checkThat(&failureCount, "the rehearsal begins",
              simanticsRehearsalBegin(&rehearsal, &fileSystem, &keep, &scratch));
    while (simanticsRehearsalStep(&rehearsal) == SIMANTICS_REHEARSAL_WORKING && steps < 100000U)
    {
        steps++;
    }
    checkThat(&failureCount, "it finishes", steps < 100000U);
    checkThat(&failureCount, "both packages are indexed", rehearsal.packagesIndexed == 2U);
    checkThat(&failureCount, "the six global trees load, the object's own does not",
              rehearsal.treeCount == 6U && rehearsal.treesRefused == 0U && rehearsal.treesReplaced == 0U);
    checkThat(&failureCount, "the global constant table loads", rehearsal.tableCount == 1U);
    checkThat(&failureCount, "the trees are sorted for lookup",
              rehearsal.trees[0].instance == 0x0100U && rehearsal.trees[5].instance == 0x0105U);

    checkThat(&failureCount, "the tree that reads its constant gets 7 and calls on into the unbuilt search",
              findStop(SIMANTICS_FAILURE_UNSUPPORTED_SEARCH, 0x0DU) != NULL_POINTER &&
                  findStop(SIMANTICS_FAILURE_UNSUPPORTED_SEARCH, 0x0DU)->count == 2U);
    checkThat(&failureCount, "one tree answers false", rehearsal.returnedFalse == 1U);
    checkThat(&failureCount, "a missing data owner is tallied by owner",
              findStop(SIMANTICS_FAILURE_UNSUPPORTED_OWNER, 0x0DU) != NULL_POINTER);
    checkThat(&failureCount, "a private call is tallied as needing a real object",
              findStop(SIMANTICS_FAILURE_NO_SUCH_TREE, 1U) != NULL_POINTER);
    checkThat(&failureCount, "the sleeper is still waiting when its ticks run out", rehearsal.stillWaiting == 1U);
    checkThat(&failureCount, "four stopped in all", rehearsal.stopped == 4U);
    checkThat(&failureCount, "instructions are counted", rehearsal.instructionsRun > 6U);

    printf("\n-- the report it logs --\n");
    simanticsRehearsalReport(&rehearsal, rememberLine);
    checkThat(&failureCount, "the most common stop comes first, named",
              loggedCount > 3U && stringContainsIgnoringCase(loggedLines[3], "x2  Set To Next search 0x0D person closest"));
    checkThat(&failureCount, "with a tree that hit it", someLineSays("Set To Next", "0x0100 \"checks a constant"));
    checkThat(&failureCount, "the data owner is named", someLineSays("data owner 0x0D", "stack object's temp"));
    checkThat(&failureCount, "and it says it is done", someLineSays("rehearsal: done", ""));

    return (int)checkSummarize(failureCount, "SimAntics rehearsal");
}
