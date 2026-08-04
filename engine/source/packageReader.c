#include "victoria/packageReader.h"

#define HEADER_SIZE 96UL

#define OFFSET_MAJOR_VERSION 0x04UL
#define OFFSET_MINOR_VERSION 0x08UL
#define OFFSET_INDEX_ENTRY_COUNT 0x24UL
#define OFFSET_INDEX_OFFSET 0x28UL
#define OFFSET_INDEX_SIZE 0x2CUL

/* The two index entry layouts seen in the wild: with and without the second
 * instance word. */
#define ENTRY_SIZE_WITHOUT_INSTANCE_HIGH 20UL
#define ENTRY_SIZE_WITH_INSTANCE_HIGH 24UL

static Unsigned32 readUnsigned32(const Unsigned8 *bytes, MemorySize offset)
{
    return (Unsigned32)bytes[offset] | ((Unsigned32)bytes[offset + 1UL] << 8) |
           ((Unsigned32)bytes[offset + 2UL] << 16) | ((Unsigned32)bytes[offset + 3UL] << 24);
}

const char *packageReadResultGetName(PackageReadResult result)
{
    switch (result)
    {
    case PACKAGE_READ_OK:
        return "ok";
    case PACKAGE_READ_NOT_A_PACKAGE:
        return "not a DBPF package";
    case PACKAGE_READ_TRUNCATED:
        return "file ends before its own header or index";
    case PACKAGE_READ_BAD_INDEX:
        return "index is malformed";
    case PACKAGE_READ_OUT_OF_ARENA:
        return "not enough arena space for the index";
    default:
        return "unknown";
    }
}

PackageReadResult packageReaderOpen(Package *package, const Unsigned8 *bytes, MemorySize sizeInBytes,
                                    MemoryArena *arena)
{
    Unsigned32 entryCount;
    Unsigned32 indexOffset;
    Unsigned32 indexSize;
    MemorySize entrySize;
    MemorySize indexEnd;
    PackageResource *resources;
    Unsigned32 index;

    package->bytes = bytes;
    package->sizeInBytes = sizeInBytes;
    package->majorVersion = 0U;
    package->minorVersion = 0U;
    package->resourceCount = 0U;
    package->resources = NULL_POINTER;

    if (sizeInBytes < HEADER_SIZE)
    {
        return PACKAGE_READ_TRUNCATED;
    }
    if (bytes[0] != 'D' || bytes[1] != 'B' || bytes[2] != 'P' || bytes[3] != 'F')
    {
        return PACKAGE_READ_NOT_A_PACKAGE;
    }

    package->majorVersion = readUnsigned32(bytes, OFFSET_MAJOR_VERSION);
    package->minorVersion = readUnsigned32(bytes, OFFSET_MINOR_VERSION);

    entryCount = readUnsigned32(bytes, OFFSET_INDEX_ENTRY_COUNT);
    indexOffset = readUnsigned32(bytes, OFFSET_INDEX_OFFSET);
    indexSize = readUnsigned32(bytes, OFFSET_INDEX_SIZE);

    if (entryCount == 0U)
    {
        return PACKAGE_READ_OK;
    }

    indexEnd = (MemorySize)indexOffset + (MemorySize)indexSize;
    if (indexEnd < (MemorySize)indexOffset || indexEnd > sizeInBytes)
    {
        return PACKAGE_READ_TRUNCATED;
    }

    /* Entry size is derived from the index rather than from the version field.
     * Both layouts occur, and the division establishes which without having to
     * interpret the version semantics correctly — the version field has proved
     * less reliable than the arithmetic. */
    entrySize = (MemorySize)indexSize / (MemorySize)entryCount;
    if ((entrySize != ENTRY_SIZE_WITHOUT_INSTANCE_HIGH && entrySize != ENTRY_SIZE_WITH_INSTANCE_HIGH) ||
        entrySize * (MemorySize)entryCount != (MemorySize)indexSize)
    {
        return PACKAGE_READ_BAD_INDEX;
    }

    resources = (PackageResource *)memoryArenaAllocate(
        arena, sizeof(PackageResource) * (MemorySize)entryCount, 16UL);
    if (resources == NULL_POINTER)
    {
        return PACKAGE_READ_OUT_OF_ARENA;
    }

    for (index = 0U; index < entryCount; index += 1U)
    {
        MemorySize entryOffset = (MemorySize)indexOffset + ((MemorySize)index * entrySize);
        PackageResource *resource = &resources[index];

        resource->key.typeIdentifier = readUnsigned32(bytes, entryOffset);
        resource->key.groupIdentifier = readUnsigned32(bytes, entryOffset + 4UL);
        resource->key.instanceIdentifier = readUnsigned32(bytes, entryOffset + 8UL);

        if (entrySize == ENTRY_SIZE_WITH_INSTANCE_HIGH)
        {
            resource->key.instanceIdentifierHigh = readUnsigned32(bytes, entryOffset + 12UL);
            resource->offsetInBytes = readUnsigned32(bytes, entryOffset + 16UL);
            resource->sizeInBytes = readUnsigned32(bytes, entryOffset + 20UL);
        }
        else
        {
            resource->key.instanceIdentifierHigh = 0U;
            resource->offsetInBytes = readUnsigned32(bytes, entryOffset + 12UL);
            resource->sizeInBytes = readUnsigned32(bytes, entryOffset + 16UL);
        }

        /* A resource claiming to extend past the end of the file makes the
         * whole index untrustworthy, so reject rather than skip the entry: a
         * partially-believed index is worse than none. */
        {
            MemorySize resourceEnd =
                (MemorySize)resource->offsetInBytes + (MemorySize)resource->sizeInBytes;
            if (resourceEnd < (MemorySize)resource->offsetInBytes || resourceEnd > sizeInBytes)
            {
                return PACKAGE_READ_BAD_INDEX;
            }
        }
    }

    package->resourceCount = entryCount;
    package->resources = resources;
    return PACKAGE_READ_OK;
}

Unsigned32 packageReaderCountResourcesOfType(const Package *package, Unsigned32 typeIdentifier)
{
    Unsigned32 count = 0U;
    Unsigned32 index;

    for (index = 0U; index < package->resourceCount; index += 1U)
    {
        if (package->resources[index].key.typeIdentifier == typeIdentifier)
        {
            count += 1U;
        }
    }
    return count;
}

const PackageResource *packageReaderFindFirstOfType(const Package *package, Unsigned32 typeIdentifier)
{
    Unsigned32 index;

    for (index = 0U; index < package->resourceCount; index += 1U)
    {
        if (package->resources[index].key.typeIdentifier == typeIdentifier)
        {
            return &package->resources[index];
        }
    }
    return NULL_POINTER;
}

const Unsigned8 *packageReaderGetResourceBytes(const Package *package, const PackageResource *resource)
{
    MemorySize resourceEnd = (MemorySize)resource->offsetInBytes + (MemorySize)resource->sizeInBytes;

    if (resourceEnd > package->sizeInBytes)
    {
        return NULL_POINTER;
    }
    return package->bytes + resource->offsetInBytes;
}

Boolean packageReaderHasCompressedResources(const Package *package)
{
    return packageReaderFindFirstOfType(package, (Unsigned32)PACKAGE_TYPE_DIRECTORY) != NULL_POINTER
               ? BOOLEAN_TRUE
               : BOOLEAN_FALSE;
}
