#ifndef VICTORIA_INSTALLER_READER_HEADER
#define VICTORIA_INSTALLER_READER_HEADER

#include "victoria/coreTypes.h"

/* Reading the installer a disc's game is sealed inside.
 *
 * The disc this was written for is a repack: 605 loose packages holding 193
 * mebibytes, and one 2.7 gibibyte file called TSData.exe holding everything
 * else. It starts MZP, which is Delphi's stub rather than Microsoft's, and
 * installer builders are Delphi programs. So the art is not missing from the
 * disc — it is behind this.
 *
 * Everything an installer holds is found through one table, and finding that
 * table is what this does. The rest — the compressed header, the file list, the
 * data blocks — hangs off the two offsets it ends with.
 *
 * The layout is version dependent, and this reader does not have a table of
 * versions. It does not need one: the table ends with a checksum over itself,
 * so the right layout is the one whose checksum agrees. That is a stronger test
 * than a version number, because a version number can be right while the field
 * list this reader believes goes with it is wrong — which is the failure that
 * produces a parser that appears to work and reads rubbish. */

/* Where a loader keeps this, past the DOS stub it had to start with. */
#define INSTALLER_LOADER_HEADER_OFFSET 0x30UL

/* Newer loaders keep a pointer here instead of the table, because the table
   outgrew the space. The mark says which of the two is present. */
#define INSTALLER_LOADER_POINTER_MARK 0x44536ED6UL

/* "rDlPtS" and two digits and a four byte mark. The digits are a revision of
   the table's own layout, not of the installer. */
#define INSTALLER_TABLE_IDENTIFIER_BYTES 12UL

/* Fields between the identifier and the checksum. Six in the oldest layout and
   eight in the newest; ten leaves room for a layout not met yet without letting
   a misread length run away. */
#define INSTALLER_TABLE_WORD_LIMIT 10U

/* Enough for the identifier, every word, and the checksum. */
#define INSTALLER_TABLE_LARGEST_BYTES \
    (INSTALLER_TABLE_IDENTIFIER_BYTES + ((MemorySize)INSTALLER_TABLE_WORD_LIMIT + 1UL) * 4UL)

/* The fixed field the version string sits in. */
#define INSTALLER_VERSION_STRING_BYTES 64UL

typedef enum InstallerReadResult
{
    INSTALLER_READ_OK = 0,
    /* No loader mark and no table identifier. An ordinary program. */
    INSTALLER_READ_NOT_AN_INSTALLER,
    /* The pointer at 0x30 disagrees with its own complement, so it is not a
       pointer — the mark matched something that is not a loader. */
    INSTALLER_READ_POINTER_INCONSISTENT,
    /* An identifier, but no length whose checksum agrees. The layout is one
       this reader has never met, or the table is damaged. */
    INSTALLER_READ_UNKNOWN_LAYOUT,
    INSTALLER_READ_TRUNCATED,
    /* Where a version string should be, something else is. */
    INSTALLER_READ_NOT_A_VERSION_STRING,
    INSTALLER_READ_RESULT_COUNT
} InstallerReadResult;

const char *installerReadResultGetName(InstallerReadResult result);

typedef struct InstallerOffsetTable
{
    /* The two digits from the identifier, as a number: 2 for rDlPtS02. */
    Unsigned32 tableRevision;
    /* Where the table was found, which is not always 0x30. */
    Unsigned64 tableOffsetInBytes;
    /* Fields between the identifier and the checksum, and their values. Kept
       whole because a field this reader does not name is still evidence, and a
       layout it does not know is exactly when somebody needs to see them. */
    Unsigned32 words[INSTALLER_TABLE_WORD_LIMIT];
    Unsigned32 wordCount;

    /* The first word: how much of the file the installer accounts for. A
       mismatch against the file's real length means the file is truncated, or
       that there is something appended past the installer. */
    Unsigned32 totalSizeInBytes;
    /* The last two words before the checksum, in every layout: where the
       compressed setup header starts, and where the file data starts. These are
       the only two fields anything downstream needs. */
    Unsigned32 headerOffsetInBytes;
    Unsigned32 dataOffsetInBytes;
} InstallerOffsetTable;

/* Where the offset table is, given the front of the file. Answers the offset
   itself for a loader that keeps the table inline, and a pointer's target for
   one that does not. Needs only the first 0x40 bytes. */
InstallerReadResult installerFindOffsetTable(const Unsigned8 *head, MemorySize headSize,
                                             Unsigned64 *tableOffsetInBytes);

/* Neither of those, and the table is somewhere in the file.
 *
 * Newer loaders keep it in one of the program's resources rather than at a
 * fixed place, and reaching it that way means walking a PE header, its section
 * table, and a resource tree — three formats deep, for an address. But the
 * table says what it is in its first eight bytes, and so does the version
 * string, so both can be found by looking for them.
 *
 * That is not a shortcut around parsing the program properly; it is a way of
 * finding out what is actually in the file before writing a parser on the
 * strength of a guess about what should be. */

#define INSTALLER_MARKER_NOT_FOUND 0xFFFFFFFFFFFFFFFFULL

/* Bytes of overlap a caller must keep between consecutive chunks, so a marker
   lying across a boundary is still met whole. One less than the longest thing
   looked for. */
#define INSTALLER_MARKER_OVERLAP_BYTES 24UL

/* The first offset table identifier in this chunk, as an offset into the file,
   or NOT_FOUND. chunkOffset is where the chunk starts in the file. */
Unsigned64 installerFindTableMarker(const Unsigned8 *bytes, MemorySize byteCount,
                                    Unsigned64 chunkOffset);

/* The same for the version string, which is stored uncompressed and so can be
   found this way even when nothing else can. */
Unsigned64 installerFindVersionMarker(const Unsigned8 *bytes, MemorySize byteCount,
                                      Unsigned64 chunkOffset);

/* And the same for any literal mark, so a caller can look for a container this
   module knows nothing about. Every archive worth the name starts with one. */
Unsigned64 installerFindMark(const Unsigned8 *bytes, MemorySize byteCount, Unsigned64 chunkOffset,
                             const char *mark, MemorySize markLength);

/* Reads the table, given the bytes at that offset.
 *
 * The field list is established by checksum rather than assumed: every
 * plausible length is tried and the one whose trailing word matches a checksum
 * over everything before it is the layout. A table whose checksum never agrees
 * is refused rather than guessed at. */
InstallerReadResult installerReadOffsetTable(const Unsigned8 *bytes, MemorySize byteCount,
                                             Unsigned64 tableOffsetInBytes,
                                             InstallerOffsetTable *table);

/* The version the installer was built with, out of the fixed field at the
   header offset — "Inno Setup Setup Data (5.5.0)" and its variants. Copied
   rather than interpreted: which fields the setup header holds depends on this,
   and the string itself is what a reader of that header will have to match. */
InstallerReadResult installerReadVersionString(const Unsigned8 *bytes, MemorySize byteCount,
                                               char *destination, MemorySize capacity);

#endif
