#include "victoria/archiveReader.h"

/* Every block starts with these, whatever kind it is. */
#define OFFSET_BLOCK_TYPE 2UL
#define OFFSET_BLOCK_FLAGS 3UL
#define OFFSET_BLOCK_SIZE 5UL
#define COMMON_HEADER_BYTES 7UL

/* A file header's own fields, counted from the start of the block. */
#define OFFSET_PACKED_SIZE 7UL
#define OFFSET_UNPACKED_SIZE 11UL
#define OFFSET_FILE_CHECKSUM 16UL
#define OFFSET_LEAST_VERSION 24UL
#define OFFSET_METHOD 25UL
#define OFFSET_NAME_SIZE 26UL
#define OFFSET_ATTRIBUTES 28UL
#define FILE_HEADER_BYTES 32UL

/* A block whose length includes data past the header says so here. Every file
   header does; most other blocks do not. */
#define FLAG_HAS_DATA 0x8000U
/* Sizes that do not fit in a word are split, with the high halves added after
   the fixed fields and before the name. */
#define FLAG_LARGE_SIZES 0x0100U
#define FLAG_IS_DIRECTORY 0x00E0U
/* Encrypted entries carry a salt between the name and the data. Not read here —
   an encrypted archive is refused elsewhere — but its length must be counted or
   every offset after it is wrong. */
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
    /* The seventh byte is the generation. Zero is the one this reads; one is a
       format that shares nothing with it but the first six bytes, and reading
       it as though it were this one would produce block lengths that walk into
       the middle of things. */
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

    /* A header shorter than the fields it must contain cannot be right, and a
       walk that believed it would advance by less than one block and read the
       same bytes for ever. */
    if ((MemorySize)headerSize < COMMON_HEADER_BYTES)
    {
        return ARCHIVE_READ_BAD_BLOCK;
    }

    if (blockType != (Unsigned8)ARCHIVE_BLOCK_FILE_HEADER)
    {
        Unsigned64 dataSize = 0ULL;

        /* Some other kind of block: an archive header, a comment, a recovery
           record. Stepped over rather than stopped at, which needs its data
           length as well as its header length when it claims to have data. */
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

    /* Anything above four gibibytes is split in two, and the high halves sit
       between the fixed fields and the name. Reading the name without counting
       them reads the last four bytes of a number as the first four letters of a
       path — which looks like a corrupt archive rather than a misread one. */
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

        /* Names may be stored twice, the second time wide, with a zero between.
           Only the first is wanted, and stopping at the zero is what separates
           them. */
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

    /* The data begins where the header ends, past the salt if there is one.
       The header's own length already covers the name and everything else. */
    entry->dataOffsetInBytes = blockOffsetInBytes + (Unsigned64)headerSize;
    if ((flags & (Unsigned16)FLAG_HAS_SALT) != 0U)
    {
        entry->dataOffsetInBytes += (Unsigned64)SALT_BYTES;
    }
    entry->nextBlockOffsetInBytes = entry->dataOffsetInBytes + packedSize;
    return ARCHIVE_READ_OK;
}
