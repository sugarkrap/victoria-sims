#ifndef VICTORIA_RESOURCE_KEY_LIST_HEADER
#define VICTORIA_RESOURCE_KEY_LIST_HEADER

#include "victoria/coreTypes.h"

#define RESOURCE_KEY_LIST_TYPE 0xAC506764UL

#define RESOURCE_KEY_LIST_SENTINEL 0xDEADBEEFUL

#define RESOURCE_KEY_LIST_LIMIT 32U

typedef struct ResourceKeyEntry
{
    Unsigned32 typeIdentifier;
    Unsigned32 groupIdentifier;
    Unsigned32 instanceIdentifier;
    Unsigned32 instanceIdentifierHigh;
} ResourceKeyEntry;

typedef enum ResourceKeyListResult
{
    RESOURCE_KEY_LIST_OK = 0,
    RESOURCE_KEY_LIST_TRUNCATED,
    RESOURCE_KEY_LIST_IMPLAUSIBLE_COUNT
} ResourceKeyListResult;

#define RESOURCE_KEY_LIST_RESULT_COUNT 3U

const char *resourceKeyListResultGetName(ResourceKeyListResult result);

typedef struct ResourceKeyList
{
    Unsigned32 version;
    Unsigned32 keyCount;
    Unsigned32 storedKeyCount;
    ResourceKeyEntry keys[RESOURCE_KEY_LIST_LIMIT];
} ResourceKeyList;

ResourceKeyListResult resourceKeyListRead(ResourceKeyList *list, const Unsigned8 *bytes,
                                          MemorySize sizeInBytes);

const ResourceKeyEntry *resourceKeyListGet(const ResourceKeyList *list, Unsigned32 index);

#endif
