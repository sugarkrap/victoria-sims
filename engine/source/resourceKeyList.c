#include "victoria/resourceKeyList.h"

const char *resourceKeyListResultGetName(ResourceKeyListResult result)
{
    switch (result)
    {
    case RESOURCE_KEY_LIST_OK:
        return "ok";
    case RESOURCE_KEY_LIST_TRUNCATED:
        return "the resource ends part way through";
    case RESOURCE_KEY_LIST_IMPLAUSIBLE_COUNT:
        return "more keys than the resource has room for";
    default:
        return "unknown";
    }
}

static Unsigned32 readUnsigned32(const Unsigned8 *bytes, MemorySize size, MemorySize *position,
                                 Boolean *overran)
{
    Unsigned32 value = 0U;
    Unsigned32 shift;

    for (shift = 0U; shift < 4U; shift++)
    {
        if (*position >= size)
        {
            *overran = BOOLEAN_TRUE;
            return value;
        }
        value |= (Unsigned32)bytes[*position] << (shift * 8U);
        (*position)++;
    }
    return value;
}

ResourceKeyListResult resourceKeyListRead(ResourceKeyList *list, const Unsigned8 *bytes,
                                          MemorySize sizeInBytes)
{
    MemorySize position = 0UL;
    Boolean overran = BOOLEAN_FALSE;
    Unsigned32 first;
    MemorySize wordsPerKey;
    Unsigned32 index;

    list->version = 1U;
    list->keyCount = 0U;
    list->storedKeyCount = 0U;
    if (bytes == NULL_POINTER)
    {
        return RESOURCE_KEY_LIST_TRUNCATED;
    }

    first = readUnsigned32(bytes, sizeInBytes, &position, &overran);
    if (overran)
    {
        return RESOURCE_KEY_LIST_TRUNCATED;
    }
    if (first == (Unsigned32)RESOURCE_KEY_LIST_SENTINEL)
    {
        list->version = readUnsigned32(bytes, sizeInBytes, &position, &overran);
        list->keyCount = readUnsigned32(bytes, sizeInBytes, &position, &overran);
    }
    else
    {
        list->version = 1U;
        list->keyCount = first;
    }
    if (overran)
    {
        return RESOURCE_KEY_LIST_TRUNCATED;
    }

    wordsPerKey = (list->version > 1U) ? 4UL : 3UL;
    if ((MemorySize)list->keyCount > (sizeInBytes - position) / (wordsPerKey * 4UL))
    {
        return RESOURCE_KEY_LIST_IMPLAUSIBLE_COUNT;
    }

    for (index = 0U; index < list->keyCount; index++)
    {
        Unsigned32 typeIdentifier = readUnsigned32(bytes, sizeInBytes, &position, &overran);
        Unsigned32 groupIdentifier = readUnsigned32(bytes, sizeInBytes, &position, &overran);
        Unsigned32 instanceIdentifier = readUnsigned32(bytes, sizeInBytes, &position, &overran);
        Unsigned32 instanceHigh = 0U;

        if (list->version > 1U)
        {
            instanceHigh = readUnsigned32(bytes, sizeInBytes, &position, &overran);
        }
        if (overran)
        {
            break;
        }
        if (list->storedKeyCount < RESOURCE_KEY_LIST_LIMIT)
        {
            list->keys[list->storedKeyCount].typeIdentifier = typeIdentifier;
            list->keys[list->storedKeyCount].groupIdentifier = groupIdentifier;
            list->keys[list->storedKeyCount].instanceIdentifier = instanceIdentifier;
            list->keys[list->storedKeyCount].instanceIdentifierHigh = instanceHigh;
            list->storedKeyCount++;
        }
    }
    return (list->storedKeyCount > 0U) ? RESOURCE_KEY_LIST_OK : RESOURCE_KEY_LIST_TRUNCATED;
}

const ResourceKeyEntry *resourceKeyListGet(const ResourceKeyList *list, Unsigned32 index)
{
    if (list == NULL_POINTER || index >= list->storedKeyCount)
    {
        return NULL_POINTER;
    }
    return &list->keys[index];
}
