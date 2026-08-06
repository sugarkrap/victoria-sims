#ifndef VICTORIA_RESOURCE_CACHE_HEADER
#define VICTORIA_RESOURCE_CACHE_HEADER

#include "victoria/coreTypes.h"
#include "victoria/memoryArena.h"
#include "victoria/packageReader.h"

/* Somewhere for a resource to live whose lifetime is "until something replaces
 * it".
 *
 * The arena is a bump pointer and freeing means rewinding to a marker, so every
 * lifetime it can express is scoped: allocated in some order, released in the
 * reverse. That is right for a load, which happens once and in an order. It is
 * wrong for a resource the player changes their mind about — an animation
 * chosen from a menu is allocated after everything else and released before
 * it, which is exactly the shape an arena cannot do. Loading a second one
 * simply grows the arena, and a Sim switched between animations a few hundred
 * times reaches the ceiling and stops.
 *
 * That is not a debug-menu problem. It is what a game does all the time.
 *
 * So: a fixed number of slots of a fixed size, carved out of the arena once and
 * never grown. Nothing here allocates — it hands out storage that already
 * exists, which is the same rule as everywhere else, with room to say "I am
 * done with this" that the arena has no way to express.
 *
 * It knows nothing about where bytes come from. The caller reads a resource
 * however it reads resources — a file descriptor natively, a range the browser
 * delivers later on the web — and admits the bytes. That is what makes it
 * agnostic of the store rather than agnostic in principle: there is no path
 * through this file that could read anything even if it wanted to.
 *
 * Fixed slot size is a deliberate simplification and not an oversight. Variable
 * sizes mean either fragmentation or compaction, and both mean an allocator
 * this project has spent its whole life not having. What does not fit is not
 * cached — it is read the way it always was, and the count of those is
 * reported, because a cache that silently declines half its callers looks
 * exactly like one that is working. */

typedef struct ResourceCacheSlot
{
    PackageResourceKey key;
    Unsigned8 *bytes;
    MemorySize byteCount;
    /* When this slot was last handed out. A counter and not a clock: the engine
       has a clock but nothing here needs a time, only an order. */
    Unsigned64 lastUsed;
    Boolean occupied;
    /* In use right now and not to be evicted under its user's feet. A pinned
       slot is the difference between a cache and a way to free memory that is
       still being read. */
    Boolean pinned;
} ResourceCacheSlot;

typedef struct ResourceCache
{
    ResourceCacheSlot *slots;
    Unsigned32 slotCount;
    MemorySize slotCapacity;
    Unsigned64 clock;

    /* Everything that happens, counted. A cache is invisible when it works and
       invisible when it does not, so the only way to tell the two apart is to
       make it say. */
    Unsigned32 hits;
    Unsigned32 misses;
    Unsigned32 admissions;
    Unsigned32 evictions;
    /* Resources larger than a slot. Not cached, not an error — read the way
       they always were. */
    Unsigned32 refusedTooLarge;
    /* Admissions with every slot pinned. Also not an error, and also worth
       knowing: it means more is held at once than there are slots. */
    Unsigned32 refusedAllPinned;
} ResourceCache;

/* Carves the slots out of the arena. Returns false when they will not fit, and
   a cache that did not fit still answers every call — as a miss, every time.
   Nothing has to check whether the cache exists. */
Boolean resourceCacheBegin(ResourceCache *cache, MemoryArena *arena, Unsigned32 slotCount,
                           MemorySize slotCapacity);

/* The bytes for this key, or null. A hit pins the slot: the caller is about to
   read it, and it must not be evicted while they do. */
const Unsigned8 *resourceCacheFind(ResourceCache *cache, const PackageResourceKey *key,
                                   MemorySize *byteCount);

/* Copies bytes in, evicting the least recently used unpinned slot if need be.
   Returns the cached copy — which is what the caller should go on to use, so
   that a hit and a miss hand back the same kind of pointer — or null when it
   would not fit or every slot is pinned. Admitted slots come back pinned, for
   the same reason a hit does. */
const Unsigned8 *resourceCacheAdmit(ResourceCache *cache, const PackageResourceKey *key,
                                    const Unsigned8 *bytes, MemorySize byteCount);

/* Done with it. Everything pinned and never released fills the cache with
   things nobody is reading, which is the one way to make it useless. */
void resourceCacheRelease(ResourceCache *cache, const PackageResourceKey *key);
void resourceCacheReleaseEverything(ResourceCache *cache);

/* How many slots are holding something, and how many of those are pinned. */
Unsigned32 resourceCacheGetOccupied(const ResourceCache *cache);
Unsigned32 resourceCacheGetPinned(const ResourceCache *cache);

#endif
