#include "victoria/resourceCollection.h"

#define OLDER_COLLECTION_MARK_LOW 0xFFF00000UL

void resourceCursorInitialize(ResourceCursor *cursor, const Unsigned8 *bytes, MemorySize sizeInBytes)
{
    cursor->bytes = bytes;
    cursor->sizeInBytes = (bytes == NULL_POINTER) ? 0UL : sizeInBytes;
    cursor->position = 0UL;
    cursor->overran = (bytes == NULL_POINTER) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

Boolean resourceCursorSkip(ResourceCursor *cursor, MemorySize count)
{
    if (cursor->overran || count > cursor->sizeInBytes - cursor->position)
    {
        cursor->overran = BOOLEAN_TRUE;
        return BOOLEAN_FALSE;
    }
    cursor->position += count;
    return BOOLEAN_TRUE;
}

Unsigned8 resourceCursorReadUnsigned8(ResourceCursor *cursor)
{
    MemorySize at = cursor->position;

    if (!resourceCursorSkip(cursor, 1UL))
    {
        return 0U;
    }
    return cursor->bytes[at];
}

Unsigned16 resourceCursorReadUnsigned16(ResourceCursor *cursor)
{
    MemorySize at = cursor->position;

    if (!resourceCursorSkip(cursor, 2UL))
    {
        return 0U;
    }
    return (Unsigned16)((Unsigned16)cursor->bytes[at] | ((Unsigned16)cursor->bytes[at + 1UL] << 8));
}

Unsigned32 resourceCursorReadUnsigned32(ResourceCursor *cursor)
{
    MemorySize at = cursor->position;

    if (!resourceCursorSkip(cursor, 4UL))
    {
        return 0U;
    }
    return (Unsigned32)cursor->bytes[at] | ((Unsigned32)cursor->bytes[at + 1UL] << 8) |
           ((Unsigned32)cursor->bytes[at + 2UL] << 16) | ((Unsigned32)cursor->bytes[at + 3UL] << 24);
}

Real32 resourceCursorReadReal32(ResourceCursor *cursor)
{
    union
    {
        Unsigned32 word;
        Real32 value;
    } converter;

    converter.word = resourceCursorReadUnsigned32(cursor);
    return converter.value;
}

void resourceCursorReadString(ResourceCursor *cursor, char *destination, MemorySize capacity)
{
    MemorySize length = 0UL;
    MemorySize shift = 0UL;
    MemorySize index;

    if (destination != NULL_POINTER && capacity > 0UL)
    {
        destination[0] = '\0';
    }

    for (;;)
    {
        Unsigned8 byte = resourceCursorReadUnsigned8(cursor);

        length |= (MemorySize)(byte & 0x7FU) << shift;
        if ((byte & 0x80U) == 0U || shift >= 28UL || cursor->overran)
        {
            break;
        }
        shift += 7UL;
    }

    for (index = 0UL; index < length; index++)
    {
        Unsigned8 character = resourceCursorReadUnsigned8(cursor);

        if (destination != NULL_POINTER && index + 1UL < capacity)
        {
            destination[index] = (char)character;
        }
        if (cursor->overran)
        {
            break;
        }
    }
    if (destination != NULL_POINTER && capacity > 0UL)
    {
        destination[(length + 1UL < capacity) ? length : capacity - 1UL] = '\0';
    }
}

void resourceCursorReadTypeInformation(ResourceCursor *cursor, PersistTypeInfo *typeInformation)
{
    if (typeInformation == NULL_POINTER)
    {
        resourceCursorReadString(cursor, NULL_POINTER, 0UL);
        (void)resourceCursorReadUnsigned32(cursor);
        (void)resourceCursorReadUnsigned32(cursor);
        return;
    }
    resourceCursorReadString(cursor, typeInformation->name, RESOURCE_NAME_LIMIT);
    typeInformation->typeIdentifier = resourceCursorReadUnsigned32(cursor);
    typeInformation->version = resourceCursorReadUnsigned32(cursor);
}

ObjectReference resourceCursorReadObjectReference(ResourceCursor *cursor)
{
    ObjectReference reference;
    Unsigned8 present;

    reference.kind = OBJECT_REFERENCE_NONE;
    reference.index = 0;

    present = resourceCursorReadUnsigned8(cursor);
    if (present == 0U || cursor->overran)
    {
        return reference;
    }

    reference.kind = (resourceCursorReadUnsigned8(cursor) == 0U) ? OBJECT_REFERENCE_INTERNAL
                                                                 : OBJECT_REFERENCE_EXTERNAL;
    reference.index = (Integer32)resourceCursorReadUnsigned32(cursor);
    if (cursor->overran)
    {
        reference.kind = OBJECT_REFERENCE_NONE;
        reference.index = 0;
    }
    return reference;
}

const char *resourceCollectionResultGetName(ResourceCollectionResult result)
{
    switch (result)
    {
    case RESOURCE_COLLECTION_OK:
        return "ok";
    case RESOURCE_COLLECTION_NOT_A_RESOURCE:
        return "not a scenegraph resource";
    case RESOURCE_COLLECTION_OLDER:
        return "an older collection, laid out differently";
    case RESOURCE_COLLECTION_TRUNCATED:
        return "the resource ends part way through";
    case RESOURCE_COLLECTION_NO_BLOCKS:
        return "a collection holding no blocks";
    default:
        return "unknown";
    }
}

ResourceCollectionResult resourceCollectionOpen(ResourceCollection *collection, ResourceCursor *cursor,
                                                const Unsigned8 *bytes, MemorySize sizeInBytes)
{
    Unsigned32 index;

    collection->versionMark = 0U;
    collection->linkCount = 0U;
    collection->storedLinkCount = 0U;
    collection->blockCount = 0U;
    collection->firstBlockType = 0U;

    resourceCursorInitialize(cursor, bytes, sizeInBytes);
    collection->versionMark = resourceCursorReadUnsigned32(cursor);
    if (cursor->overran)
    {
        return RESOURCE_COLLECTION_TRUNCATED;
    }
    if (collection->versionMark != (Unsigned32)RESOURCE_COLLECTION_MARK)
    {
        if ((collection->versionMark & 0x0000FFFFUL) == 0x00000001UL &&
            (collection->versionMark & 0xFFFF0000UL) >= OLDER_COLLECTION_MARK_LOW)
        {
            return RESOURCE_COLLECTION_OLDER;
        }
        return RESOURCE_COLLECTION_NOT_A_RESOURCE;
    }

    collection->linkCount = resourceCursorReadUnsigned32(cursor);
    if (cursor->overran)
    {
        return RESOURCE_COLLECTION_TRUNCATED;
    }
    for (index = 0U; index < collection->linkCount; index++)
    {
        Unsigned32 group = resourceCursorReadUnsigned32(cursor);
        Unsigned32 instance = resourceCursorReadUnsigned32(cursor);
        Unsigned32 instanceHigh = resourceCursorReadUnsigned32(cursor);
        Unsigned32 type = resourceCursorReadUnsigned32(cursor);

        if (cursor->overran)
        {
            return RESOURCE_COLLECTION_TRUNCATED;
        }
        if (index < RESOURCE_COLLECTION_LINK_LIMIT)
        {
            collection->links[index].typeIdentifier = type;
            collection->links[index].groupIdentifier = group;
            collection->links[index].instanceIdentifier = instance;
            collection->links[index].instanceIdentifierHigh = instanceHigh;
            collection->storedLinkCount = index + 1U;
        }
    }

    collection->blockCount = resourceCursorReadUnsigned32(cursor);
    if (cursor->overran)
    {
        return RESOURCE_COLLECTION_TRUNCATED;
    }
    if (collection->blockCount == 0U)
    {
        return RESOURCE_COLLECTION_NO_BLOCKS;
    }

    collection->firstBlockType = resourceCursorReadUnsigned32(cursor);
    resourceCursorSkip(cursor, ((MemorySize)collection->blockCount - 1UL) * 4UL);
    if (cursor->overran)
    {
        return RESOURCE_COLLECTION_TRUNCATED;
    }
    return RESOURCE_COLLECTION_OK;
}

const PackageResourceKey *resourceCollectionGetLink(const ResourceCollection *collection,
                                                    ObjectReference reference)
{
    if (reference.kind != OBJECT_REFERENCE_EXTERNAL || reference.index < 0)
    {
        return NULL_POINTER;
    }
    if ((Unsigned32)reference.index >= collection->storedLinkCount)
    {
        return NULL_POINTER;
    }
    return &collection->links[reference.index];
}
