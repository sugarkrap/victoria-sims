#include "victoria/compression.h"

#define HEADER_SIZE 9UL
#define SIGNATURE_OFFSET 4UL
#define DECOMPRESSED_SIZE_OFFSET 6UL

const char *compressionResultGetName(CompressionResult result)
{
    switch (result)
    {
    case COMPRESSION_OK:
        return "ok";
    case COMPRESSION_NOT_COMPRESSED:
        return "not a RefPack stream";
    case COMPRESSION_TRUNCATED:
        return "the stream ends part way through";
    case COMPRESSION_BAD_REFERENCE:
        return "a back reference points before the start of the output";
    case COMPRESSION_DESTINATION_TOO_SMALL:
        return "the decompressed resource does not fit where it was to go";
    default:
        return "unknown";
    }
}

Boolean compressionLooksLikeRefPack(const Unsigned8 *bytes, MemorySize sizeInBytes)
{
    if (bytes == NULL_POINTER || sizeInBytes < HEADER_SIZE)
    {
        return BOOLEAN_FALSE;
    }
    if (bytes[SIGNATURE_OFFSET] != 0x10U || bytes[SIGNATURE_OFFSET + 1UL] != 0xFBU)
    {
        return BOOLEAN_FALSE;
    }
    return BOOLEAN_TRUE;
}

MemorySize compressionGetDecompressedSize(const Unsigned8 *bytes, MemorySize sizeInBytes)
{
    if (!compressionLooksLikeRefPack(bytes, sizeInBytes))
    {
        return 0UL;
    }
    return ((MemorySize)bytes[DECOMPRESSED_SIZE_OFFSET] << 16) |
           ((MemorySize)bytes[DECOMPRESSED_SIZE_OFFSET + 1UL] << 8) |
           (MemorySize)bytes[DECOMPRESSED_SIZE_OFFSET + 2UL];
}

CompressionResult compressionDecompressRefPack(Unsigned8 *destination, MemorySize destinationCapacity,
                                               const Unsigned8 *source, MemorySize sourceSizeInBytes,
                                               MemorySize *decompressedSizeInBytes)
{
    MemorySize readPosition = HEADER_SIZE;
    MemorySize writePosition = 0UL;
    MemorySize expectedSize;

    if (decompressedSizeInBytes != NULL_POINTER)
    {
        *decompressedSizeInBytes = 0UL;
    }
    if (!compressionLooksLikeRefPack(source, sourceSizeInBytes))
    {
        return COMPRESSION_NOT_COMPRESSED;
    }
    expectedSize = compressionGetDecompressedSize(source, sourceSizeInBytes);
    if (expectedSize > destinationCapacity)
    {
        return COMPRESSION_DESTINATION_TOO_SMALL;
    }

    while (readPosition < sourceSizeInBytes)
    {
        Unsigned8 control = source[readPosition];
        MemorySize literalCount;
        MemorySize copyCount = 0UL;
        MemorySize distance = 0UL;
        MemorySize index;
        Boolean isLast = BOOLEAN_FALSE;

        if (control < 0x80U)
        {
            if (readPosition + 2UL > sourceSizeInBytes)
            {
                return COMPRESSION_TRUNCATED;
            }
            literalCount = (MemorySize)(control & 0x03U);
            copyCount = (MemorySize)(((control & 0x1CU) >> 2) + 3U);
            distance = (MemorySize)(((control & 0x60U) << 3) | source[readPosition + 1UL]) + 1UL;
            readPosition += 2UL;
        }
        else if (control < 0xC0U)
        {
            if (readPosition + 3UL > sourceSizeInBytes)
            {
                return COMPRESSION_TRUNCATED;
            }
            literalCount = (MemorySize)((source[readPosition + 1UL] & 0xC0U) >> 6);
            copyCount = (MemorySize)((control & 0x3FU) + 4U);
            distance = (MemorySize)(((source[readPosition + 1UL] & 0x3FU) << 8) |
                                    source[readPosition + 2UL]) +
                       1UL;
            readPosition += 3UL;
        }
        else if (control < 0xE0U)
        {
            if (readPosition + 4UL > sourceSizeInBytes)
            {
                return COMPRESSION_TRUNCATED;
            }
            literalCount = (MemorySize)(control & 0x03U);
            copyCount = (MemorySize)(((control & 0x0CU) << 6) | source[readPosition + 3UL]) + 5UL;
            distance = (MemorySize)(((control & 0x10U) << 12) | (source[readPosition + 1UL] << 8) |
                                    source[readPosition + 2UL]) +
                       1UL;
            readPosition += 4UL;
        }
        else if (control < 0xFCU)
        {
            literalCount = (MemorySize)(((control & 0x1FU) << 2) + 4U);
            readPosition += 1UL;
        }
        else
        {
            literalCount = (MemorySize)(control & 0x03U);
            readPosition += 1UL;
            isLast = BOOLEAN_TRUE;
        }

        if (literalCount > sourceSizeInBytes - readPosition)
        {
            return COMPRESSION_TRUNCATED;
        }
        if (literalCount > destinationCapacity - writePosition)
        {
            return COMPRESSION_DESTINATION_TOO_SMALL;
        }
        for (index = 0UL; index < literalCount; index++)
        {
            destination[writePosition + index] = source[readPosition + index];
        }
        readPosition += literalCount;
        writePosition += literalCount;

        if (copyCount == 0UL)
        {
            if (isLast)
            {
                break;
            }
            continue;
        }

        if (distance > writePosition)
        {
            return COMPRESSION_BAD_REFERENCE;
        }
        if (copyCount > destinationCapacity - writePosition)
        {
            return COMPRESSION_DESTINATION_TOO_SMALL;
        }
        for (index = 0UL; index < copyCount; index++)
        {
            destination[writePosition + index] = destination[writePosition + index - distance];
        }
        writePosition += copyCount;
    }

    if (decompressedSizeInBytes != NULL_POINTER)
    {
        *decompressedSizeInBytes = writePosition;
    }
    if (writePosition != expectedSize)
    {
        return COMPRESSION_TRUNCATED;
    }
    return COMPRESSION_OK;
}
