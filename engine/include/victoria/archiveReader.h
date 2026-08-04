#ifndef VICTORIA_ARCHIVE_READER_HEADER
#define VICTORIA_ARCHIVE_READER_HEADER

#include "victoria/coreTypes.h"

/* Walking the RAR archive a repack's game is inside.
 *
 * The disc's TSData.exe is a program with 2.7 gibibytes appended past the end
 * of it, and what is appended starts Rar! — the fourth-generation mark, not the
 * fifth. A package mark turned up barely a kibibyte into it, which is where a
 * file's data begins if that file was stored rather than compressed.
 *
 * That distinction decides how much work the rest of this is. A stored entry is
 * a range of the file and nothing more: it can be handed to the package reader
 * as it stands. A compressed one needs an unpacker for a format whose only
 * complete implementation is licensed in a way this project cannot borrow from.
 * So the first thing this answers is which, and it answers it per entry rather
 * than for the archive, because an archive may hold both.
 *
 * A chain of blocks, each saying how long it is, so the walk is a matter of
 * addition. Nothing is decoded here and nothing is decompressed. */

/* Rar!\x1a\x07\x00 — the seven bytes an archive starts with. The fifth
   generation writes an eighth byte and is a different format; this reader says
   so rather than misreading it. */
#define ARCHIVE_MARK_BYTES 7UL

/* Blocks this reader knows by their type byte. */
#define ARCHIVE_BLOCK_ARCHIVE_HEADER 0x73U
#define ARCHIVE_BLOCK_FILE_HEADER 0x74U

/* Stored: the entry's bytes are in the archive exactly as they are in the file.
   Anything above this is one of the packing methods. */
#define ARCHIVE_METHOD_STORED 0x30U

/* Longest name this reader keeps. Retail paths run to about ninety characters
   and an entry whose name does not fit is reported rather than truncated. */
#define ARCHIVE_NAME_LIMIT 256U

typedef enum ArchiveReadResult
{
    ARCHIVE_READ_OK = 0,
    ARCHIVE_READ_NOT_AN_ARCHIVE,
    /* The fifth generation, which is a different format entirely. */
    ARCHIVE_READ_NEWER_GENERATION,
    ARCHIVE_READ_TRUNCATED,
    /* A block header whose length cannot be right — zero, or shorter than the
       fields it must contain. A walk that trusted it would not advance. */
    ARCHIVE_READ_BAD_BLOCK,
    ARCHIVE_READ_NAME_TOO_LONG,
    /* Not a file header. Not a failure: the walk steps past it. */
    ARCHIVE_READ_NOT_A_FILE,
    ARCHIVE_READ_RESULT_COUNT
} ArchiveReadResult;

const char *archiveReadResultGetName(ArchiveReadResult result);

typedef struct ArchiveEntry
{
    char name[ARCHIVE_NAME_LIMIT];
    /* Where this entry's data starts in the file, and how much of it there is
       packed and unpacked. Equal when the entry is stored. */
    Unsigned64 dataOffsetInBytes;
    Unsigned64 packedSizeInBytes;
    Unsigned64 unpackedSizeInBytes;
    Unsigned8 method;
    Unsigned8 leastVersionNeeded;
    Boolean isDirectory;
    /* Where the next block starts, so the walk continues without re-reading. */
    Unsigned64 nextBlockOffsetInBytes;
} ArchiveEntry;

/* Whether these bytes start an archive this reader can walk. The offset is
   where the first block is, past the mark. */
ArchiveReadResult archiveReadMark(const Unsigned8 *bytes, MemorySize byteCount,
                                  Unsigned64 markOffsetInBytes, Unsigned64 *firstBlockOffsetInBytes);

/* Reads one block, given bytes read at blockOffsetInBytes.
 *
 * Answers OK for a file header, filling the entry. Answers NOT_A_FILE for any
 * other block, still filling nextBlockOffsetInBytes so the caller can step past
 * it — an archive comment or a recovery record is something to walk over, not
 * something to stop at. */
ArchiveReadResult archiveReadBlock(const Unsigned8 *bytes, MemorySize byteCount,
                                   Unsigned64 blockOffsetInBytes, ArchiveEntry *entry);

/* How many bytes of a block a caller must supply for archiveReadBlock to be
   able to answer. A header plus the longest name it will accept. */
#define ARCHIVE_BLOCK_BYTES_NEEDED (64UL + (MemorySize)ARCHIVE_NAME_LIMIT)

#endif
