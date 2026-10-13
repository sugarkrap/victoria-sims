#include "victoria/simanticsRehearsal.h"

#include "utils/strings.h"
#include "victoria/compression.h"
#include "victoria/simanticsNames.h"

#define HEADER_SIZE 96UL
#define OFFSET_INDEX_ENTRY_COUNT 36UL
#define OFFSET_INDEX_OFFSET 40UL
#define OFFSET_INDEX_SIZE 44UL

#define ENTRY_SIZE_WITHOUT_INSTANCE_HIGH 20UL
#define ENTRY_SIZE_WITH_INSTANCE_HIGH 24UL

#define WINDOW_BYTES (1024UL * 1024UL)

#define TABLE_NAME_BYTES 64UL
#define TABLE_COUNT_MASK 0x7FFFU

#define REHEARSAL_RANDOM_SEED 1U
#define REHEARSAL_OBJECT_COUNT 2U

#define CALL_SCOPE_GLOBAL 0U
#define CALL_SCOPE_PRIVATE 1U
#define CALL_SCOPE_SEMIGLOBAL 2U

#define LINE_CAPACITY 320UL

static Unsigned32 readUnsigned32(const Unsigned8 *bytes, MemorySize offset)
{
    return (Unsigned32)bytes[offset] | ((Unsigned32)bytes[offset + 1UL] << 8) |
           ((Unsigned32)bytes[offset + 2UL] << 16) | ((Unsigned32)bytes[offset + 3UL] << 24);
}

static Unsigned16 readUnsigned16(const Unsigned8 *bytes, MemorySize offset)
{
    return (Unsigned16)((Unsigned16)bytes[offset] | (Unsigned16)((Unsigned16)bytes[offset + 1UL] << 8));
}

static const SimanticsRehearsalTree *findLoadedTree(const SimanticsRehearsal *rehearsal, Unsigned16 instance)
{
    Unsigned32 low = 0U;
    Unsigned32 high = rehearsal->treeCount;

    while (low < high)
    {
        Unsigned32 middle = low + (high - low) / 2U;

        if (rehearsal->trees[middle].instance < instance)
        {
            low = middle + 1U;
        }
        else
        {
            high = middle;
        }
    }
    return (low < rehearsal->treeCount && rehearsal->trees[low].instance == instance) ? &rehearsal->trees[low]
                                                                                       : NULL_POINTER;
}

static Unsigned32 findLoadedTable(const SimanticsRehearsal *rehearsal, Unsigned16 instance)
{
    Unsigned32 which;

    for (which = 0U; which < rehearsal->tableCount; which++)
    {
        if (rehearsal->tables[which].instance == instance)
        {
            return which;
        }
    }
    return rehearsal->tableCount;
}

static const SimanticsTree *hostFindTree(void *context, Unsigned32 groupIdentifier, Unsigned16 instance)
{
    const SimanticsRehearsalTree *found;

    if (groupIdentifier != SIMANTICS_GLOBAL_GROUP)
    {
        return NULL_POINTER;
    }
    found = findLoadedTree((const SimanticsRehearsal *)context, instance);
    return (found != NULL_POINTER) ? &found->tree : NULL_POINTER;
}

static Boolean hostFindConstant(void *context, Unsigned32 groupIdentifier, Unsigned16 instance, Unsigned16 index,
                                Integer16 *value)
{
    const SimanticsRehearsalTable *table;
    const SimanticsRehearsal *rehearsal = (const SimanticsRehearsal *)context;
    Unsigned32 found;

    if (groupIdentifier != SIMANTICS_GLOBAL_GROUP)
    {
        return BOOLEAN_FALSE;
    }
    found = findLoadedTable(rehearsal, instance);
    if (found >= rehearsal->tableCount || index >= rehearsal->tables[found].count)
    {
        return BOOLEAN_FALSE;
    }
    table = &rehearsal->tables[found];
    *value = table->values[index];
    return BOOLEAN_TRUE;
}

static void countInstruction(void *context, const SimanticsThread *thread, const SimanticsInstruction *instruction,
                             Unsigned16 target)
{
    (void)thread;
    (void)instruction;
    (void)target;
    ((SimanticsRehearsal *)context)->instructionsRun++;
}

