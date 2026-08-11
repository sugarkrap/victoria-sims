#ifndef VICTORIA_TEXTURE_DECODE_HEADER
#define VICTORIA_TEXTURE_DECODE_HEADER

#include "victoria/coreTypes.h"
#include "victoria/textureReader.h"

MemorySize textureDecodeGetRequiredBytes(Integer32 width, Integer32 height);

typedef enum TextureDecodeResult
{
    TEXTURE_DECODE_OK = 0,
    TEXTURE_DECODE_UNSUPPORTED_FORMAT,
    TEXTURE_DECODE_TRUNCATED,
    TEXTURE_DECODE_DESTINATION_TOO_SMALL
} TextureDecodeResult;

const char *textureDecodeResultGetName(TextureDecodeResult result);

TextureDecodeResult textureDecodeLevel(Unsigned8 *destination, MemorySize destinationCapacity,
                                       const Unsigned8 *source, MemorySize sourceSizeInBytes,
                                       TextureFormat format, Integer32 width, Integer32 height);

#endif
