#include "victoria/resourceIndex.h"

#include "utils/resourceHash.h"
#include "utils/strings.h"

#define HEADER_SIZE 96UL
#define OFFSET_INDEX_ENTRY_COUNT 36UL
#define OFFSET_INDEX_OFFSET 40UL
#define OFFSET_INDEX_SIZE 44UL

#define ENTRY_SIZE_WITHOUT_INSTANCE_HIGH 20UL
#define ENTRY_SIZE_WITH_INSTANCE_HIGH 24UL

#define LARGEST_INDEX_BYTES (8UL * 1024UL * 1024UL)

const char *resourceIndexStatusGetName(ResourceIndexStatus status)
{
    switch (status)
    {
    case RESOURCE_INDEX_COMPLETE:
        return "complete";
    case RESOURCE_INDEX_WORKING:
        return "still reading";
    case RESOURCE_INDEX_OUT_OF_ROOM:
        return "not enough room to index this disc";
    default:
        return "unknown";
    }
}

static Unsigned32 readUnsigned32(const Unsigned8 *bytes, MemorySize offset)
{
    return (Unsigned32)bytes[offset] | ((Unsigned32)bytes[offset + 1UL] << 8) |
           ((Unsigned32)bytes[offset + 2UL] << 16) | ((Unsigned32)bytes[offset + 3UL] << 24);
}

static Boolean endsWithPackage(const char *path)
{
    return stringEndsWithIgnoringCase(path, ".package");
}

static void recordInCensus(ResourceIndex *index, Unsigned32 typeIdentifier)
{
    Unsigned32 which;

    for (which = 0U; which < index->censusCount; which++)
    {
        if (index->censusTypes[which] == typeIdentifier)
        {
            index->censusCounts[which]++;
            return;
        }
    }
    if (index->censusCount >= RESOURCE_INDEX_CENSUS_LIMIT)
    {
        index->censusOverflow++;
        return;
    }
    index->censusTypes[index->censusCount] = typeIdentifier;
    index->censusCounts[index->censusCount] = 1U;
    index->censusCount++;
}

static Unsigned32 wantedSlot(const ResourceIndex *index, Unsigned32 typeIdentifier)
{
    Unsigned32 which;

    for (which = 0U; which < index->wantedTypeCount; which++)
    {
        if (index->wantedTypes[which] == typeIdentifier)
        {
            return which;
        }
    }
    return index->wantedTypeCount;
}

