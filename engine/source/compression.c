#include "victoria/compression.h"

/* The header is nine bytes: a four byte compressed length, the two byte
 * signature, and a three byte decompressed length stored big endian — which is
 * the opposite of everything else in the format, and is not a mistake in this
 * reader. */
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
    /* Stored low byte first, so 0x10 then 0xFB reads as 0xFB10. */
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

    /* Each pass reads one control byte, copies some literals, then copies a run
     * from earlier in the output. Which of the four control shapes it is comes
     * from the top bits, and they differ in how many bytes follow and how far
     * back the run can reach. */
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
            /* Literals only, in multiples of four. */
            literalCount = (MemorySize)(((control & 0x1FU) << 2) + 4U);
            readPosition += 1UL;
        }
        else
        {
            /* The last control byte: up to three literals, then the stream ends
             * whether or not there are bytes after it. */
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
        /* Byte at a time, and deliberately not a block copy: a run may overlap
         * itself, which is how the format encodes a repeated pattern — a
         * distance of one and a count of fifty is fifty copies of one byte. A
         * memoryCopy would read the source before the destination was written
         * and produce something else entirely. */
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
    /* The header says how much there should be. Less than that means the stream
     * was cut short somewhere the control bytes did not reveal. */
    if (writePosition != expectedSize)
    {
        return COMPRESSION_TRUNCATED;
    }
    return COMPRESSION_OK;
}
