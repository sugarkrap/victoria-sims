#include "victoria/textureReader.h"

#include "utils/strings.h"

#define BLOCK_IMAGE_DATA 0x1C4A276CUL

#define BLOCK_LEVEL_INFO 0xED534136UL

#define LARGEST_MIP_COUNT 32U
#define LARGEST_SUB_IMAGE_COUNT 32U
#define LARGEST_DIMENSION 8192

const char *textureFormatGetName(TextureFormat format)
{
    switch (format)
    {
    case TEXTURE_FORMAT_RGBA32:
        return "RGBA32";
    case TEXTURE_FORMAT_BGR24:
    case TEXTURE_FORMAT_BGR24_REPEAT:
        return "BGR24";
    case TEXTURE_FORMAT_ALPHA8:
        return "Alpha8";
    case TEXTURE_FORMAT_DXT1:
        return "DXT1";
    case TEXTURE_FORMAT_DXT3:
        return "DXT3";
    case TEXTURE_FORMAT_LUMINANCE8:
        return "Luminance8";
    case TEXTURE_FORMAT_LUMINANCE16:
        return "Luminance16";
    case TEXTURE_FORMAT_DXT5:
        return "DXT5";
    default:
        return "unknown";
    }
}

const char *textureReadResultGetName(TextureReadResult result)
{
    switch (result)
    {
    case TEXTURE_READ_OK:
        return "ok";
    case TEXTURE_READ_NOT_A_RESOURCE:
        return "not a scenegraph resource";
    case TEXTURE_READ_OLDER_COLLECTION:
        return "an older collection, laid out differently";
    case TEXTURE_READ_WRONG_TYPE:
        return "not an image";
    case TEXTURE_READ_TRUNCATED:
        return "the resource ends part way through";
    case TEXTURE_READ_UNSUPPORTED_FORMAT:
        return "a pixel format this reader does not decode";
    case TEXTURE_READ_NO_LEVELS:
        return "every level of this image is in another resource";
    default:
        return "unknown";
    }
}

MemorySize textureFormatGetBlockBytes(TextureFormat format)
{
    switch (format)
    {
    case TEXTURE_FORMAT_DXT1:
        return 8UL;
    case TEXTURE_FORMAT_DXT3:
    case TEXTURE_FORMAT_DXT5:
        return 16UL;
    default:
        return 0UL;
    }
}

MemorySize textureFormatGetPixelBytes(TextureFormat format)
{
    switch (format)
    {
    case TEXTURE_FORMAT_RGBA32:
        return 4UL;
    case TEXTURE_FORMAT_BGR24:
    case TEXTURE_FORMAT_BGR24_REPEAT:
        return 3UL;
    case TEXTURE_FORMAT_ALPHA8:
    case TEXTURE_FORMAT_LUMINANCE8:
        return 1UL;
    case TEXTURE_FORMAT_LUMINANCE16:
        return 2UL;
    default:
        return 0UL;
    }
}

MemorySize textureFormatGetLevelBytes(TextureFormat format, Integer32 width, Integer32 height)
{
    MemorySize blockBytes = textureFormatGetBlockBytes(format);
    MemorySize pixelBytes;

    if (width <= 0 || height <= 0)
    {
        return 0UL;
    }
    if (blockBytes != 0UL)
    {
        MemorySize blocksAcross = ((MemorySize)width + 3UL) / 4UL;
        MemorySize blocksDown = ((MemorySize)height + 3UL) / 4UL;

        return blocksAcross * blocksDown * blockBytes;
    }
    pixelBytes = textureFormatGetPixelBytes(format);
    return pixelBytes * (MemorySize)width * (MemorySize)height;
}

static Integer32 halveTo(Integer32 value, Unsigned32 times)
{
    Unsigned32 index;

    for (index = 0U; index < times; index++)
    {
        value /= 2;
    }
    return (value < 1) ? 1 : value;
}

