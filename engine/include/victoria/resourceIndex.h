#ifndef VICTORIA_RESOURCE_INDEX_HEADER
#define VICTORIA_RESOURCE_INDEX_HEADER

#include "victoria/coreTypes.h"
#include "victoria/memoryArena.h"
#include "victoria/packageReader.h"
#include "victoria/virtualFileSystem.h"

#define RESOURCE_INDEX_TYPE_LIMIT 16U

#define RESOURCE_INDEX_CENSUS_LIMIT 256U

typedef struct ResourceIndexEntry
{
    Unsigned32 typeIdentifier;
    Unsigned32 groupIdentifier;
    Unsigned32 instanceIdentifier;
    Unsigned32 instanceIdentifierHigh;
    Unsigned32 fileIndex;
    Unsigned32 offsetInBytes;
    Unsigned32 sizeInBytes;
} ResourceIndexEntry;

typedef enum ResourceIndexStatus
{
    RESOURCE_INDEX_COMPLETE = 0,
    RESOURCE_INDEX_WORKING,
    RESOURCE_INDEX_OUT_OF_ROOM
} ResourceIndexStatus;

const char *resourceIndexStatusGetName(ResourceIndexStatus status);

typedef struct ResourceIndex
{
    VirtualFileSystem *fileSystem;
    MemoryArena *arena;

    ResourceIndexEntry *entries;
    Unsigned32 capacity;
    Unsigned32 count;
    Unsigned32 dropped;

    Unsigned32 wantedTypes[RESOURCE_INDEX_TYPE_LIMIT];
    Unsigned32 wantedTypeCount;
    Unsigned32 wantedTypesRefused;
    Unsigned32 countByType[RESOURCE_INDEX_TYPE_LIMIT];
    Unsigned32 entriesSeen;

    Unsigned32 censusTypes[RESOURCE_INDEX_CENSUS_LIMIT];
    Unsigned32 censusCounts[RESOURCE_INDEX_CENSUS_LIMIT];
    Unsigned32 censusCount;
    Unsigned32 censusOverflow;

    Unsigned32 nextFileIndex;
    Boolean readingHeader;
    Unsigned32 filesIndexed;
    Unsigned32 filesRefused;

    Unsigned32 pendingEntryCount;
    Unsigned32 pendingIndexOffset;
    Unsigned32 pendingIndexSize;
} ResourceIndex;

Boolean resourceIndexBegin(ResourceIndex *index, VirtualFileSystem *fileSystem, MemoryArena *arena,
                           Unsigned32 entryCapacity, const Unsigned32 *wantedTypes,
                           Unsigned32 wantedTypeCount);

ResourceIndexStatus resourceIndexStep(ResourceIndex *index);

const ResourceIndexEntry *resourceIndexFind(const ResourceIndex *index, Unsigned32 typeIdentifier,
                                            Unsigned32 instanceIdentifier,
                                            Unsigned32 instanceIdentifierHigh);

const ResourceIndexEntry *resourceIndexFindInGroup(const ResourceIndex *index,
                                                   Unsigned32 typeIdentifier,
                                                   Unsigned32 groupIdentifier,
                                                   Unsigned32 instanceIdentifier,
                                                   Unsigned32 instanceIdentifierHigh);

const ResourceIndexEntry *resourceIndexFindNamed(const ResourceIndex *index,
                                                 Unsigned32 typeIdentifier, const char *name);

Boolean resourceIndexGetCensusRank(const ResourceIndex *index, Unsigned32 rank,
                                   Unsigned32 *typeIdentifier, Unsigned32 *count);

#endif
