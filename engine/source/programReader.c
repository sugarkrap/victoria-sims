#include "victoria/programReader.h"

/* The header the DOS stub points at, and the two fields of it that matter.
 *
 * Counted from the specification rather than remembered: two bytes of machine,
 * then the section count, then twelve bytes of symbol table fields nobody has
 * used since COFF, then the size of the optional header. Both of these were
 * wrong at first — and the hand-written test agreed with them, because it was
 * written from the same wrong memory. What caught it was a fixture built
 * independently from the field list. */
#define HEADER_MARK_BYTES 4UL
#define COFF_HEADER_BYTES 20UL
#define OFFSET_SECTION_COUNT 2UL
#define OFFSET_OPTIONAL_HEADER_SIZE 16UL

/* One section's entry, and where its file range sits inside it. */
#define SECTION_ENTRY_BYTES 40UL
#define OFFSET_RAW_SIZE 16UL
#define OFFSET_RAW_OFFSET 20UL

const char *programReadResultGetName(ProgramReadResult result)
{
    switch (result)
    {
    case PROGRAM_READ_OK:
        return "read";
    case PROGRAM_READ_NOT_A_PROGRAM:
        return "not a Windows program";
    case PROGRAM_READ_TRUNCATED:
        return "not enough of the front to hold a header";
    case PROGRAM_READ_TOO_MANY_SECTIONS:
        return "more sections than a program has";
    case PROGRAM_READ_SECTION_OUT_OF_RANGE:
        return "a section reaching past the end of the file";
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

ProgramReadResult programReadLayout(const Unsigned8 *bytes, MemorySize byteCount,
                                    Unsigned64 fileSizeInBytes, ProgramLayout *layout)
{
    Unsigned32 headerAt;
    Unsigned16 sectionCount;
    Unsigned16 optionalHeaderSize;
    MemorySize sectionTableAt;
    Unsigned32 which;
    Unsigned64 furthest = 0ULL;

    layout->sectionCount = 0U;
    layout->endOfProgramInBytes = 0ULL;

    if (byteCount < PROGRAM_HEADER_POINTER_OFFSET + 4UL)
    {
        return PROGRAM_READ_TRUNCATED;
    }
    /* MZ, and MZP too: Delphi writes the same header with a third character,
       and it is still a program. */
    if (bytes[0] != (Unsigned8)'M' || bytes[1] != (Unsigned8)'Z')
    {
        return PROGRAM_READ_NOT_A_PROGRAM;
    }

    headerAt = readUnsigned32(bytes, PROGRAM_HEADER_POINTER_OFFSET);
    if ((MemorySize)headerAt + HEADER_MARK_BYTES + COFF_HEADER_BYTES > byteCount)
    {
        return PROGRAM_READ_TRUNCATED;
    }
    if (bytes[headerAt] != (Unsigned8)'P' || bytes[headerAt + 1UL] != (Unsigned8)'E' ||
        bytes[headerAt + 2UL] != 0U || bytes[headerAt + 3UL] != 0U)
    {
        return PROGRAM_READ_NOT_A_PROGRAM;
    }

    sectionCount = readUnsigned16(bytes, (MemorySize)headerAt + HEADER_MARK_BYTES + OFFSET_SECTION_COUNT);
    optionalHeaderSize =
        readUnsigned16(bytes, (MemorySize)headerAt + HEADER_MARK_BYTES + OFFSET_OPTIONAL_HEADER_SIZE);
    if (sectionCount == 0U || sectionCount > (Unsigned16)PROGRAM_SECTION_LIMIT)
    {
        return PROGRAM_READ_TOO_MANY_SECTIONS;
    }

    sectionTableAt = (MemorySize)headerAt + HEADER_MARK_BYTES + COFF_HEADER_BYTES +
                     (MemorySize)optionalHeaderSize;
    if (sectionTableAt + (MemorySize)sectionCount * SECTION_ENTRY_BYTES > byteCount)
    {
        return PROGRAM_READ_TRUNCATED;
    }

    for (which = 0U; which < (Unsigned32)sectionCount; which++)
    {
        MemorySize entryAt = sectionTableAt + (MemorySize)which * SECTION_ENTRY_BYTES;
        Unsigned64 rawSize = (Unsigned64)readUnsigned32(bytes, entryAt + OFFSET_RAW_SIZE);
        Unsigned64 rawOffset = (Unsigned64)readUnsigned32(bytes, entryAt + OFFSET_RAW_OFFSET);
        Unsigned64 endsAt = rawOffset + rawSize;

        /* A section with no bytes in the file occupies none of it. Uninitialised
           data is the common case and its offset is meaningless. */
        if (rawSize == 0ULL)
        {
            continue;
        }
        if (endsAt < rawOffset || endsAt > fileSizeInBytes)
        {
            return PROGRAM_READ_SECTION_OUT_OF_RANGE;
        }
        if (endsAt > furthest)
        {
            furthest = endsAt;
        }
    }

    layout->sectionCount = (Unsigned32)sectionCount;
    layout->endOfProgramInBytes = furthest;
    return PROGRAM_READ_OK;
}