Boolean simanticsRehearsalBegin(SimanticsRehearsal *rehearsal, VirtualFileSystem *fileSystem, MemoryArena *keep,
                                MemoryArena *scratch)
{
    MemorySize byteIndex;
    Unsigned8 *raw = (Unsigned8 *)rehearsal;

    for (byteIndex = 0UL; byteIndex < sizeof(*rehearsal); byteIndex++)
    {
        raw[byteIndex] = 0U;
    }
    rehearsal->fileSystem = fileSystem;
    rehearsal->keep = keep;
    rehearsal->scratch = scratch;
    rehearsal->stage = SIMANTICS_REHEARSAL_STAGE_HEADER;
    rehearsal->resources = (SimanticsRehearsalResource *)memoryArenaAllocate(
        keep, (MemorySize)SIMANTICS_REHEARSAL_RESOURCE_LIMIT * sizeof(SimanticsRehearsalResource), 4UL);
    rehearsal->trees = (SimanticsRehearsalTree *)memoryArenaAllocate(
        keep, (MemorySize)SIMANTICS_REHEARSAL_TREE_LIMIT * sizeof(SimanticsRehearsalTree), 8UL);
    rehearsal->tables = (SimanticsRehearsalTable *)memoryArenaAllocate(
        keep, (MemorySize)SIMANTICS_REHEARSAL_TABLE_LIMIT * sizeof(SimanticsRehearsalTable), 8UL);
    return (rehearsal->resources != NULL_POINTER && rehearsal->trees != NULL_POINTER &&
            rehearsal->tables != NULL_POINTER)
               ? BOOLEAN_TRUE
               : BOOLEAN_FALSE;
}

static void nextPackage(SimanticsRehearsal *rehearsal)
{
    rehearsal->nextFileIndex++;
    rehearsal->stage = SIMANTICS_REHEARSAL_STAGE_HEADER;
}

static SimanticsRehearsalStatus readHeader(SimanticsRehearsal *rehearsal)
{
    const VirtualFileEntry *entry;
    Unsigned8 header[HEADER_SIZE];
    VirtualReadResult read;
    MemorySize indexEnd;

    if (rehearsal->nextFileIndex >= rehearsal->fileSystem->entryCount)
    {
        rehearsal->stage = SIMANTICS_REHEARSAL_STAGE_LOAD;
        return SIMANTICS_REHEARSAL_WORKING;
    }
    entry = virtualFileSystemGetEntry(rehearsal->fileSystem, rehearsal->nextFileIndex);
    if (entry == NULL_POINTER || !stringEndsWithIgnoringCase(entry->path, ".package") ||
        entry->sizeInBytes < (Unsigned64)HEADER_SIZE)
    {
        nextPackage(rehearsal);
        return SIMANTICS_REHEARSAL_WORKING;
    }
    read = virtualFileSystemReadFile(rehearsal->fileSystem, rehearsal->nextFileIndex, 0U, HEADER_SIZE, header);
    if (read == VIRTUAL_READ_PENDING)
    {
        return SIMANTICS_REHEARSAL_WORKING;
    }
    if (read != VIRTUAL_READ_OK || header[0] != 'D' || header[1] != 'B' || header[2] != 'P' || header[3] != 'F')
    {
        rehearsal->packagesRefused++;
        nextPackage(rehearsal);
        return SIMANTICS_REHEARSAL_WORKING;
    }
    rehearsal->pendingEntryCount = readUnsigned32(header, OFFSET_INDEX_ENTRY_COUNT);
    rehearsal->pendingIndexOffset = readUnsigned32(header, OFFSET_INDEX_OFFSET);
    rehearsal->pendingIndexSize = readUnsigned32(header, OFFSET_INDEX_SIZE);
    indexEnd = (MemorySize)rehearsal->pendingIndexOffset + (MemorySize)rehearsal->pendingIndexSize;
    if (rehearsal->pendingEntryCount == 0U || rehearsal->pendingIndexSize == 0U ||
        indexEnd < (MemorySize)rehearsal->pendingIndexOffset || (Unsigned64)indexEnd > entry->sizeInBytes)
    {
        rehearsal->packagesRefused++;
        nextPackage(rehearsal);
        return SIMANTICS_REHEARSAL_WORKING;
    }
    rehearsal->stage = SIMANTICS_REHEARSAL_STAGE_INDEX;
    return SIMANTICS_REHEARSAL_WORKING;
}

