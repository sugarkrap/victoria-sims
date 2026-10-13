#ifndef VICTORIA_SIMANTICS_REHEARSAL_HEADER
#define VICTORIA_SIMANTICS_REHEARSAL_HEADER

#include "victoria/coreTypes.h"
#include "victoria/memoryArena.h"
#include "victoria/simanticsMachine.h"
#include "victoria/simanticsTree.h"
#include "victoria/virtualFileSystem.h"

#define SIMANTICS_REHEARSAL_RESOURCE_LIMIT 8192U
#define SIMANTICS_REHEARSAL_TREE_LIMIT 4096U
#define SIMANTICS_REHEARSAL_TABLE_LIMIT 2048U
#define SIMANTICS_REHEARSAL_STOP_LIMIT 256U
#define SIMANTICS_REHEARSAL_TICK_LIMIT 32U

#define SIMANTICS_REHEARSAL_TREE_TYPE 0x42484156UL
#define SIMANTICS_REHEARSAL_TABLE_TYPE 0x42434F4EUL

typedef enum SimanticsRehearsalStatus
{
    SIMANTICS_REHEARSAL_COMPLETE = 0,
    SIMANTICS_REHEARSAL_WORKING
} SimanticsRehearsalStatus;

typedef enum SimanticsRehearsalStage
{
    SIMANTICS_REHEARSAL_STAGE_HEADER = 0,
    SIMANTICS_REHEARSAL_STAGE_INDEX,
    SIMANTICS_REHEARSAL_STAGE_LOAD,
    SIMANTICS_REHEARSAL_STAGE_RUN,
    SIMANTICS_REHEARSAL_STAGE_DONE
} SimanticsRehearsalStage;

typedef struct SimanticsRehearsalResource
{
    Unsigned32 typeIdentifier;
    Unsigned32 fileIndex;
    Unsigned16 instance;
    Unsigned32 offsetInBytes;
    Unsigned32 sizeInBytes;
} SimanticsRehearsalResource;

typedef struct SimanticsRehearsalTree
{
    Unsigned16 instance;
    SimanticsTree tree;
} SimanticsRehearsalTree;

typedef struct SimanticsRehearsalTable
{
    Unsigned16 instance;
    Unsigned16 count;
    const Integer16 *values;
} SimanticsRehearsalTable;

typedef struct SimanticsRehearsalStop
{
    SimanticsFailure failure;
    Unsigned32 detail;
    Unsigned32 count;
    Unsigned32 exampleTree;
} SimanticsRehearsalStop;

typedef struct SimanticsRehearsal
{
    VirtualFileSystem *fileSystem;
    MemoryArena *keep;
    MemoryArena *scratch;
    SimanticsRehearsalStage stage;

    Unsigned32 nextFileIndex;
    Unsigned32 pendingEntryCount;
    Unsigned32 pendingIndexOffset;
    Unsigned32 pendingIndexSize;
    Unsigned32 packagesIndexed;
    Unsigned32 packagesRefused;

    SimanticsRehearsalResource *resources;
    Unsigned32 resourceCount;
    Unsigned32 resourcesDropped;
    Unsigned32 loadCursor;

    SimanticsRehearsalTree *trees;
    Unsigned32 treeCount;
    Unsigned32 treesRefused;
    Unsigned32 treesReplaced;
    SimanticsRehearsalTable *tables;
    Unsigned32 tableCount;
    Unsigned32 tablesRefused;

    SimanticsMachine machine;
    SimanticsThread thread;
    Unsigned32 runCursor;
    Unsigned32 instructionsRun;
    Unsigned32 returnedTrue;
    Unsigned32 returnedFalse;
    Unsigned32 stillWaiting;
    Unsigned32 stopped;
    SimanticsRehearsalStop stops[SIMANTICS_REHEARSAL_STOP_LIMIT];
    Unsigned32 stopCount;
    Unsigned32 stopsUntallied;
} SimanticsRehearsal;

Boolean simanticsRehearsalBegin(SimanticsRehearsal *rehearsal, VirtualFileSystem *fileSystem, MemoryArena *keep,
                                MemoryArena *scratch);

SimanticsRehearsalStatus simanticsRehearsalStep(SimanticsRehearsal *rehearsal);

typedef void (*SimanticsRehearsalLog)(const char *line);

void simanticsRehearsalReport(const SimanticsRehearsal *rehearsal, SimanticsRehearsalLog log);

#endif
