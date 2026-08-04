#include "victoria/memoryArena.h"

static MemorySize alignForward(MemorySize value, MemorySize alignmentInBytes)
{
    MemorySize remainder = value & (alignmentInBytes - 1UL);
    if (remainder == 0UL)
    {
        return value;
    }
    return value + (alignmentInBytes - remainder);
}

void memoryArenaInitialize(MemoryArena *arena, Unsigned8 *baseAddress, MemorySize totalSizeInBytes)
{
    arena->baseAddress = baseAddress;
    arena->totalSizeInBytes = totalSizeInBytes;
    arena->usedSizeInBytes = 0UL;
    arena->highWaterMarkInBytes = 0UL;
}

void *memoryArenaAllocate(MemoryArena *arena, MemorySize sizeInBytes, MemorySize alignmentInBytes)
{
    MemorySize alignedOffset;
    MemorySize nextOffset;

    if (alignmentInBytes == 0UL || (alignmentInBytes & (alignmentInBytes - 1UL)) != 0UL)
    {
        return NULL_POINTER;
    }

    alignedOffset = alignForward(arena->usedSizeInBytes, alignmentInBytes);
    nextOffset = alignedOffset + sizeInBytes;

    /* Ordered so that an overflowing sum cannot slip past the capacity test. */
    if (nextOffset < alignedOffset || nextOffset > arena->totalSizeInBytes)
    {
        return NULL_POINTER;
    }

    arena->usedSizeInBytes = nextOffset;
    if (nextOffset > arena->highWaterMarkInBytes)
    {
        arena->highWaterMarkInBytes = nextOffset;
    }

    return arena->baseAddress + alignedOffset;
}

MemorySize memoryArenaGetMarker(const MemoryArena *arena)
{
    return arena->usedSizeInBytes;
}

void memoryArenaRewindToMarker(MemoryArena *arena, MemorySize marker)
{
    if (marker <= arena->usedSizeInBytes)
    {
        arena->usedSizeInBytes = marker;
    }
}

MemorySize memoryArenaGetRemainingBytes(const MemoryArena *arena)
{
    return arena->totalSizeInBytes - arena->usedSizeInBytes;
}