static SimanticsRehearsalStatus readIndex(SimanticsRehearsal *rehearsal)
{
    MemorySize marker = memoryArenaGetMarker(rehearsal->scratch);
    MemorySize entrySize = (MemorySize)rehearsal->pendingIndexSize / (MemorySize)rehearsal->pendingEntryCount;
    Unsigned8 *indexBytes;
    VirtualReadResult read;
    Unsigned32 which;

    if ((entrySize != ENTRY_SIZE_WITHOUT_INSTANCE_HIGH && entrySize != ENTRY_SIZE_WITH_INSTANCE_HIGH) ||
        entrySize * (MemorySize)rehearsal->pendingEntryCount != (MemorySize)rehearsal->pendingIndexSize)
    {
        rehearsal->packagesRefused++;
        nextPackage(rehearsal);
        return SIMANTICS_REHEARSAL_WORKING;
    }
    indexBytes = (Unsigned8 *)memoryArenaAllocate(rehearsal->scratch, (MemorySize)rehearsal->pendingIndexSize, 4UL);
    if (indexBytes == NULL_POINTER)
    {
        rehearsal->packagesRefused++;
        nextPackage(rehearsal);
        return SIMANTICS_REHEARSAL_WORKING;
    }
    read = virtualFileSystemReadFile(rehearsal->fileSystem, rehearsal->nextFileIndex,
                                     (Unsigned64)rehearsal->pendingIndexOffset,
                                     (MemorySize)rehearsal->pendingIndexSize, indexBytes);
    if (read == VIRTUAL_READ_PENDING)
    {
        memoryArenaRewindToMarker(rehearsal->scratch, marker);
        return SIMANTICS_REHEARSAL_WORKING;
    }
    if (read != VIRTUAL_READ_OK)
    {
        memoryArenaRewindToMarker(rehearsal->scratch, marker);
        rehearsal->packagesRefused++;
        nextPackage(rehearsal);
        return SIMANTICS_REHEARSAL_WORKING;
    }

    for (which = 0U; which < rehearsal->pendingEntryCount; which++)
    {
        MemorySize at = (MemorySize)which * entrySize;
        MemorySize locationAt = at + ((entrySize == ENTRY_SIZE_WITH_INSTANCE_HIGH) ? 16UL : 12UL);
        Unsigned32 typeIdentifier = readUnsigned32(indexBytes, at);
        SimanticsRehearsalResource *resource;

        if ((typeIdentifier != SIMANTICS_REHEARSAL_TREE_TYPE && typeIdentifier != SIMANTICS_REHEARSAL_TABLE_TYPE) ||
            readUnsigned32(indexBytes, at + 4UL) != SIMANTICS_GLOBAL_GROUP)
        {
            continue;
        }
        if (rehearsal->resourceCount >= SIMANTICS_REHEARSAL_RESOURCE_LIMIT)
        {
            rehearsal->resourcesDropped++;
            continue;
        }
        resource = &rehearsal->resources[rehearsal->resourceCount];
        rehearsal->resourceCount++;
        resource->typeIdentifier = typeIdentifier;
        resource->fileIndex = rehearsal->nextFileIndex;
        resource->instance = (Unsigned16)readUnsigned32(indexBytes, at + 8UL);
        resource->offsetInBytes = readUnsigned32(indexBytes, locationAt);
        resource->sizeInBytes = readUnsigned32(indexBytes, locationAt + 4UL);
    }
    memoryArenaRewindToMarker(rehearsal->scratch, marker);
    rehearsal->packagesIndexed++;
    nextPackage(rehearsal);
    return SIMANTICS_REHEARSAL_WORKING;
}

