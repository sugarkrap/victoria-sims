/* Checks the resource cache: what it keeps, what it throws away, and what it
 * refuses.
 *
 * A cache is invisible when it works and invisible when it does not, which is
 * the whole reason this file is long. Every wrong version of it still draws a
 * Sim: one that never hits is merely slow, one that evicts a slot somebody is
 * reading crashes somewhere else entirely, and one that matches three key words
 * out of four hands back a different resource and makes the disc look wrong.
 *
 * So the cases are the ones where a plausible implementation differs from a
 * correct one. */

#include <stdio.h>

#include "utils/assert.h"
#include "victoria/resourceCache.h"

static Integer32 failureCount = 0;

#define ARENA_CAPACITY 65536UL
static Unsigned8 arenaStorage[ARENA_CAPACITY];

static PackageResourceKey keyOf(Unsigned32 type, Unsigned32 group, Unsigned32 instance,
                                Unsigned32 instanceHigh)
{
    PackageResourceKey key;

    key.typeIdentifier = type;
    key.groupIdentifier = group;
    key.instanceIdentifier = instance;
    key.instanceIdentifierHigh = instanceHigh;
    return key;
}

int main(void)
{
    MemoryArena arena;
    ResourceCache cache;
    static Unsigned8 payload[64];
    Unsigned32 index;

    for (index = 0U; index < sizeof(payload); index++)
    {
        payload[index] = (Unsigned8)index;
    }

    /* A cache that would not fit still answers every call, so nothing has to
       check whether it exists. */
    {
        MemoryArena tiny;
        static Unsigned8 tinyStorage[8];
        MemorySize got = 0UL;

        memoryArenaInitialize(&tiny, tinyStorage, sizeof(tinyStorage));
        checkThat(&failureCount, "a cache with no room refuses to begin",
                  !resourceCacheBegin(&cache, &tiny, 8U, 4096UL));
        checkThat(&failureCount, "and then answers as a miss rather than crashing",
                  resourceCacheFind(&cache, &(PackageResourceKey){ 1U, 2U, 3U, 4U }, &got) ==
                      NULL_POINTER);
        checkThat(&failureCount, "and admits nothing",
                  resourceCacheAdmit(&cache, &(PackageResourceKey){ 1U, 2U, 3U, 4U }, payload,
                                     8UL) == NULL_POINTER);
    }

    memoryArenaInitialize(&arena, arenaStorage, ARENA_CAPACITY);
    checkThat(&failureCount, "a cache of four slots begins",
              resourceCacheBegin(&cache, &arena, 4U, 64UL) && cache.slotCount == 4U);

    /* A miss, then an admission, then a hit. */
    {
        PackageResourceKey first = keyOf(0xAAU, 1U, 100U, 0U);
        MemorySize got = 0UL;

        checkThat(&failureCount, "an empty cache misses",
                  resourceCacheFind(&cache, &first, &got) == NULL_POINTER && got == 0UL);
        checkThat(&failureCount, "admitting hands back a pointer to the cached copy",
                  resourceCacheAdmit(&cache, &first, payload, 16UL) != NULL_POINTER);
        {
            const Unsigned8 *found = resourceCacheFind(&cache, &first, &got);

            checkThat(&failureCount, "and finding it hits with the right length",
                      found != NULL_POINTER && got == 16UL);
            checkThat(&failureCount, "with the bytes that went in",
                      found != NULL_POINTER && found[0] == 0U && found[15] == 15U);
            checkThat(&failureCount, "and the copy is not the caller's buffer",
                      found != payload);
        }
    }

    /* All four key words. Two resources differing only in the high instance
       word are different resources, and handing back the wrong one is worse
       than not caching — it makes the disc look wrong. */
    {
        PackageResourceKey nearly = keyOf(0xAAU, 1U, 100U, 999U);
        MemorySize got = 0UL;

        checkThat(&failureCount, "a key differing only in the high instance word is a miss",
                  resourceCacheFind(&cache, &nearly, &got) == NULL_POINTER);
        nearly = keyOf(0xAAU, 2U, 100U, 0U);
        checkThat(&failureCount, "and so is one differing only in the group",
                  resourceCacheFind(&cache, &nearly, &got) == NULL_POINTER);
        nearly = keyOf(0xABU, 1U, 100U, 0U);
        checkThat(&failureCount, "and one differing only in the type",
                  resourceCacheFind(&cache, &nearly, &got) == NULL_POINTER);
    }

    /* Too large to fit. Not an error — read the way it always was — but
       counted, because a cache declining half its callers silently looks
       exactly like one that is working. */
    {
        PackageResourceKey big = keyOf(0xBBU, 1U, 1U, 0U);

        checkThat(&failureCount, "a resource larger than a slot is not cached",
                  resourceCacheAdmit(&cache, &big, payload, 65UL) == NULL_POINTER);
        checkThat(&failureCount, "and is counted as too large rather than as a failure",
                  cache.refusedTooLarge == 1U);
    }

    /* Pinning. A slot handed out is being read, and evicting it under its
       reader is the one way this can corrupt something rather than be slow. */
    {
        PackageResourceKey keys[5];
        Unsigned32 which;

        /* A cache of its own, so the counts below are about this block rather
           than about everything that has happened so far. */
        memoryArenaInitialize(&arena, arenaStorage, ARENA_CAPACITY);
        (void)resourceCacheBegin(&cache, &arena, 4U, 64UL);
        for (which = 0U; which < 4U; which++)
        {
            keys[which] = keyOf(0xCCU, 1U, which, 0U);
            (void)resourceCacheAdmit(&cache, &keys[which], payload, 8UL);
        }
        checkThat(&failureCount, "four admissions fill four slots",
                  resourceCacheGetOccupied(&cache) == 4U);
        checkThat(&failureCount, "and every one of them is pinned",
                  resourceCacheGetPinned(&cache) == 4U);

        keys[4] = keyOf(0xCCU, 1U, 4U, 0U);
        checkThat(&failureCount, "a fifth cannot displace any of them while they are held",
                  resourceCacheAdmit(&cache, &keys[4], payload, 8UL) == NULL_POINTER &&
                      cache.refusedAllPinned == 1U);

        resourceCacheRelease(&cache, &keys[0]);
        checkThat(&failureCount, "releasing one does not empty it",
                  resourceCacheGetOccupied(&cache) == 4U &&
                      resourceCacheGetPinned(&cache) == 3U);
        {
            MemorySize got = 0UL;

            checkThat(&failureCount, "so it can still be found afterwards",
                      resourceCacheFind(&cache, &keys[0], &got) != NULL_POINTER);
            resourceCacheRelease(&cache, &keys[0]);
        }
        checkThat(&failureCount, "and now a fifth takes the freed slot",
                  resourceCacheAdmit(&cache, &keys[4], payload, 8UL) != NULL_POINTER &&
                      cache.evictions == 1U);
        {
            MemorySize got = 0UL;

            checkThat(&failureCount, "the one it displaced is gone",
                      resourceCacheFind(&cache, &keys[0], &got) == NULL_POINTER);
            checkThat(&failureCount, "and the ones that were held are not",
                      resourceCacheFind(&cache, &keys[1], &got) != NULL_POINTER);
        }
    }

    /* Least recently USED, not least recently admitted. A cache that evicted by
       age of admission would throw away the thing being read every frame in
       favour of something touched once. */
    {
        PackageResourceKey keys[5];
        Unsigned32 which;
        MemorySize got = 0UL;

        memoryArenaInitialize(&arena, arenaStorage, ARENA_CAPACITY);
        (void)resourceCacheBegin(&cache, &arena, 4U, 64UL);
        for (which = 0U; which < 4U; which++)
        {
            keys[which] = keyOf(0xDDU, 1U, which, 0U);
            (void)resourceCacheAdmit(&cache, &keys[which], payload, 8UL);
        }
        resourceCacheReleaseEverything(&cache);

        /* Touch the oldest, so it is no longer the least recently used. */
        (void)resourceCacheFind(&cache, &keys[0], &got);
        resourceCacheRelease(&cache, &keys[0]);

        keys[4] = keyOf(0xDDU, 1U, 4U, 0U);
        (void)resourceCacheAdmit(&cache, &keys[4], payload, 8UL);
        checkThat(&failureCount, "the slot touched most recently survives",
                  resourceCacheFind(&cache, &keys[0], &got) != NULL_POINTER);
        resourceCacheRelease(&cache, &keys[0]);
        checkThat(&failureCount, "and the one nobody has touched since is the one evicted",
                  resourceCacheFind(&cache, &keys[1], &got) == NULL_POINTER);
    }

    /* Re-admitting a key already held must not end up with two slots claiming
       it — one of which would then be stale for ever. */
    {
        PackageResourceKey same = keyOf(0xEEU, 3U, 7U, 7U);
        MemorySize got = 0UL;
        Unsigned32 occupiedBefore;

        memoryArenaInitialize(&arena, arenaStorage, ARENA_CAPACITY);
        (void)resourceCacheBegin(&cache, &arena, 4U, 64UL);
        (void)resourceCacheAdmit(&cache, &same, payload, 8UL);
        resourceCacheReleaseEverything(&cache);
        occupiedBefore = resourceCacheGetOccupied(&cache);
        (void)resourceCacheAdmit(&cache, &same, payload, 12UL);
        checkThat(&failureCount, "re-admitting the same key does not occupy a second slot",
                  resourceCacheGetOccupied(&cache) == occupiedBefore);
        checkThat(&failureCount, "and is a refresh rather than an eviction",
                  cache.evictions == 0U);
        checkThat(&failureCount, "and finding it gives the length most recently admitted",
                  resourceCacheFind(&cache, &same, &got) != NULL_POINTER && got == 12UL);
    }

    return checkSummarize(failureCount, "resource cache");
}
