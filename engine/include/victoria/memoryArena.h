#ifndef VICTORIA_MEMORY_ARENA_HEADER
#define VICTORIA_MEMORY_ARENA_HEADER

#include "victoria/coreTypes.h"

typedef struct MemoryArena
{
    Unsigned8 *baseAddress;
    MemorySize totalSizeInBytes;
    MemorySize usedSizeInBytes;
    MemorySize highWaterMarkInBytes;
} MemoryArena;

void memoryArenaInitialize(MemoryArena *arena, Unsigned8 *baseAddress, MemorySize totalSizeInBytes);

void *memoryArenaAllocate(MemoryArena *arena, MemorySize sizeInBytes, MemorySize alignmentInBytes);

MemorySize memoryArenaGetMarker(const MemoryArena *arena);
void memoryArenaRewindToMarker(MemoryArena *arena, MemorySize marker);
MemorySize memoryArenaGetRemainingBytes(const MemoryArena *arena);

#endif