static void keepTree(SimanticsRehearsal *rehearsal, Unsigned16 instance, const Unsigned8 *bytes, MemorySize size)
{
    SimanticsRehearsalTree *slot = NULL_POINTER;
    SimanticsTree decoded;
    Unsigned32 which;

    if (simanticsTreeRead(&decoded, bytes, size, rehearsal->keep) != SIMANTICS_TREE_OK)
    {
        rehearsal->treesRefused++;
        return;
    }
    for (which = 0U; which < rehearsal->treeCount; which++)
    {
        if (rehearsal->trees[which].instance == instance)
        {
            slot = &rehearsal->trees[which];
        }
    }
    if (slot == NULL_POINTER)
    {
        if (rehearsal->treeCount >= SIMANTICS_REHEARSAL_TREE_LIMIT)
        {
            rehearsal->treesRefused++;
            return;
        }
        slot = &rehearsal->trees[rehearsal->treeCount];
        rehearsal->treeCount++;
    }
    else
    {
        rehearsal->treesReplaced++;
    }
    slot->instance = instance;
    slot->tree = decoded;
}

static void keepTable(SimanticsRehearsal *rehearsal, Unsigned16 instance, const Unsigned8 *bytes, MemorySize size)
{
    SimanticsRehearsalTable *table;
    Integer16 *values;
    Unsigned16 count;
    Unsigned32 which;

    if (size < TABLE_NAME_BYTES + 2UL)
    {
        rehearsal->tablesRefused++;
        return;
    }
    count = (Unsigned16)(readUnsigned16(bytes, TABLE_NAME_BYTES) & TABLE_COUNT_MASK);
    if (TABLE_NAME_BYTES + 2UL + (MemorySize)count * 2UL > size)
    {
        rehearsal->tablesRefused++;
        return;
    }
    which = findLoadedTable(rehearsal, instance);
    if (which < rehearsal->tableCount)
    {
        table = &rehearsal->tables[which];
    }
    else
    {
        if (rehearsal->tableCount >= SIMANTICS_REHEARSAL_TABLE_LIMIT)
        {
            rehearsal->tablesRefused++;
            return;
        }
        table = &rehearsal->tables[rehearsal->tableCount];
        rehearsal->tableCount++;
    }
    values = (Integer16 *)memoryArenaAllocate(rehearsal->keep, (MemorySize)count * sizeof(Integer16) + 2UL, 2UL);
    if (values == NULL_POINTER)
    {
        rehearsal->tablesRefused++;
        table->count = 0U;
        return;
    }
    for (which = 0U; which < count; which++)
    {
        values[which] = (Integer16)readUnsigned16(bytes, TABLE_NAME_BYTES + 2UL + (MemorySize)which * 2UL);
    }
    table->instance = instance;
    table->count = count;
    table->values = values;
}

static void keepResource(SimanticsRehearsal *rehearsal, const SimanticsRehearsalResource *resource,
                         const Unsigned8 *bytes)
{
    MemorySize size = (MemorySize)resource->sizeInBytes;
    MemorySize marker = memoryArenaGetMarker(rehearsal->scratch);

    if (compressionLooksLikeRefPack(bytes, size))
    {
        MemorySize unpackedSize = compressionGetDecompressedSize(bytes, size);
        Unsigned8 *unpacked = (Unsigned8 *)memoryArenaAllocate(rehearsal->scratch, unpackedSize, 4UL);

        if (unpacked == NULL_POINTER ||
            compressionDecompressRefPack(unpacked, unpackedSize, bytes, size, &unpackedSize) != COMPRESSION_OK)
        {
            memoryArenaRewindToMarker(rehearsal->scratch, marker);
            if (resource->typeIdentifier == SIMANTICS_REHEARSAL_TREE_TYPE)
            {
                rehearsal->treesRefused++;
            }
            else
            {
                rehearsal->tablesRefused++;
            }
            return;
        }
        bytes = unpacked;
        size = unpackedSize;
    }
    if (resource->typeIdentifier == SIMANTICS_REHEARSAL_TREE_TYPE)
    {
        keepTree(rehearsal, resource->instance, bytes, size);
    }
    else
    {
        keepTable(rehearsal, resource->instance, bytes, size);
    }
    memoryArenaRewindToMarker(rehearsal->scratch, marker);
}