Boolean resourceIndexBegin(ResourceIndex *index, VirtualFileSystem *fileSystem, MemoryArena *arena,
                           Unsigned32 entryCapacity, const Unsigned32 *wantedTypes,
                           Unsigned32 wantedTypeCount)
{
    Unsigned32 which;

    index->fileSystem = fileSystem;
    index->arena = arena;
    index->capacity = entryCapacity;
    index->count = 0U;
    index->dropped = 0U;
    index->nextFileIndex = 0U;
    index->readingHeader = BOOLEAN_TRUE;
    index->filesIndexed = 0U;
    index->filesRefused = 0U;
    index->pendingEntryCount = 0U;
    index->pendingIndexOffset = 0U;
    index->pendingIndexSize = 0U;
    index->entriesSeen = 0U;
    index->censusCount = 0U;
    index->censusOverflow = 0U;
    index->wantedTypeCount =
        (wantedTypeCount > RESOURCE_INDEX_TYPE_LIMIT) ? RESOURCE_INDEX_TYPE_LIMIT : wantedTypeCount;
    index->wantedTypesRefused = wantedTypeCount - index->wantedTypeCount;
    for (which = 0U; which < index->wantedTypeCount; which++)
    {
        index->wantedTypes[which] = wantedTypes[which];
        index->countByType[which] = 0U;
    }

    index->entries = (ResourceIndexEntry *)memoryArenaAllocate(
        arena, (MemorySize)entryCapacity * sizeof(ResourceIndexEntry), sizeof(Unsigned32));
    return (index->entries != NULL_POINTER) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

ResourceIndexStatus resourceIndexStep(ResourceIndex *index)
{
    const VirtualFileEntry *entry;
    MemorySize marker;

    if (index->nextFileIndex >= index->fileSystem->entryCount)
    {
        return RESOURCE_INDEX_COMPLETE;
    }

    entry = virtualFileSystemGetEntry(index->fileSystem, index->nextFileIndex);
    if (entry == NULL_POINTER || !endsWithPackage(entry->path) ||
        entry->sizeInBytes < (Unsigned64)HEADER_SIZE)
    {
        index->nextFileIndex++;
        index->readingHeader = BOOLEAN_TRUE;
        return RESOURCE_INDEX_WORKING;
    }

    marker = memoryArenaGetMarker(index->arena);

    if (index->readingHeader)
    {
        Unsigned8 header[HEADER_SIZE];
        VirtualReadResult read =
            virtualFileSystemReadFile(index->fileSystem, index->nextFileIndex, 0U, HEADER_SIZE, header);
        MemorySize indexEnd;

        if (read == VIRTUAL_READ_PENDING)
        {
            return RESOURCE_INDEX_WORKING;
        }
        if (read != VIRTUAL_READ_OK || header[0] != 'D' || header[1] != 'B' || header[2] != 'P' ||
            header[3] != 'F')
        {
            index->filesRefused++;
            index->nextFileIndex++;
            return RESOURCE_INDEX_WORKING;
        }

        index->pendingEntryCount = readUnsigned32(header, OFFSET_INDEX_ENTRY_COUNT);
        index->pendingIndexOffset = readUnsigned32(header, OFFSET_INDEX_OFFSET);
        index->pendingIndexSize = readUnsigned32(header, OFFSET_INDEX_SIZE);

        indexEnd = (MemorySize)index->pendingIndexOffset + (MemorySize)index->pendingIndexSize;
        if (index->pendingEntryCount == 0U || index->pendingIndexSize == 0UL ||
            (MemorySize)index->pendingIndexSize > LARGEST_INDEX_BYTES ||
            indexEnd < (MemorySize)index->pendingIndexOffset ||
            (Unsigned64)indexEnd > entry->sizeInBytes)
        {
            index->filesRefused++;
            index->nextFileIndex++;
            return RESOURCE_INDEX_WORKING;
        }
        index->readingHeader = BOOLEAN_FALSE;
        return RESOURCE_INDEX_WORKING;
    }

    {
        MemorySize entrySize;
        Unsigned8 *indexBytes;
        VirtualReadResult read;
        Unsigned32 which;

        indexBytes =
            (Unsigned8 *)memoryArenaAllocate(index->arena, (MemorySize)index->pendingIndexSize, 4UL);
        if (indexBytes == NULL_POINTER)
        {
            memoryArenaRewindToMarker(index->arena, marker);
            return RESOURCE_INDEX_OUT_OF_ROOM;
        }
        read = virtualFileSystemReadFile(index->fileSystem, index->nextFileIndex,
                                         (Unsigned64)index->pendingIndexOffset,
                                         (MemorySize)index->pendingIndexSize, indexBytes);
        if (read == VIRTUAL_READ_PENDING)
        {
            memoryArenaRewindToMarker(index->arena, marker);
            return RESOURCE_INDEX_WORKING;
        }

        index->nextFileIndex++;
        index->readingHeader = BOOLEAN_TRUE;
        if (read != VIRTUAL_READ_OK)
        {
            index->filesRefused++;
            memoryArenaRewindToMarker(index->arena, marker);
            return RESOURCE_INDEX_WORKING;
        }

        entrySize = (MemorySize)index->pendingIndexSize / (MemorySize)index->pendingEntryCount;
        if ((entrySize != ENTRY_SIZE_WITHOUT_INSTANCE_HIGH &&
             entrySize != ENTRY_SIZE_WITH_INSTANCE_HIGH) ||
            entrySize * (MemorySize)index->pendingEntryCount != (MemorySize)index->pendingIndexSize)
        {
            index->filesRefused++;
            memoryArenaRewindToMarker(index->arena, marker);
            return RESOURCE_INDEX_WORKING;
        }

        for (which = 0U; which < index->pendingEntryCount; which++)
        {
            MemorySize at = (MemorySize)which * entrySize;
            Unsigned32 typeIdentifier = readUnsigned32(indexBytes, at);
            Unsigned32 slot = wantedSlot(index, typeIdentifier);
            ResourceIndexEntry *stored;

            index->entriesSeen++;
            recordInCensus(index, typeIdentifier);
            if (slot >= index->wantedTypeCount)
            {
                continue;
            }
            index->countByType[slot]++;
            if (index->count >= index->capacity)
            {
                index->dropped++;
                continue;
            }
            stored = &index->entries[index->count];
            stored->typeIdentifier = typeIdentifier;
            stored->groupIdentifier = readUnsigned32(indexBytes, at + 4UL);
            stored->instanceIdentifier = readUnsigned32(indexBytes, at + 8UL);
            if (entrySize == ENTRY_SIZE_WITH_INSTANCE_HIGH)
            {
                stored->instanceIdentifierHigh = readUnsigned32(indexBytes, at + 12UL);
                stored->offsetInBytes = readUnsigned32(indexBytes, at + 16UL);
                stored->sizeInBytes = readUnsigned32(indexBytes, at + 20UL);
            }
            else
            {
                stored->instanceIdentifierHigh = 0U;
                stored->offsetInBytes = readUnsigned32(indexBytes, at + 12UL);
                stored->sizeInBytes = readUnsigned32(indexBytes, at + 16UL);
            }
            stored->fileIndex = index->nextFileIndex - 1U;
            index->count++;
        }
        index->filesIndexed++;
        memoryArenaRewindToMarker(index->arena, marker);
        return RESOURCE_INDEX_WORKING;
    }
}

const ResourceIndexEntry *resourceIndexFind(const ResourceIndex *index, Unsigned32 typeIdentifier,
                                            Unsigned32 instanceIdentifier,
                                            Unsigned32 instanceIdentifierHigh)
{
    Unsigned32 which;

    for (which = 0U; which < index->count; which++)
    {
        const ResourceIndexEntry *candidate = &index->entries[which];

        if (candidate->typeIdentifier != typeIdentifier ||
            candidate->instanceIdentifier != instanceIdentifier)
        {
            continue;
        }
        if (candidate->instanceIdentifierHigh == instanceIdentifierHigh)
        {
            return candidate;
        }
    }
    return NULL_POINTER;
}

Boolean resourceIndexGetCensusRank(const ResourceIndex *index, Unsigned32 rank,
                                   Unsigned32 *typeIdentifier, Unsigned32 *count)
{
    Unsigned32 which;

    for (which = 0U; which < index->censusCount; which++)
    {
        Unsigned32 ahead = 0U;
        Unsigned32 other;

        for (other = 0U; other < index->censusCount; other++)
        {
            if (index->censusCounts[other] > index->censusCounts[which] ||
                (index->censusCounts[other] == index->censusCounts[which] && other < which))
            {
                ahead++;
            }
        }
        if (ahead == rank)
        {
            *typeIdentifier = index->censusTypes[which];
            *count = index->censusCounts[which];
            return BOOLEAN_TRUE;
        }
    }
    return BOOLEAN_FALSE;
}

const ResourceIndexEntry *resourceIndexFindInGroup(const ResourceIndex *index,
                                                   Unsigned32 typeIdentifier,
                                                   Unsigned32 groupIdentifier,
                                                   Unsigned32 instanceIdentifier,
                                                   Unsigned32 instanceIdentifierHigh)
{
    Unsigned32 which;

    for (which = 0U; which < index->count; which++)
    {
        const ResourceIndexEntry *entry = &index->entries[which];

        if (entry->typeIdentifier == typeIdentifier &&
            entry->groupIdentifier == groupIdentifier &&
            entry->instanceIdentifier == instanceIdentifier &&
            entry->instanceIdentifierHigh == instanceIdentifierHigh)
        {
            return entry;
        }
    }
    return NULL_POINTER;
}

const ResourceIndexEntry *resourceIndexFindNamed(const ResourceIndex *index,
                                                 Unsigned32 typeIdentifier, const char *name)
{
    return resourceIndexFind(index, typeIdentifier, resourceHashInstance(name),
                             resourceHashInstanceHigh(name));
}
