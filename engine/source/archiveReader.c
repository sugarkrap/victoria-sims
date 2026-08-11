#include "victoria/archiveReader.h"

#define OFFSET_BLOCK_TYPE 2UL
#define OFFSET_BLOCK_FLAGS 3UL
#define OFFSET_BLOCK_SIZE 5UL
#define COMMON_HEADER_BYTES 7UL

#define OFFSET_PACKED_SIZE 7UL
#define OFFSET_UNPACKED_SIZE 11UL
#define OFFSET_FILE_CHECKSUM 16UL
#define OFFSET_LEAST_VERSION 24UL
#define OFFSET_METHOD 25UL
#define OFFSET_NAME_SIZE 26UL
#define OFFSET_ATTRIBUTES 28UL
#define FILE_HEADER_BYTES 32UL

#define FLAG_HAS_DATA 0x8000U
#define FLAG_LARGE_SIZES 0x0100U
#define FLAG_IS_DIRECTORY 0x00E0U
#define FLAG_HAS_SALT 0x0400U
#define SALT_BYTES 8UL

const char *archiveReadResultGetName(ArchiveReadResult result)
{
    switch (result)
    {
    case ARCHIVE_READ_OK:
        return "read";
    case ARCHIVE_READ_NOT_AN_ARCHIVE:
        return "not an archive";
    case ARCHIVE_READ_NEWER_GENERATION:
        return "a newer generation of archive than this reader knows";
    case ARCHIVE_READ_TRUNCATED:
        return "not enough bytes to hold a block";
    case ARCHIVE_READ_BAD_BLOCK:
        return "a block whose length cannot be right";
    case ARCHIVE_READ_NAME_TOO_LONG:
        return "a name longer than this reader keeps";
    case ARCHIVE_READ_NOT_A_FILE:
        return "not a file";
    default:
        return "unknown";
    }
}

static Unsigned16 readUnsigned16(const Unsigned8 *bytes, MemorySize offset)
{
    return (Unsigned16)((Unsigned16)bytes[offset] | ((Unsigned16)bytes[offset + 1UL] << 8));
}

static Unsigned32 readUnsigned32(const Unsigned8 *bytes, MemorySize offset)
{
    return (Unsigned32)bytes[offset] | ((Unsigned32)bytes[offset + 1UL] << 8) |
           ((Unsigned32)bytes[offset + 2UL] << 16) | ((Unsigned32)bytes[offset + 3UL] << 24);
}

ArchiveReadResult archiveReadMark(const Unsigned8 *bytes, MemorySize byteCount,
                                  Unsigned64 markOffsetInBytes, Unsigned64 *firstBlockOffsetInBytes)
{
    static const Unsigned8 mark[ARCHIVE_MARK_BYTES] = { 0x52U, 0x61U, 0x72U, 0x21U,
                                                        0x1AU, 0x07U, 0x00U };
    MemorySize index;

    if (byteCount < ARCHIVE_MARK_BYTES)
    {
        return ARCHIVE_READ_TRUNCATED;
    }
    for (index = 0UL; index < ARCHIVE_MARK_BYTES - 1UL; index += 1UL)
    {
        if (bytes[index] != mark[index])
        {
            return ARCHIVE_READ_NOT_AN_ARCHIVE;
        }
    }
    if (bytes[ARCHIVE_MARK_BYTES - 1UL] != 0U)
    {
        return ARCHIVE_READ_NEWER_GENERATION;
    }

    *firstBlockOffsetInBytes = markOffsetInBytes + (Unsigned64)ARCHIVE_MARK_BYTES;
    return ARCHIVE_READ_OK;
}