static void sortTrees(SimanticsRehearsal *rehearsal)
{
    Unsigned32 which;

    for (which = 1U; which < rehearsal->treeCount; which++)
    {
        SimanticsRehearsalTree moving = rehearsal->trees[which];
        Unsigned32 at = which;

        while (at > 0U && rehearsal->trees[at - 1U].instance > moving.instance)
        {
            rehearsal->trees[at] = rehearsal->trees[at - 1U];
            at--;
        }
        rehearsal->trees[at] = moving;
    }
}

static void beginRunning(SimanticsRehearsal *rehearsal)
{
    SimanticsHost host;

    sortTrees(rehearsal);
    host.context = rehearsal;
    host.findTree = hostFindTree;
    host.findConstant = hostFindConstant;
    if (!simanticsMachineBegin(&rehearsal->machine, &host, rehearsal->keep, REHEARSAL_OBJECT_COUNT,
                               REHEARSAL_RANDOM_SEED))
    {
        rehearsal->stage = SIMANTICS_REHEARSAL_STAGE_DONE;
        return;
    }
    rehearsal->machine.trace = countInstruction;
    rehearsal->machine.traceContext = rehearsal;
    rehearsal->stage = SIMANTICS_REHEARSAL_STAGE_RUN;
}

static SimanticsRehearsalStatus loadResources(SimanticsRehearsal *rehearsal)
{
    const SimanticsRehearsalResource *first;
    Unsigned32 windowStart;
    Unsigned32 windowEnd;
    Unsigned32 last;
    MemorySize marker = memoryArenaGetMarker(rehearsal->scratch);
    Unsigned8 *window;
    VirtualReadResult read;
    Unsigned32 which;

    if (rehearsal->loadCursor >= rehearsal->resourceCount)
    {
        beginRunning(rehearsal);
        return SIMANTICS_REHEARSAL_WORKING;
    }
    first = &rehearsal->resources[rehearsal->loadCursor];
    windowStart = first->offsetInBytes;
    windowEnd = first->offsetInBytes + first->sizeInBytes;
    last = rehearsal->loadCursor + 1U;
    while (last < rehearsal->resourceCount)
    {
        const SimanticsRehearsalResource *next = &rehearsal->resources[last];
        Unsigned32 nextEnd = next->offsetInBytes + next->sizeInBytes;

        if (next->fileIndex != first->fileIndex || next->offsetInBytes < windowStart ||
            nextEnd < next->offsetInBytes ||
            (MemorySize)(((nextEnd > windowEnd) ? nextEnd : windowEnd) - windowStart) > WINDOW_BYTES)
        {
            break;
        }
        if (nextEnd > windowEnd)
        {
            windowEnd = nextEnd;
        }
        last++;
    }

    window = (windowEnd < windowStart)
                 ? NULL_POINTER
                 : (Unsigned8 *)memoryArenaAllocate(rehearsal->scratch, (MemorySize)(windowEnd - windowStart), 8UL);
    if (window == NULL_POINTER)
    {
        rehearsal->treesRefused += last - rehearsal->loadCursor;
        rehearsal->loadCursor = last;
        return SIMANTICS_REHEARSAL_WORKING;
    }
    read = virtualFileSystemReadFile(rehearsal->fileSystem, first->fileIndex, (Unsigned64)windowStart,
                                     (MemorySize)(windowEnd - windowStart), window);
    if (read == VIRTUAL_READ_PENDING)
    {
        memoryArenaRewindToMarker(rehearsal->scratch, marker);
        return SIMANTICS_REHEARSAL_WORKING;
    }
    for (which = rehearsal->loadCursor; which < last; which++)
    {
        const SimanticsRehearsalResource *resource = &rehearsal->resources[which];

        if (read != VIRTUAL_READ_OK)
        {
            rehearsal->treesRefused++;
            continue;
        }
        keepResource(rehearsal, resource, window + (resource->offsetInBytes - windowStart));
    }
    memoryArenaRewindToMarker(rehearsal->scratch, marker);
    rehearsal->loadCursor = last;
    return SIMANTICS_REHEARSAL_WORKING;
}