TextureReadResult textureReaderOpen(TextureDescription *texture, const Unsigned8 *bytes,
                                    MemorySize sizeInBytes)
{
    ResourceCollection collection;
    ResourceCursor cursor;
    ResourceCollectionResult collectionResult;
    PersistTypeInfo blockType;
    Unsigned32 subImageCount;
    Unsigned32 subImage;
    Unsigned32 formatIdentifier;

    texture->resourceName[0] = '\0';
    texture->blockVersion = 0U;
    texture->width = 0;
    texture->height = 0;
    texture->format = TEXTURE_FORMAT_UNKNOWN;
    texture->mipCount = 0U;
    texture->bytes = NULL_POINTER;
    texture->byteCount = 0UL;
    texture->levelWidth = 0;
    texture->levelHeight = 0;
    texture->largestIsElsewhere = BOOLEAN_FALSE;
    texture->lifoName[0] = '\0';

    collectionResult = resourceCollectionOpen(&collection, &cursor, bytes, sizeInBytes);
    if (collectionResult != RESOURCE_COLLECTION_OK)
    {
        switch (collectionResult)
        {
        case RESOURCE_COLLECTION_OLDER:
            return TEXTURE_READ_OLDER_COLLECTION;
        case RESOURCE_COLLECTION_TRUNCATED:
            return TEXTURE_READ_TRUNCATED;
        default:
            return TEXTURE_READ_NOT_A_RESOURCE;
        }
    }
    if (collection.firstBlockType != (Unsigned32)BLOCK_IMAGE_DATA)
    {
        return TEXTURE_READ_WRONG_TYPE;
    }

    resourceCursorReadTypeInformation(&cursor, &blockType);
    if (cursor.overran)
    {
        return TEXTURE_READ_TRUNCATED;
    }
    if (blockType.typeIdentifier != (Unsigned32)BLOCK_IMAGE_DATA)
    {
        return TEXTURE_READ_WRONG_TYPE;
    }
    texture->blockVersion = blockType.version;

    resourceCursorReadTypeInformation(&cursor, NULL_POINTER);
    resourceCursorReadString(&cursor, texture->resourceName, RESOURCE_NAME_LIMIT);

    texture->width = (Integer32)resourceCursorReadUnsigned32(&cursor);
    texture->height = (Integer32)resourceCursorReadUnsigned32(&cursor);
    formatIdentifier = resourceCursorReadUnsigned32(&cursor);
    texture->mipCount = resourceCursorReadUnsigned32(&cursor);
    (void)resourceCursorReadReal32(&cursor);
    subImageCount = resourceCursorReadUnsigned32(&cursor);
    (void)resourceCursorReadUnsigned32(&cursor);
    if (cursor.overran)
    {
        return TEXTURE_READ_TRUNCATED;
    }

    if (formatIdentifier == 0U || formatIdentifier > (Unsigned32)TEXTURE_FORMAT_BGR24_REPEAT)
    {
        return TEXTURE_READ_UNSUPPORTED_FORMAT;
    }
    texture->format = (TextureFormat)formatIdentifier;
    if (texture->width <= 0 || texture->height <= 0 || texture->width > LARGEST_DIMENSION ||
        texture->height > LARGEST_DIMENSION || texture->mipCount == 0U ||
        texture->mipCount > LARGEST_MIP_COUNT || subImageCount == 0U ||
        subImageCount > LARGEST_SUB_IMAGE_COUNT)
    {
        return TEXTURE_READ_TRUNCATED;
    }

    if (blockType.version > 6U)
    {
        resourceCursorReadString(&cursor, NULL_POINTER, 0UL);
    }

    for (subImage = 0U; subImage < subImageCount; subImage++)
    {
        Unsigned32 levelCount = texture->mipCount;
        Unsigned32 level;

        if (blockType.version >= 9U)
        {
            levelCount = resourceCursorReadUnsigned32(&cursor);
            if (cursor.overran || levelCount > texture->mipCount)
            {
                return TEXTURE_READ_TRUNCATED;
            }
        }

        for (level = 0U; level < levelCount; level++)
        {
            Boolean isReference = BOOLEAN_FALSE;

            if (blockType.version >= 9U)
            {
                isReference = (resourceCursorReadUnsigned8(&cursor) != 0U) ? BOOLEAN_TRUE
                                                                          : BOOLEAN_FALSE;
            }
            if (isReference)
            {
                if (subImage == 0U)
                {
                    resourceCursorReadString(&cursor, texture->lifoName, RESOURCE_NAME_LIMIT);
                    texture->largestIsElsewhere = BOOLEAN_TRUE;
                }
                else
                {
                    resourceCursorReadString(&cursor, NULL_POINTER, 0UL);
                }
            }
            else
            {
                Unsigned32 levelBytes = resourceCursorReadUnsigned32(&cursor);
                MemorySize position = cursor.position;

                if (cursor.overran || (MemorySize)levelBytes > cursor.sizeInBytes - cursor.position)
                {
                    return TEXTURE_READ_TRUNCATED;
                }
                resourceCursorSkip(&cursor, (MemorySize)levelBytes);
                if (subImage == 0U)
                {
                    texture->bytes = &bytes[position];
                    texture->byteCount = (MemorySize)levelBytes;
                    texture->levelWidth = halveTo(texture->width, levelCount - 1U - level);
                    texture->levelHeight = halveTo(texture->height, levelCount - 1U - level);
                    texture->largestIsElsewhere = BOOLEAN_FALSE;
                }
            }
            if (cursor.overran)
            {
                return TEXTURE_READ_TRUNCATED;
            }
        }

        if (blockType.version > 6U)
        {
            resourceCursorSkip(&cursor, 4UL);
        }
        if (blockType.version > 7U)
        {
            (void)resourceCursorReadReal32(&cursor);
        }
        if (cursor.overran)
        {
            return TEXTURE_READ_TRUNCATED;
        }
    }

    if (texture->bytes == NULL_POINTER)
    {
        return TEXTURE_READ_NO_LEVELS;
    }
    if (texture->byteCount <
        textureFormatGetLevelBytes(texture->format, texture->levelWidth, texture->levelHeight))
    {
        return TEXTURE_READ_TRUNCATED;
    }
    return TEXTURE_READ_OK;
}

