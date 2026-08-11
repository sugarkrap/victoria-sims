#ifndef VICTORIA_ARCHIVE_READER_HEADER
#define VICTORIA_ARCHIVE_READER_HEADER

#include "victoria/coreTypes.h"

#define ARCHIVE_MARK_BYTES 7UL

#define ARCHIVE_BLOCK_ARCHIVE_HEADER 0x73U
#define ARCHIVE_BLOCK_FILE_HEADER 0x74U

#define ARCHIVE_METHOD_STORED 0x30U

#define ARCHIVE_NAME_LIMIT 256U

typedef enum ArchiveReadResult
{
    ARCHIVE_READ_OK = 0,
    ARCHIVE_READ_NOT_AN_ARCHIVE,
    ARCHIVE_READ_NEWER_GENERATION,
    ARCHIVE_READ_TRUNCATED,
    ARCHIVE_READ_BAD_BLOCK,
    ARCHIVE_READ_NAME_TOO_LONG,
    ARCHIVE_READ_NOT_A_FILE,
    ARCHIVE_READ_RESULT_COUNT
} ArchiveReadResult;

const char *archiveReadResultGetName(ArchiveReadResult result);

typedef struct ArchiveEntry
{
    char name[ARCHIVE_NAME_LIMIT];
    Unsigned64 dataOffsetInBytes;
    Unsigned64 packedSizeInBytes;
    Unsigned64 unpackedSizeInBytes;
    Unsigned8 method;
    Unsigned8 leastVersionNeeded;
    Boolean isDirectory;
    Unsigned64 nextBlockOffsetInBytes;
} ArchiveEntry;

ArchiveReadResult archiveReadMark(const Unsigned8 *bytes, MemorySize byteCount,
                                  Unsigned64 markOffsetInBytes, Unsigned64 *firstBlockOffsetInBytes);

ArchiveReadResult archiveReadBlock(const Unsigned8 *bytes, MemorySize byteCount,
                                   Unsigned64 blockOffsetInBytes, ArchiveEntry *entry);

#define ARCHIVE_BLOCK_BYTES_NEEDED (64UL + (MemorySize)ARCHIVE_NAME_LIMIT)

#endif