static Unsigned32 stopDetail(const SimanticsThread *thread)
{
    switch (thread->failure)
    {
    case SIMANTICS_FAILURE_UNSUPPORTED_PRIMITIVE:
    case SIMANTICS_FAILURE_UNSUPPORTED_OWNER:
    case SIMANTICS_FAILURE_UNSUPPORTED_SEARCH:
    case SIMANTICS_FAILURE_READ_ONLY_OWNER:
        return thread->failureDetail;
    case SIMANTICS_FAILURE_NO_SUCH_TREE:
        if (thread->failureDetail < SIMANTICS_FIRST_PRIVATE_OPCODE)
        {
            return CALL_SCOPE_GLOBAL;
        }
        return (thread->failureDetail < SIMANTICS_FIRST_SEMIGLOBAL_OPCODE) ? CALL_SCOPE_PRIVATE
                                                                         : CALL_SCOPE_SEMIGLOBAL;
    case SIMANTICS_FAILURE_NO_SUCH_CONSTANT:
        return (thread->failureDetail >> 13) & 0x03U;
    default:
        return 0U;
    }
}

static void tallyStop(SimanticsRehearsal *rehearsal, Unsigned32 treeIndex)
{
    SimanticsFailure failure = rehearsal->thread.failure;
    Unsigned32 detail = stopDetail(&rehearsal->thread);
    Unsigned32 which;

    for (which = 0U; which < rehearsal->stopCount; which++)
    {
        if (rehearsal->stops[which].failure == failure && rehearsal->stops[which].detail == detail)
        {
            rehearsal->stops[which].count++;
            return;
        }
    }
    if (rehearsal->stopCount >= SIMANTICS_REHEARSAL_STOP_LIMIT)
    {
        rehearsal->stopsUntallied++;
        return;
    }
    rehearsal->stops[rehearsal->stopCount].failure = failure;
    rehearsal->stops[rehearsal->stopCount].detail = detail;
    rehearsal->stops[rehearsal->stopCount].count = 1U;
    rehearsal->stops[rehearsal->stopCount].exampleTree = treeIndex;
    rehearsal->stopCount++;
}

static void resetWorld(SimanticsRehearsal *rehearsal)
{
    Unsigned32 which;

    rehearsal->machine.objectCount = 0U;
    rehearsal->machine.randomState = REHEARSAL_RANDOM_SEED;
    for (which = 0U; which < SIMANTICS_GLOBAL_COUNT; which++)
    {
        rehearsal->machine.globals[which] = 0;
    }
    simanticsMachineAddObject(&rehearsal->machine, 0U, 0U)->isPerson = BOOLEAN_TRUE;
    simanticsMachineAddObject(&rehearsal->machine, 0U, 0U)->isPerson = BOOLEAN_TRUE;
}

static SimanticsRehearsalStatus runOneTree(SimanticsRehearsal *rehearsal)
{
    const SimanticsRehearsalTree *candidate;
    SimanticsThreadStatus status = SIMANTICS_THREAD_RUNNING;
    Unsigned32 ticks;

    if (rehearsal->runCursor >= rehearsal->treeCount)
    {
        rehearsal->stage = SIMANTICS_REHEARSAL_STAGE_DONE;
        return SIMANTICS_REHEARSAL_COMPLETE;
    }
    candidate = &rehearsal->trees[rehearsal->runCursor];
    resetWorld(rehearsal);
    if (!simanticsThreadStart(&rehearsal->machine, &rehearsal->thread, 1, &candidate->tree, NULL_POINTER, 0U))
    {
        rehearsal->stopped++;
        rehearsal->runCursor++;
        return SIMANTICS_REHEARSAL_WORKING;
    }
    rehearsal->thread.stackObjectIdentifier = 2;
    for (ticks = 0U; ticks < SIMANTICS_REHEARSAL_TICK_LIMIT; ticks++)
    {
        status = simanticsThreadTick(&rehearsal->machine, &rehearsal->thread);
        if (status != SIMANTICS_THREAD_RUNNING && status != SIMANTICS_THREAD_SLEEPING)
        {
            break;
        }
    }
    switch (status)
    {
    case SIMANTICS_THREAD_RETURNED_TRUE:
        rehearsal->returnedTrue++;
        break;
    case SIMANTICS_THREAD_RETURNED_FALSE:
        rehearsal->returnedFalse++;
        break;
    case SIMANTICS_THREAD_FAILED:
        rehearsal->stopped++;
        tallyStop(rehearsal, rehearsal->runCursor);
        break;
    default:
        rehearsal->stillWaiting++;
        break;
    }
    rehearsal->runCursor++;
    return SIMANTICS_REHEARSAL_WORKING;
}

