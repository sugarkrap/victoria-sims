#include "victoria/resourceCache.h"

static Boolean keysMatch(const PackageResourceKey *first, const PackageResourceKey *second)
{
    return (first->typeIdentifier == second->typeIdentifier &&
            first->groupIdentifier == second->groupIdentifier &&
            first->instanceIdentifier == second->instanceIdentifier &&
            first->instanceIdentifierHigh == second->instanceIdentifierHigh)
               ? BOOLEAN_TRUE
               : BOOLEAN_FALSE;
}

Boolean resourceCacheBegin(ResourceCache *cache, MemoryArena *arena, Unsigned32 slotCount,
                           MemorySize slotCapacity)
{
    Unsigned32 index;

    cache->slots = NULL_POINTER;
    cache->slotCount = 0U;
    cache->slotCapacity = 0UL;
    cache->clock = 0ULL;
    cache->hits = 0U;
    cache->misses = 0U;
    cache->admissions = 0U;
    cache->evictions = 0U;
    cache->refusedTooLarge = 0U;
    cache->refusedAllPinned = 0U;

    if (arena == NULL_POINTER || slotCount == 0U || slotCapacity == 0UL)
    {
        return BOOLEAN_FALSE;
    }

    cache->slots = (ResourceCacheSlot *)memoryArenaAllocate(
        arena, (MemorySize)slotCount * sizeof(ResourceCacheSlot), sizeof(MemorySize));
    if (cache->slots == NULL_POINTER)
    {
        return BOOLEAN_FALSE;
    }
    for (index = 0U; index < slotCount; index++)
    {
        Unsigned8 *storage = (Unsigned8 *)memoryArenaAllocate(arena, slotCapacity, 16UL);

        if (storage == NULL_POINTER)
        {
            break;
        }
        cache->slots[index].bytes = storage;
        cache->slots[index].byteCount = 0UL;
        cache->slots[index].lastUsed = 0ULL;
        cache->slots[index].occupied = BOOLEAN_FALSE;
        cache->slots[index].pinned = BOOLEAN_FALSE;
        cache->slots[index].key.typeIdentifier = 0U;
        cache->slots[index].key.groupIdentifier = 0U;
        cache->slots[index].key.instanceIdentifier = 0U;
        cache->slots[index].key.instanceIdentifierHigh = 0U;
        cache->slotCount = index + 1U;
    }
    if (cache->slotCount == 0U)
    {
        cache->slots = NULL_POINTER;
        return BOOLEAN_FALSE;
    }
    cache->slotCapacity = slotCapacity;
    return BOOLEAN_TRUE;
}

const Unsigned8 *resourceCacheFind(ResourceCache *cache, const PackageResourceKey *key,
                                   MemorySize *byteCount)
{
    Unsigned32 index;

    if (byteCount != NULL_POINTER)
    {
        *byteCount = 0UL;
    }
    if (cache->slots == NULL_POINTER || key == NULL_POINTER)
    {
        cache->misses++;
        return NULL_POINTER;
    }
    for (index = 0U; index < cache->slotCount; index++)
    {
        ResourceCacheSlot *slot = &cache->slots[index];

        if (!slot->occupied || !keysMatch(&slot->key, key))
        {
            continue;
        }
        cache->clock++;
        slot->lastUsed = cache->clock;
        slot->pinned = BOOLEAN_TRUE;
        cache->hits++;
        if (byteCount != NULL_POINTER)
        {
            *byteCount = slot->byteCount;
        }
        return slot->bytes;
    }
    cache->misses++;
    return NULL_POINTER;
}

static ResourceCacheSlot *slotToUse(ResourceCache *cache, const PackageResourceKey *key)
{
    ResourceCacheSlot *oldest = NULL_POINTER;
    Unsigned32 index;

    for (index = 0U; index < cache->slotCount; index++)
    {
        if (cache->slots[index].occupied && keysMatch(&cache->slots[index].key, key))
        {
            return &cache->slots[index];
        }
    }
    for (index = 0U; index < cache->slotCount; index++)
    {
        if (!cache->slots[index].occupied)
        {
            return &cache->slots[index];
        }
    }
    for (index = 0U; index < cache->slotCount; index++)
    {
        ResourceCacheSlot *slot = &cache->slots[index];

        if (slot->pinned)
        {
            continue;
        }
        if (oldest == NULL_POINTER || slot->lastUsed < oldest->lastUsed)
        {
            oldest = slot;
        }
    }
    return oldest;
}

const Unsigned8 *resourceCacheAdmit(ResourceCache *cache, const PackageResourceKey *key,
                                    const Unsigned8 *bytes, MemorySize byteCount)
{
    ResourceCacheSlot *slot;
    MemorySize at;

    if (cache->slots == NULL_POINTER || key == NULL_POINTER || bytes == NULL_POINTER ||
        byteCount == 0UL)
    {
        return NULL_POINTER;
    }
    if (byteCount > cache->slotCapacity)
    {
        cache->refusedTooLarge++;
        return NULL_POINTER;
    }
    slot = slotToUse(cache, key);
    if (slot == NULL_POINTER)
    {
        cache->refusedAllPinned++;
        return NULL_POINTER;
    }
    if (slot->occupied && !keysMatch(&slot->key, key))
    {
        cache->evictions++;
    }
    for (at = 0UL; at < byteCount; at++)
    {
        slot->bytes[at] = bytes[at];
    }
    slot->byteCount = byteCount;
    slot->key = *key;
    slot->occupied = BOOLEAN_TRUE;
    slot->pinned = BOOLEAN_TRUE;
    cache->clock++;
    slot->lastUsed = cache->clock;
    cache->admissions++;
    return slot->bytes;
}

void resourceCacheRelease(ResourceCache *cache, const PackageResourceKey *key)
{
    Unsigned32 index;

    if (cache->slots == NULL_POINTER || key == NULL_POINTER)
    {
        return;
    }
    for (index = 0U; index < cache->slotCount; index++)
    {
        if (cache->slots[index].occupied && keysMatch(&cache->slots[index].key, key))
        {
            cache->slots[index].pinned = BOOLEAN_FALSE;
            return;
        }
    }
}

void resourceCacheReleaseEverything(ResourceCache *cache)
{
    Unsigned32 index;

    for (index = 0U; index < cache->slotCount; index++)
    {
        cache->slots[index].pinned = BOOLEAN_FALSE;
    }
}

Unsigned32 resourceCacheGetOccupied(const ResourceCache *cache)
{
    Unsigned32 count = 0U;
    Unsigned32 index;

    for (index = 0U; index < cache->slotCount; index++)
    {
        if (cache->slots[index].occupied)
        {
            count++;
        }
    }
    return count;
}

Unsigned32 resourceCacheGetPinned(const ResourceCache *cache)
{
    Unsigned32 count = 0U;
    Unsigned32 index;

    for (index = 0U; index < cache->slotCount; index++)
    {
        if (cache->slots[index].pinned)
        {
            count++;
        }
    }
    return count;
}
