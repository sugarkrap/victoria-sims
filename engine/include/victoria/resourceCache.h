#ifndef VICTORIA_RESOURCE_CACHE_HEADER
#define VICTORIA_RESOURCE_CACHE_HEADER

#include "victoria/coreTypes.h"
#include "victoria/memoryArena.h"
#include "victoria/packageReader.h"

typedef struct ResourceCacheSlot
{
    PackageResourceKey key;
    Unsigned8 *bytes;
    MemorySize byteCount;
    Unsigned64 lastUsed;
    Boolean occupied;
    Boolean pinned;
} ResourceCacheSlot;

typedef struct ResourceCache
{
    ResourceCacheSlot *slots;
    Unsigned32 slotCount;
    MemorySize slotCapacity;
    Unsigned64 clock;

    Unsigned32 hits;
    Unsigned32 misses;
    Unsigned32 admissions;
    Unsigned32 evictions;
    Unsigned32 refusedTooLarge;
    Unsigned32 refusedAllPinned;
} ResourceCache;

Boolean resourceCacheBegin(ResourceCache *cache, MemoryArena *arena, Unsigned32 slotCount,
                           MemorySize slotCapacity);

const Unsigned8 *resourceCacheFind(ResourceCache *cache, const PackageResourceKey *key,
                                   MemorySize *byteCount);

const Unsigned8 *resourceCacheAdmit(ResourceCache *cache, const PackageResourceKey *key,
                                    const Unsigned8 *bytes, MemorySize byteCount);

void resourceCacheRelease(ResourceCache *cache, const PackageResourceKey *key);
void resourceCacheReleaseEverything(ResourceCache *cache);

Unsigned32 resourceCacheGetOccupied(const ResourceCache *cache);
Unsigned32 resourceCacheGetPinned(const ResourceCache *cache);

#endif