SimanticsRehearsalStatus simanticsRehearsalStep(SimanticsRehearsal *rehearsal)
{
    switch (rehearsal->stage)
    {
    case SIMANTICS_REHEARSAL_STAGE_HEADER:
        return readHeader(rehearsal);
    case SIMANTICS_REHEARSAL_STAGE_INDEX:
        return readIndex(rehearsal);
    case SIMANTICS_REHEARSAL_STAGE_LOAD:
        return loadResources(rehearsal);
    case SIMANTICS_REHEARSAL_STAGE_RUN:
        return runOneTree(rehearsal);
    default:
        return SIMANTICS_REHEARSAL_COMPLETE;
    }
}

static void appendNumber(char *line, Unsigned64 value)
{
    MemorySize used = stringLength(line);

    (void)stringWriteUnsigned(line + used, LINE_CAPACITY - used, value);
}

static void appendHex(char *line, Unsigned64 value, MemorySize digits)
{
    MemorySize used = stringLength(line);

    (void)stringWriteHexadecimal(line + used, LINE_CAPACITY - used, value, digits);
}

static void describeStop(char *line, const SimanticsRehearsalStop *stop)
{
    static const char *const callScopes[] = { "a global tree missing from the disc",
                                              "a private tree, which needs a real object's own trees",
                                              "a semi-global tree, which needs a real object's semi-global group" };
    static const char *const constantScopes[] = { "a private constant, which needs a real object's own tables",
                                                  "a semi-global constant, which needs a real object's semi-global group",
                                                  "a global constant missing from the disc",
                                                  "a constant in an unknown scope" };

    switch (stop->failure)
    {
    case SIMANTICS_FAILURE_UNSUPPORTED_PRIMITIVE:
        stringAppend(line, LINE_CAPACITY, "primitive ");
        appendHex(line, stop->detail, 2UL);
        stringAppend(line, LINE_CAPACITY, " ");
        stringAppend(line, LINE_CAPACITY, simanticsPrimitiveGetName(stop->detail));
        return;
    case SIMANTICS_FAILURE_UNSUPPORTED_SEARCH:
        stringAppend(line, LINE_CAPACITY, "Set To Next search ");
        appendHex(line, stop->detail, 2UL);
        stringAppend(line, LINE_CAPACITY, " ");
        stringAppend(line, LINE_CAPACITY, simanticsSearchGetName(stop->detail));
        return;
    case SIMANTICS_FAILURE_UNSUPPORTED_OWNER:
    case SIMANTICS_FAILURE_READ_ONLY_OWNER:
        stringAppend(line, LINE_CAPACITY,
                     (stop->failure == SIMANTICS_FAILURE_UNSUPPORTED_OWNER) ? "data owner " : "a write to data owner ");
        appendHex(line, stop->detail, 2UL);
        stringAppend(line, LINE_CAPACITY, " ");
        stringAppend(line, LINE_CAPACITY, simanticsOwnerGetName(stop->detail));
        return;
    case SIMANTICS_FAILURE_NO_SUCH_TREE:
        stringAppend(line, LINE_CAPACITY, "a call to ");
        stringAppend(line, LINE_CAPACITY, callScopes[stop->detail % VICTORIA_ARRAY_LENGTH(callScopes)]);
        return;
    case SIMANTICS_FAILURE_NO_SUCH_CONSTANT:
        stringAppend(line, LINE_CAPACITY, constantScopes[stop->detail % VICTORIA_ARRAY_LENGTH(constantScopes)]);
        return;
    default:
        stringAppend(line, LINE_CAPACITY, simanticsFailureGetName(stop->failure));
        return;
    }
}

