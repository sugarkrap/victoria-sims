#ifndef VICTORIA_RESOURCE_KEY_LIST_HEADER
#define VICTORIA_RESOURCE_KEY_LIST_HEADER

#include "victoria/coreTypes.h"

/* Reads a cTSPersistResKeyList — a flat list of resource keys.
 *
 * A sidecar. It shares the group and instance of the resource it belongs to,
 * and exists so that resource can refer to keys by index instead of carrying a
 * whole type/group/instance for each one. A catalogue entry's `shapekeyidx` is
 * exactly that: a number, meaningless without this list.
 *
 * So this is the hop between "the catalogue names an outfit" and "here is its
 * shape". Everything after it — shape to geometry node to container — the
 * engine already walks.
 *
 * The version is the trap. Version 1 keys carry three words; version 2 carries
 * four, the extra one being the high half of the instance. A version 1 list
 * read as version 2 takes the next key's type word as an instance half and is
 * wrong about every key after the first, in a way that still yields plausible
 * numbers. Which version it is is decided by a sentinel at the front, and a
 * list with no sentinel is version 1 — its first word is the count. */

#define RESOURCE_KEY_LIST_TYPE 0xAC506764UL

/* Written at the front of a version 2 list and absent from a version 1 one. Not
   a magic number identifying the format — a version 1 list begins with its
   count, which could be anything — so this cannot be used to tell a key list
   from something that is not one. */
#define RESOURCE_KEY_LIST_SENTINEL 0xDEADBEEFUL

/* Keys kept from one list. Retail lists are short: an outfit names a shape and
   a handful of material overrides. */
#define RESOURCE_KEY_LIST_LIMIT 32U

typedef struct ResourceKeyEntry
{
    Unsigned32 typeIdentifier;
    Unsigned32 groupIdentifier;
    Unsigned32 instanceIdentifier;
    /* Nought on a version 1 list, which does not carry one. */
    Unsigned32 instanceIdentifierHigh;
} ResourceKeyEntry;

typedef enum ResourceKeyListResult
{
    RESOURCE_KEY_LIST_OK = 0,
    RESOURCE_KEY_LIST_TRUNCATED,
    /* A count larger than the resource has bytes to describe. */
    RESOURCE_KEY_LIST_IMPLAUSIBLE_COUNT
} ResourceKeyListResult;

#define RESOURCE_KEY_LIST_RESULT_COUNT 3U

const char *resourceKeyListResultGetName(ResourceKeyListResult result);

typedef struct ResourceKeyList
{
    Unsigned32 version;
    /* What the file said it holds, which may be more than was kept. */
    Unsigned32 keyCount;
    Unsigned32 storedKeyCount;
    ResourceKeyEntry keys[RESOURCE_KEY_LIST_LIMIT];
} ResourceKeyList;

ResourceKeyListResult resourceKeyListRead(ResourceKeyList *list, const Unsigned8 *bytes,
                                          MemorySize sizeInBytes);

/* The key at that index, or null when the list is shorter than the index it was
   asked for — which is what a property naming an index into a list that did not
   read looks like, and is worth telling apart from a key of all noughts. */
const ResourceKeyEntry *resourceKeyListGet(const ResourceKeyList *list, Unsigned32 index);

#endif
