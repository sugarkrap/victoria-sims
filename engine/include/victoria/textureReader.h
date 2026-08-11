#ifndef VICTORIA_TEXTURE_READER_HEADER
#define VICTORIA_TEXTURE_READER_HEADER

#include "victoria/coreTypes.h"
#include "victoria/resourceCollection.h"

typedef enum TextureFormat
{
    TEXTURE_FORMAT_UNKNOWN = 0,
    TEXTURE_FORMAT_RGBA32 = 1,
    TEXTURE_FORMAT_BGR24 = 2,
    TEXTURE_FORMAT_ALPHA8 = 3,
    TEXTURE_FORMAT_DXT1 = 4,
    TEXTURE_FORMAT_DXT3 = 5,
    TEXTURE_FORMAT_LUMINANCE8 = 6,
    TEXTURE_FORMAT_LUMINANCE16 = 7,
    TEXTURE_FORMAT_DXT5 = 8,
    TEXTURE_FORMAT_BGR24_REPEAT = 9
} TextureFormat;

const char *textureFormatGetName(TextureFormat format);

MemorySize textureFormatGetBlockBytes(TextureFormat format);

MemorySize textureFormatGetPixelBytes(TextureFormat format);

typedef enum TextureReadResult
{
    TEXTURE_READ_OK = 0,
    TEXTURE_READ_NOT_A_RESOURCE,
    TEXTURE_READ_OLDER_COLLECTION,
    TEXTURE_READ_WRONG_TYPE,
    TEXTURE_READ_TRUNCATED,
    TEXTURE_READ_UNSUPPORTED_FORMAT,
    TEXTURE_READ_NO_LEVELS
} TextureReadResult;

const char *textureReadResultGetName(TextureReadResult result);

typedef struct TextureDescription
{
    char resourceName[RESOURCE_NAME_LIMIT];
    Unsigned32 blockVersion;

    Integer32 width;
    Integer32 height;
    TextureFormat format;
    Unsigned32 mipCount;

    const Unsigned8 *bytes;
    MemorySize byteCount;
    Integer32 levelWidth;
    Integer32 levelHeight;

    Boolean largestIsElsewhere;
    char lifoName[RESOURCE_NAME_LIMIT];
} TextureDescription;

TextureReadResult textureReaderOpen(TextureDescription *texture, const Unsigned8 *bytes,
                                    MemorySize sizeInBytes);

typedef struct TextureLevel
{
    char resourceName[RESOURCE_NAME_LIMIT];
    Unsigned32 blockVersion;

    Integer32 width;
    Integer32 height;
    const Unsigned8 *bytes;
    MemorySize byteCount;
} TextureLevel;

TextureReadResult textureReaderOpenLevel(TextureLevel *level, const Unsigned8 *bytes,
                                         MemorySize sizeInBytes);

MemorySize textureFormatGetLevelBytes(TextureFormat format, Integer32 width, Integer32 height);

#endif