void simanticsRehearsalReport(const SimanticsRehearsal *rehearsal, SimanticsRehearsalLog log)
{
    char line[LINE_CAPACITY];
    Boolean shown[SIMANTICS_REHEARSAL_STOP_LIMIT];
    Unsigned32 which;
    Unsigned32 rank;

    line[0] = '\0';
    stringAppend(line, sizeof(line), "rehearsal: ");
    appendNumber(line, rehearsal->treeCount);
    stringAppend(line, sizeof(line), " global tree(s) and ");
    appendNumber(line, rehearsal->tableCount);
    stringAppend(line, sizeof(line), " global constant table(s) loaded from ");
    appendNumber(line, rehearsal->packagesIndexed);
    stringAppend(line, sizeof(line), " package(s), ");
    appendNumber(line, rehearsal->treesReplaced);
    stringAppend(line, sizeof(line), " of them replacing a copy loaded earlier; ");
    appendNumber(line, rehearsal->treesRefused);
    stringAppend(line, sizeof(line), " tree(s) and ");
    appendNumber(line, rehearsal->tablesRefused);
    stringAppend(line, sizeof(line), " table(s) refused");
    if (rehearsal->resourcesDropped > 0U)
    {
        stringAppend(line, sizeof(line), ", ");
        appendNumber(line, rehearsal->resourcesDropped);
        stringAppend(line, sizeof(line), " past the list's room");
    }
    log(line);

    line[0] = '\0';
    stringAppend(line, sizeof(line), "rehearsal: ran each once on a blank Sim facing another, ");
    appendNumber(line, rehearsal->instructionsRun);
    stringAppend(line, sizeof(line), " instruction(s) in all — ");
    appendNumber(line, rehearsal->returnedTrue);
    stringAppend(line, sizeof(line), " returned true, ");
    appendNumber(line, rehearsal->returnedFalse);
    stringAppend(line, sizeof(line), " returned false, ");
    appendNumber(line, rehearsal->stillWaiting);
    stringAppend(line, sizeof(line), " still waiting after ");
    appendNumber(line, SIMANTICS_REHEARSAL_TICK_LIMIT);
    stringAppend(line, sizeof(line), " ticks, ");
    appendNumber(line, rehearsal->stopped);
    stringAppend(line, sizeof(line), " stopped");
    log(line);

    if (rehearsal->stopCount > 0U)
    {
        log("rehearsal: what stopped them, most common first:");
    }
    for (which = 0U; which < SIMANTICS_REHEARSAL_STOP_LIMIT; which++)
    {
        shown[which] = BOOLEAN_FALSE;
    }
    for (rank = 0U; rank < rehearsal->stopCount; rank++)
    {
        Unsigned32 best = rehearsal->stopCount;
        const SimanticsRehearsalStop *stop;
        const SimanticsTree *example;

        for (which = 0U; which < rehearsal->stopCount; which++)
        {
            if (!shown[which] && (best == rehearsal->stopCount ||
                                  rehearsal->stops[which].count > rehearsal->stops[best].count))
            {
                best = which;
            }
        }
        shown[best] = BOOLEAN_TRUE;
        stop = &rehearsal->stops[best];
        example = &rehearsal->trees[stop->exampleTree].tree;

        line[0] = '\0';
        stringAppend(line, sizeof(line), "rehearsal:   x");
        appendNumber(line, stop->count);
        stringAppend(line, sizeof(line), "  ");
        describeStop(line, stop);
        stringAppend(line, sizeof(line), " — e.g. ");
        appendHex(line, rehearsal->trees[stop->exampleTree].instance, 4UL);
        stringAppend(line, sizeof(line), " \"");
        stringAppend(line, sizeof(line), example->name);
        stringAppend(line, sizeof(line), "\"");
        log(line);
    }
    if (rehearsal->stopsUntallied > 0U)
    {
        line[0] = '\0';
        stringAppend(line, sizeof(line), "rehearsal:   and ");
        appendNumber(line, rehearsal->stopsUntallied);
        stringAppend(line, sizeof(line), " stop(s) of kinds past the table's room");
        log(line);
    }
    log("rehearsal: done");
}
