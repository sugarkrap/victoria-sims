#ifndef VICTORIA_TEXTURE_READER_HEADER
#define VICTORIA_TEXTURE_READER_HEADER

#include "victoria/coreTypes.h"
#include "victoria/resourceCollection.h"

/* Reads a TXTR — a cImageData block — into something that can be uploaded.
 *
 * Two things about the layout are worth knowing before reading any of this,
 * because both are the opposite of what you would guess.
 *
 * The mip levels are stored smallest first. Level zero is a single block of
 * pixels and the last entry is the full size image, which is the reverse of
 * every graphics API's numbering.
 *
 * The largest level is frequently not in the resource at all. It is a named
 * reference to a LIFO, a separate resource holding just that one level. A
 * 256 by 256 texture whose TXTR is eleven kilobytes is not truncated — the
 * thirty-two kilobyte top level is somewhere else, and this says where.
 *
 * Nothing here allocates or decodes. The bytes are pointed at where they lie in
 * the resource, and turning them into pixels is textureDecode's job. */

typedef enum TextureFormat
{
    TEXTURE_FORMAT_UNKNOWN = 0,
    TEXTURE_FORMAT_RGBA32 = 1,
    TEXTURE_FORMAT_BGR24 = 2,
    TEXTURE_FORMAT_ALPHA8 = 3,
    TEXTURE_FORMAT_DXT1 = 4,
    TEXTURE_FORMAT_DXT3 = 5,
    /* White, with the bits carrying transparency. */
    TEXTURE_FORMAT_LUMINANCE8 = 6,
    TEXTURE_FORMAT_LUMINANCE16 = 7,
    TEXTURE_FORMAT_DXT5 = 8,
    /* The same layout as BGR24 under a second number, which is in the format
       and not a mistake in this reader. */
    TEXTURE_FORMAT_BGR24_REPEAT = 9
} TextureFormat;

const char *textureFormatGetName(TextureFormat format);

/* Bytes one four by four block occupies, or zero when the format is not block
   compressed. */
MemorySize textureFormatGetBlockBytes(TextureFormat format);

/* Bytes one pixel occupies for the formats that are not block compressed, or
   zero for the ones that are. */
MemorySize textureFormatGetPixelBytes(TextureFormat format);

typedef enum TextureReadResult
{
    TEXTURE_READ_OK = 0,
    TEXTURE_READ_NOT_A_RESOURCE,
    TEXTURE_READ_OLDER_COLLECTION,
    TEXTURE_READ_WRONG_TYPE,
    TEXTURE_READ_TRUNCATED,
    TEXTURE_READ_UNSUPPORTED_FORMAT,
    /* Read, but every level of it lives in a LIFO. There is nothing to upload
       from this resource alone. */
    TEXTURE_READ_NO_LEVELS
} TextureReadResult;

const char *textureReadResultGetName(TextureReadResult result);

typedef struct TextureDescription
{
    char resourceName[RESOURCE_NAME_LIMIT];
    Unsigned32 blockVersion;

    /* The full size of the image, which is the size of the last level and not
       of the one this description points at. */
    Integer32 width;
    Integer32 height;
    TextureFormat format;
    Unsigned32 mipCount;

    /* The largest level whose bytes are in this resource, which is what can
       actually be uploaded without finding another file first. */
    const Unsigned8 *bytes;
    MemorySize byteCount;
    Integer32 levelWidth;
    Integer32 levelHeight;

    /* Set when a level larger than the one above lives elsewhere. Not an
       error — it is how the format stores a big texture — but a caller that
       does not follow it is showing a lower resolution than the disc holds,
       and should be able to say so. */
    Boolean largestIsElsewhere;
    char lifoName[RESOURCE_NAME_LIMIT];
} TextureDescription;

TextureReadResult textureReaderOpen(TextureDescription *texture, const Unsigned8 *bytes,
                                    MemorySize sizeInBytes);

/* One mip level on its own, out of the LIFO a texture referred to.
 *
 * Not the same block as a texture, which is the first thing to know about it: a
 * TXTR holds a cImageData and a LIFO holds a cLevelInfo, and running the one
 * reader over the other gets nowhere. The difference is not decorative — a
 * level knows its size and its bytes and nothing else, because the format it is
 * in belongs to the texture that named it.
 *
 * So a caller needs both: the description for what the pixels mean, and this
 * for which pixels. */
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

/* How many bytes a level of these dimensions occupies in this format. Zero when
   the format is not one this reader measures. */
MemorySize textureFormatGetLevelBytes(TextureFormat format, Integer32 width, Integer32 height);

#endif
