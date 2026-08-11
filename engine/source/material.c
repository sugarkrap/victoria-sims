#include "victoria/material.h"

#include "utils/strings.h"

#define BLOCK_MATERIAL_DEFINITION 0x49596978UL

#define BASE_TEXTURE_PROPERTY "stdMatBaseTextureName"

#define MINIMUM_BLOCK_VERSION 8U

const char *materialReadResultGetName(MaterialReadResult result)
{
    switch (result)
    {
    case MATERIAL_READ_OK:
        return "ok";
    case MATERIAL_READ_NOT_A_RESOURCE:
        return "not a scenegraph resource";
    case MATERIAL_READ_OLDER_COLLECTION:
        return "an older collection, laid out differently";
    case MATERIAL_READ_WRONG_TYPE:
        return "not a material definition";
    case MATERIAL_READ_TRUNCATED:
        return "the resource ends part way through";
    case MATERIAL_READ_UNSUPPORTED_VERSION:
        return "a block version this reader does not decode";
    default:
        return "unknown";
    }
}

void materialBuildResourceName(char *destination, MemorySize capacity, const char *name,
                               const char *suffix)
{
    if (destination == NULL_POINTER || capacity == 0UL)
    {
        return;
    }
    destination[0] = '\0';
    stringAppend(destination, capacity, name);
    stringAppend(destination, capacity, suffix);
}

MaterialReadResult materialRead(MaterialDescription *material, const Unsigned8 *bytes,
                                MemorySize sizeInBytes)
{
    ResourceCollection collection;
    ResourceCursor cursor;
    ResourceCollectionResult collectionResult;
    PersistTypeInfo blockType;
    Unsigned32 propertyCount;
    Unsigned32 index;

    material->resourceName[0] = '\0';
    material->materialName[0] = '\0';
    material->definitionType[0] = '\0';
    material->baseTextureName[0] = '\0';
    material->blockVersion = 0U;
    material->textureCount = 0U;
    material->storedTextureCount = 0U;

    collectionResult = resourceCollectionOpen(&collection, &cursor, bytes, sizeInBytes);
    if (collectionResult != RESOURCE_COLLECTION_OK)
    {
        switch (collectionResult)
        {
        case RESOURCE_COLLECTION_OLDER:
            return MATERIAL_READ_OLDER_COLLECTION;
        case RESOURCE_COLLECTION_TRUNCATED:
            return MATERIAL_READ_TRUNCATED;
        default:
            return MATERIAL_READ_NOT_A_RESOURCE;
        }
    }
    if (collection.firstBlockType != (Unsigned32)BLOCK_MATERIAL_DEFINITION)
    {
        return MATERIAL_READ_WRONG_TYPE;
    }

    resourceCursorReadTypeInformation(&cursor, &blockType);
    if (cursor.overran)
    {
        return MATERIAL_READ_TRUNCATED;
    }
    if (blockType.typeIdentifier != (Unsigned32)BLOCK_MATERIAL_DEFINITION)
    {
        return MATERIAL_READ_WRONG_TYPE;
    }
    material->blockVersion = blockType.version;
    if (blockType.version < MINIMUM_BLOCK_VERSION)
    {
        return MATERIAL_READ_UNSUPPORTED_VERSION;
    }

    resourceCursorReadTypeInformation(&cursor, NULL_POINTER);
    resourceCursorReadString(&cursor, material->resourceName, RESOURCE_NAME_LIMIT);
    resourceCursorReadString(&cursor, material->materialName, RESOURCE_NAME_LIMIT);
    resourceCursorReadString(&cursor, material->definitionType, RESOURCE_NAME_LIMIT);

    propertyCount = resourceCursorReadUnsigned32(&cursor);
    if (cursor.overran)
    {
        return MATERIAL_READ_TRUNCATED;
    }
    if ((MemorySize)propertyCount * 2UL > cursor.sizeInBytes - cursor.position)
    {
        return MATERIAL_READ_TRUNCATED;
    }
    for (index = 0U; index < propertyCount; index++)
    {
        char key[RESOURCE_NAME_LIMIT];

        resourceCursorReadString(&cursor, key, RESOURCE_NAME_LIMIT);
        if (stringEquals(key, BASE_TEXTURE_PROPERTY))
        {
            resourceCursorReadString(&cursor, material->baseTextureName, RESOURCE_NAME_LIMIT);
        }
        else
        {
            resourceCursorReadString(&cursor, NULL_POINTER, 0UL);
        }
        if (cursor.overran)
        {
            return MATERIAL_READ_TRUNCATED;
        }
    }

    material->textureCount = resourceCursorReadUnsigned32(&cursor);
    if (cursor.overran ||
        (MemorySize)material->textureCount > cursor.sizeInBytes - cursor.position)
    {
        return MATERIAL_READ_TRUNCATED;
    }
    for (index = 0U; index < material->textureCount; index++)
    {
        if (material->storedTextureCount < MATERIAL_TEXTURE_LIMIT)
        {
            resourceCursorReadString(&cursor, material->textureNames[material->storedTextureCount],
                                     RESOURCE_NAME_LIMIT);
            material->storedTextureCount++;
        }
        else
        {
            resourceCursorReadString(&cursor, NULL_POINTER, 0UL);
        }
        if (cursor.overran)
        {
            return MATERIAL_READ_TRUNCATED;
        }
    }

    if (material->baseTextureName[0] == '\0' && material->storedTextureCount > 0U)
    {
        stringAppend(material->baseTextureName, RESOURCE_NAME_LIMIT, material->textureNames[0]);
    }
    return MATERIAL_READ_OK;
}
