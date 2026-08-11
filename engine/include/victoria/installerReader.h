#ifndef VICTORIA_INSTALLER_READER_HEADER
#define VICTORIA_INSTALLER_READER_HEADER

#include "victoria/coreTypes.h"

#define INSTALLER_LOADER_HEADER_OFFSET 0x30UL

#define INSTALLER_LOADER_POINTER_MARK 0x44536ED6UL

#define INSTALLER_TABLE_IDENTIFIER_BYTES 12UL

#define INSTALLER_TABLE_WORD_LIMIT 10U

#define INSTALLER_TABLE_LARGEST_BYTES \
    (INSTALLER_TABLE_IDENTIFIER_BYTES + ((MemorySize)INSTALLER_TABLE_WORD_LIMIT + 1UL) * 4UL)

#define INSTALLER_VERSION_STRING_BYTES 64UL

typedef enum InstallerReadResult
{
    INSTALLER_READ_OK = 0,
    INSTALLER_READ_NOT_AN_INSTALLER,
    INSTALLER_READ_POINTER_INCONSISTENT,
    INSTALLER_READ_UNKNOWN_LAYOUT,
    INSTALLER_READ_TRUNCATED,
    INSTALLER_READ_NOT_A_VERSION_STRING,
    INSTALLER_READ_RESULT_COUNT
} InstallerReadResult;

const char *installerReadResultGetName(InstallerReadResult result);

typedef struct InstallerOffsetTable
{
    Unsigned32 tableRevision;
    Unsigned64 tableOffsetInBytes;
    Unsigned32 words[INSTALLER_TABLE_WORD_LIMIT];
    Unsigned32 wordCount;

    Unsigned32 totalSizeInBytes;
    Unsigned32 headerOffsetInBytes;
    Unsigned32 dataOffsetInBytes;
} InstallerOffsetTable;

InstallerReadResult installerFindOffsetTable(const Unsigned8 *head, MemorySize headSize,
                                             Unsigned64 *tableOffsetInBytes);

#define INSTALLER_MARKER_NOT_FOUND 0xFFFFFFFFFFFFFFFFULL

#define INSTALLER_MARKER_OVERLAP_BYTES 24UL

Unsigned64 installerFindTableMarker(const Unsigned8 *bytes, MemorySize byteCount,
                                    Unsigned64 chunkOffset);

Unsigned64 installerFindVersionMarker(const Unsigned8 *bytes, MemorySize byteCount,
                                      Unsigned64 chunkOffset);

Unsigned64 installerFindMark(const Unsigned8 *bytes, MemorySize byteCount, Unsigned64 chunkOffset,
                             const char *mark, MemorySize markLength);

InstallerReadResult installerReadOffsetTable(const Unsigned8 *bytes, MemorySize byteCount,
                                             Unsigned64 tableOffsetInBytes,
                                             InstallerOffsetTable *table);

InstallerReadResult installerReadVersionString(const Unsigned8 *bytes, MemorySize byteCount,
                                               char *destination, MemorySize capacity);

#endif
