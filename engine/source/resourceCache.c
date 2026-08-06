#include "victoria/resourceCache.h"

static Boolean keysMatch(const PackageResourceKey *first, const PackageResourceKey *second)
{
    /* All four words. Two resources differing only in the high instance word
       are different resources, and a cache that ignored it would hand back the
       wrong one — which is worse than not caching at all, because it looks like
       the disc is wrong. */
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
        /* Each slot's storage taken separately, so a cache that runs out of
           arena part way keeps the slots it did get rather than failing whole.
           The count is trimmed to what was actually carved. */
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
        /* Pinned on the way out: the caller is about to read it, and a slot
           evicted while being read is a use-after-free with extra steps. */
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

/* The slot to write into: the one already holding this key, else a free one,
 * else the least recently used one nobody is holding. Null when every slot is
 * pinned.
 *
 * The key it already holds comes FIRST, and that ordering is the whole
 * correctness of it. Taking a free slot for a key that is already cached leaves
 * two slots claiming one resource, and a lookup scanning in slot order then
 * finds whichever came first — so the older copy wins every time and the newer
 * one is never seen again. A cache that answers with stale bytes is worse than
 * no cache: it does not look slow, it looks like the disc is wrong. */
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
        /* Read the way it always was. Counted, because a cache quietly
           declining half its callers looks exactly like one that is working. */
        cache->refusedTooLarge++;
        return NULL_POINTER;
    }
    slot = slotToUse(cache, key);
    if (slot == NULL_POINTER)
    {
        cache->refusedAllPinned++;
        return NULL_POINTER;
    }
    /* Replacing a slot's own key is a refresh and not an eviction: nothing was
       thrown away that anybody could still have wanted. */
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
            /* Unpinned, not emptied. The whole point is that the bytes are
               still there for the next caller who wants them. */
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