TextureReadResult textureReaderOpenLevel(TextureLevel *level, const Unsigned8 *bytes,
                                         MemorySize sizeInBytes)
{
    ResourceCollection collection;
    ResourceCursor cursor;
    ResourceCollectionResult collectionResult;
    PersistTypeInfo blockType;
    Unsigned32 byteCount;

    level->resourceName[0] = '\0';
    level->blockVersion = 0U;
    level->width = 0;
    level->height = 0;
    level->bytes = NULL_POINTER;
    level->byteCount = 0UL;

    collectionResult = resourceCollectionOpen(&collection, &cursor, bytes, sizeInBytes);
    if (collectionResult != RESOURCE_COLLECTION_OK)
    {
        switch (collectionResult)
        {
        case RESOURCE_COLLECTION_OLDER:
            return TEXTURE_READ_OLDER_COLLECTION;
        case RESOURCE_COLLECTION_TRUNCATED:
            return TEXTURE_READ_TRUNCATED;
        default:
            return TEXTURE_READ_NOT_A_RESOURCE;
        }
    }
    if (collection.firstBlockType != (Unsigned32)BLOCK_LEVEL_INFO)
    {
        return TEXTURE_READ_WRONG_TYPE;
    }

    resourceCursorReadTypeInformation(&cursor, &blockType);
    if (cursor.overran)
    {
        return TEXTURE_READ_TRUNCATED;
    }
    if (blockType.typeIdentifier != (Unsigned32)BLOCK_LEVEL_INFO)
    {
        return TEXTURE_READ_WRONG_TYPE;
    }
    level->blockVersion = blockType.version;

    resourceCursorReadTypeInformation(&cursor, NULL_POINTER);
    resourceCursorReadString(&cursor, level->resourceName, RESOURCE_NAME_LIMIT);

    level->width = (Integer32)resourceCursorReadUnsigned32(&cursor);
    level->height = (Integer32)resourceCursorReadUnsigned32(&cursor);
    (void)resourceCursorReadUnsigned32(&cursor);
    byteCount = resourceCursorReadUnsigned32(&cursor);
    if (cursor.overran)
    {
        return TEXTURE_READ_TRUNCATED;
    }
    if (level->width <= 0 || level->height <= 0 || level->width > LARGEST_DIMENSION ||
        level->height > LARGEST_DIMENSION)
    {
        return TEXTURE_READ_TRUNCATED;
    }
    if ((MemorySize)byteCount > sizeInBytes - cursor.position)
    {
        return TEXTURE_READ_TRUNCATED;
    }

    level->bytes = &bytes[cursor.position];
    level->byteCount = (MemorySize)byteCount;
    return TEXTURE_READ_OK;
}
