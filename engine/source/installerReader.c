#include "victoria/installerReader.h"

#include "utils/checksum.h"
#include "utils/strings.h"

/* Shortest and longest field list worth trying. Six is the oldest layout —
   total size, where the program is, how long it is packed and unpacked, its
   checksum, then the two offsets — and every later one added fields in the
   middle. Below six there is nothing to find; above the limit a length that
   agrees by accident becomes likelier than one that agrees because it is
   right. */
#define SMALLEST_WORD_COUNT 4U

/* What every table ends with. Inno writes these two last whatever else it
   writes, so they can be taken from the end rather than counted to from the
   front — which is what lets this read a layout it does not know. */
#define TRAILING_OFFSET_WORDS 2U

static const char tableIdentifierPrefix[] = "rDlPtS";
static const char versionStringPrefix[] = "Inno Setup Setup Data";

const char *installerReadResultGetName(InstallerReadResult result)
{
    switch (result)
    {
    case INSTALLER_READ_OK:
        return "read";
    case INSTALLER_READ_NOT_AN_INSTALLER:
        return "not an installer";
    case INSTALLER_READ_POINTER_INCONSISTENT:
        return "the loader's pointer disagrees with itself";
    case INSTALLER_READ_UNKNOWN_LAYOUT:
        return "an offset table layout this reader does not know";
    case INSTALLER_READ_TRUNCATED:
        return "not enough bytes to hold an offset table";
    case INSTALLER_READ_NOT_A_VERSION_STRING:
        return "no version string where one should be";
    default:
        return "unknown";
    }
}

static Unsigned32 readUnsigned32(const Unsigned8 *bytes, MemorySize offset)
{
    return (Unsigned32)bytes[offset] | ((Unsigned32)bytes[offset + 1UL] << 8) |
           ((Unsigned32)bytes[offset + 2UL] << 16) | ((Unsigned32)bytes[offset + 3UL] << 24);
}

static Boolean looksLikeTableIdentifier(const Unsigned8 *bytes, MemorySize byteCount)
{
    MemorySize index;
    MemorySize prefixLength = stringLength(tableIdentifierPrefix);

    if (byteCount < INSTALLER_TABLE_IDENTIFIER_BYTES)
    {
        return BOOLEAN_FALSE;
    }
    for (index = 0UL; index < prefixLength; index += 1UL)
    {
        if (bytes[index] != (Unsigned8)tableIdentifierPrefix[index])
        {
            return BOOLEAN_FALSE;
        }
    }
    /* Two digits after it. Checked rather than skipped, because the prefix
       alone is six characters of a string that could occur in anything. */
    return (Boolean)(bytes[prefixLength] >= (Unsigned8)'0' && bytes[prefixLength] <= (Unsigned8)'9' &&
                     bytes[prefixLength + 1UL] >= (Unsigned8)'0' &&
                     bytes[prefixLength + 1UL] <= (Unsigned8)'9');
}

InstallerReadResult installerFindOffsetTable(const Unsigned8 *head, MemorySize headSize,
                                             Unsigned64 *tableOffsetInBytes)
{
    Unsigned32 mark;
    Unsigned32 pointer;
    Unsigned32 complement;

    if (headSize < INSTALLER_LOADER_HEADER_OFFSET + 12UL)
    {
        return INSTALLER_READ_TRUNCATED;
    }

    /* Inline first: the older loaders put the table itself here, and it says so
       in its own identifier. */
    if (looksLikeTableIdentifier(&head[INSTALLER_LOADER_HEADER_OFFSET],
                                 headSize - INSTALLER_LOADER_HEADER_OFFSET))
    {
        *tableOffsetInBytes = (Unsigned64)INSTALLER_LOADER_HEADER_OFFSET;
        return INSTALLER_READ_OK;
    }

    mark = readUnsigned32(head, INSTALLER_LOADER_HEADER_OFFSET);
    if (mark != (Unsigned32)INSTALLER_LOADER_POINTER_MARK)
    {
        return INSTALLER_READ_NOT_AN_INSTALLER;
    }

    /* The pointer is written twice, the second time inverted. Two words that
       are each other's complement do not happen by accident, so this is what
       separates a real loader from four bytes that happen to match. */
    pointer = readUnsigned32(head, INSTALLER_LOADER_HEADER_OFFSET + 4UL);
    complement = readUnsigned32(head, INSTALLER_LOADER_HEADER_OFFSET + 8UL);
    if (pointer != ~complement)
    {
        return INSTALLER_READ_POINTER_INCONSISTENT;
    }

    *tableOffsetInBytes = (Unsigned64)pointer;
    return INSTALLER_READ_OK;
}

Unsigned64 installerFindTableMarker(const Unsigned8 *bytes, MemorySize byteCount,
                                    Unsigned64 chunkOffset)
{
    MemorySize at;

    if (byteCount < INSTALLER_TABLE_IDENTIFIER_BYTES)
    {
        return (Unsigned64)INSTALLER_MARKER_NOT_FOUND;
    }
    for (at = 0UL; at + INSTALLER_TABLE_IDENTIFIER_BYTES <= byteCount; at += 1UL)
    {
        /* The same test the inline case uses, digits and all, rather than the
           six letters alone. Six letters occur in a gigabyte of compressed data
           by chance; six letters followed by two digits do not. */
        if (looksLikeTableIdentifier(&bytes[at], byteCount - at))
        {
            return chunkOffset + (Unsigned64)at;
        }
    }
    return (Unsigned64)INSTALLER_MARKER_NOT_FOUND;
}

