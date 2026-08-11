#ifndef VICTORIA_COMPRESSION_HEADER
#define VICTORIA_COMPRESSION_HEADER

#include "victoria/coreTypes.h"

#define COMPRESSION_REFPACK_SIGNATURE 0xFB10U

typedef enum CompressionResult
{
    COMPRESSION_OK = 0,
    COMPRESSION_NOT_COMPRESSED,
    COMPRESSION_TRUNCATED,
    COMPRESSION_BAD_REFERENCE,
    COMPRESSION_DESTINATION_TOO_SMALL
} CompressionResult;

const char *compressionResultGetName(CompressionResult result);

Boolean compressionLooksLikeRefPack(const Unsigned8 *bytes, MemorySize sizeInBytes);

MemorySize compressionGetDecompressedSize(const Unsigned8 *bytes, MemorySize sizeInBytes);

CompressionResult compressionDecompressRefPack(Unsigned8 *destination, MemorySize destinationCapacity,
                                               const Unsigned8 *source, MemorySize sourceSizeInBytes,
                                               MemorySize *decompressedSizeInBytes);

#endif