ArchiveReadResult archiveReadBlock(const Unsigned8 *bytes, MemorySize byteCount,
                                   Unsigned64 blockOffsetInBytes, ArchiveEntry *entry)
{
    Unsigned8 blockType;
    Unsigned16 flags;
    Unsigned16 headerSize;
    Unsigned64 packedSize;
    Unsigned64 unpackedSize;
    Unsigned16 nameSize;
    MemorySize nameAt;
    MemorySize index;

    entry->name[0] = '\0';
    entry->dataOffsetInBytes = 0ULL;
    entry->packedSizeInBytes = 0ULL;
    entry->unpackedSizeInBytes = 0ULL;
    entry->method = 0U;
    entry->leastVersionNeeded = 0U;
    entry->isDirectory = BOOLEAN_FALSE;
    entry->nextBlockOffsetInBytes = 0ULL;

    if (byteCount < COMMON_HEADER_BYTES)
    {
        return ARCHIVE_READ_TRUNCATED;
    }
    blockType = bytes[OFFSET_BLOCK_TYPE];
    flags = readUnsigned16(bytes, OFFSET_BLOCK_FLAGS);
    headerSize = readUnsigned16(bytes, OFFSET_BLOCK_SIZE);

    if ((MemorySize)headerSize < COMMON_HEADER_BYTES)
    {
        return ARCHIVE_READ_BAD_BLOCK;
    }

    if (blockType != (Unsigned8)ARCHIVE_BLOCK_FILE_HEADER)
    {
        Unsigned64 dataSize = 0ULL;

        if ((flags & (Unsigned16)FLAG_HAS_DATA) != 0U)
        {
            if (byteCount < COMMON_HEADER_BYTES + 4UL)
            {
                return ARCHIVE_READ_TRUNCATED;
            }
            dataSize = (Unsigned64)readUnsigned32(bytes, COMMON_HEADER_BYTES);
        }
        entry->nextBlockOffsetInBytes =
            blockOffsetInBytes + (Unsigned64)headerSize + dataSize;
        return ARCHIVE_READ_NOT_A_FILE;
    }

    if (byteCount < FILE_HEADER_BYTES)
    {
        return ARCHIVE_READ_TRUNCATED;
    }

    packedSize = (Unsigned64)readUnsigned32(bytes, OFFSET_PACKED_SIZE);
    unpackedSize = (Unsigned64)readUnsigned32(bytes, OFFSET_UNPACKED_SIZE);
    entry->leastVersionNeeded = bytes[OFFSET_LEAST_VERSION];
    entry->method = bytes[OFFSET_METHOD];
    nameSize = readUnsigned16(bytes, OFFSET_NAME_SIZE);
    nameAt = FILE_HEADER_BYTES;

    if ((flags & (Unsigned16)FLAG_LARGE_SIZES) != 0U)
    {
        if (byteCount < FILE_HEADER_BYTES + 8UL)
        {
            return ARCHIVE_READ_TRUNCATED;
        }
        packedSize |= (Unsigned64)readUnsigned32(bytes, FILE_HEADER_BYTES) << 32;
        unpackedSize |= (Unsigned64)readUnsigned32(bytes, FILE_HEADER_BYTES + 4UL) << 32;
        nameAt += 8UL;
    }

    if ((MemorySize)nameSize >= (MemorySize)ARCHIVE_NAME_LIMIT)
    {
        return ARCHIVE_READ_NAME_TOO_LONG;
    }
    if (nameAt + (MemorySize)nameSize > byteCount || nameAt + (MemorySize)nameSize > (MemorySize)headerSize)
    {
        return ARCHIVE_READ_TRUNCATED;
    }
    for (index = 0UL; index < (MemorySize)nameSize; index += 1UL)
    {
        char character = (char)bytes[nameAt + index];

        if (character == '\0')
        {
            break;
        }
        entry->name[index] = (character == '\\') ? '/' : character;
    }
    entry->name[index] = '\0';

    entry->packedSizeInBytes = packedSize;
    entry->unpackedSizeInBytes = unpackedSize;
    entry->isDirectory = (Boolean)((flags & (Unsigned16)FLAG_IS_DIRECTORY) ==
                                   (Unsigned16)FLAG_IS_DIRECTORY);

    entry->dataOffsetInBytes = blockOffsetInBytes + (Unsigned64)headerSize;
    if ((flags & (Unsigned16)FLAG_HAS_SALT) != 0U)
    {
        entry->dataOffsetInBytes += (Unsigned64)SALT_BYTES;
    }
    entry->nextBlockOffsetInBytes = entry->dataOffsetInBytes + packedSize;
    return ARCHIVE_READ_OK;
}