Unsigned64 installerFindVersionMarker(const Unsigned8 *bytes, MemorySize byteCount,
                                      Unsigned64 chunkOffset)
{
    MemorySize prefixLength = stringLength(versionStringPrefix);
    MemorySize at;

    if (byteCount < prefixLength)
    {
        return (Unsigned64)INSTALLER_MARKER_NOT_FOUND;
    }
    for (at = 0UL; at + prefixLength <= byteCount; at += 1UL)
    {
        MemorySize index;
        Boolean matches = BOOLEAN_TRUE;

        for (index = 0UL; index < prefixLength; index += 1UL)
        {
            if (bytes[at + index] != (Unsigned8)versionStringPrefix[index])
            {
                matches = BOOLEAN_FALSE;
                break;
            }
        }
        if (matches)
        {
            return chunkOffset + (Unsigned64)at;
        }
    }
    return (Unsigned64)INSTALLER_MARKER_NOT_FOUND;
}

Unsigned64 installerFindMark(const Unsigned8 *bytes, MemorySize byteCount, Unsigned64 chunkOffset,
                             const char *mark, MemorySize markLength)
{
    MemorySize at;

    if (markLength == 0UL || byteCount < markLength)
    {
        return (Unsigned64)INSTALLER_MARKER_NOT_FOUND;
    }
    for (at = 0UL; at + markLength <= byteCount; at += 1UL)
    {
        MemorySize index;

        /* The first byte decides almost every position, so the inner loop is
           entered once in two hundred and fifty six rather than every time. */
        if (bytes[at] != (Unsigned8)mark[0])
        {
            continue;
        }
        for (index = 1UL; index < markLength; index += 1UL)
        {
            if (bytes[at + index] != (Unsigned8)mark[index])
            {
                break;
            }
        }
        if (index == markLength)
        {
            return chunkOffset + (Unsigned64)at;
        }
    }
    return (Unsigned64)INSTALLER_MARKER_NOT_FOUND;
}

InstallerReadResult installerReadOffsetTable(const Unsigned8 *bytes, MemorySize byteCount,
                                             Unsigned64 tableOffsetInBytes,
                                             InstallerOffsetTable *table)
{
    Unsigned32 wordCount;
    MemorySize index;

    if (byteCount < INSTALLER_TABLE_IDENTIFIER_BYTES + ((MemorySize)SMALLEST_WORD_COUNT + 1UL) * 4UL)
    {
        return INSTALLER_READ_TRUNCATED;
    }
    if (!looksLikeTableIdentifier(bytes, byteCount))
    {
        return INSTALLER_READ_NOT_AN_INSTALLER;
    }

    /* Every length in turn, shortest first. The checksum decides. */
    for (wordCount = SMALLEST_WORD_COUNT; wordCount <= (Unsigned32)INSTALLER_TABLE_WORD_LIMIT;
         wordCount++)
    {
        MemorySize checksumAt = INSTALLER_TABLE_IDENTIFIER_BYTES + (MemorySize)wordCount * 4UL;

        if (checksumAt + 4UL > byteCount)
        {
            break;
        }
        if (checksumCrc32(bytes, checksumAt) != readUnsigned32(bytes, checksumAt))
        {
            continue;
        }

        table->tableRevision = (Unsigned32)((bytes[6] - (Unsigned8)'0') * 10) +
                               (Unsigned32)(bytes[7] - (Unsigned8)'0');
        table->tableOffsetInBytes = tableOffsetInBytes;
        table->wordCount = wordCount;
        for (index = 0UL; index < (MemorySize)wordCount; index += 1UL)
        {
            table->words[index] =
                readUnsigned32(bytes, INSTALLER_TABLE_IDENTIFIER_BYTES + index * 4UL);
        }
        table->totalSizeInBytes = table->words[0];
        table->headerOffsetInBytes = table->words[wordCount - TRAILING_OFFSET_WORDS];
        table->dataOffsetInBytes = table->words[wordCount - 1U];
        return INSTALLER_READ_OK;
    }

    return INSTALLER_READ_UNKNOWN_LAYOUT;
}

InstallerReadResult installerReadVersionString(const Unsigned8 *bytes, MemorySize byteCount,
                                               char *destination, MemorySize capacity)
{
    MemorySize prefixLength = stringLength(versionStringPrefix);
    MemorySize index;
    MemorySize written = 0UL;

    if (capacity == 0UL)
    {
        return INSTALLER_READ_TRUNCATED;
    }
    destination[0] = '\0';
    if (byteCount < prefixLength)
    {
        return INSTALLER_READ_TRUNCATED;
    }
    for (index = 0UL; index < prefixLength; index += 1UL)
    {
        if (bytes[index] != (Unsigned8)versionStringPrefix[index])
        {
            return INSTALLER_READ_NOT_A_VERSION_STRING;
        }
    }

    /* Up to the terminator that pads the field out. Anything unprintable ends
       it too: this is going into a log, and a control character in a log line
       is a log line nobody can read. */
    for (index = 0UL; index < byteCount && index < INSTALLER_VERSION_STRING_BYTES; index += 1UL)
    {
        char character = (char)bytes[index];

        if (character < ' ' || character > '~')
        {
            break;
        }
        if (written + 1UL >= capacity)
        {
            break;
        }
        destination[written] = character;
        written += 1UL;
    }
    destination[written] = '\0';
    return INSTALLER_READ_OK;
}
