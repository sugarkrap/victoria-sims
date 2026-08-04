#ifndef VICTORIA_MEMORY_ARENA_HEADER
#define VICTORIA_MEMORY_ARENA_HEADER

#include "victoria/coreTypes.h"

/* Every byte the engine will ever touch comes out of one of these. An arena is
   a bump pointer over storage that already exists; it never asks the operating
   system for more. Freeing happens by rewinding to a marker, so lifetimes are
   scoped rather than individually tracked. */
typedef struct MemoryArena
{
    Unsigned8 *baseAddress;
    MemorySize totalSizeInBytes;
    MemorySize usedSizeInBytes;
    MemorySize highWaterMarkInBytes;
} MemoryArena;

void memoryArenaInitialize(MemoryArena *arena, Unsigned8 *baseAddress, MemorySize totalSizeInBytes);

/* Returns NULL_POINTER when the request does not fit. Callers must check:
   there is no growth path and no fallback allocator. */
void *memoryArenaAllocate(MemoryArena *arena, MemorySize sizeInBytes, MemorySize alignmentInBytes);

MemorySize memoryArenaGetMarker(const MemoryArena *arena);
void memoryArenaRewindToMarker(MemoryArena *arena, MemorySize marker);
MemorySize memoryArenaGetRemainingBytes(const MemoryArena *arena);

#endif
