#ifndef VICTORIA_PROGRAM_READER_HEADER
#define VICTORIA_PROGRAM_READER_HEADER

#include "victoria/coreTypes.h"

/* Where a Windows program stops and whatever was appended to it begins.
 *
 * A file that carries an archive by pretending to be a program has to start
 * with a real program, because that is what the operating system runs. So the
 * payload is past the end of it — and the program says where its own end is, in
 * the section table it needs anyway.
 *
 * The disc this was written for has a 2.7 gibibyte file of exactly this shape,
 * and searching the front of it for known marks found nothing in thirty three
 * mebibytes. That is not evidence that there is nothing there; it is evidence
 * that the thing is not at the front. This says where the front stops.
 *
 * Only the layout is read. Nothing here loads, relocates or runs anything: the
 * section table is a list of file ranges, and the largest end of any of them is
 * the answer. */

/* The DOS header keeps the real header's position here. */
#define PROGRAM_HEADER_POINTER_OFFSET 0x3CUL

/* Bytes of the front a caller needs to supply. Enough for the DOS stub, the
   header it points at, and a section table far larger than anything real. */
#define PROGRAM_LAYOUT_BYTES_NEEDED 4096UL

/* Refused above this. A program with more sections than this is not one. */
#define PROGRAM_SECTION_LIMIT 96U

typedef enum ProgramReadResult
{
    PROGRAM_READ_OK = 0,
    PROGRAM_READ_NOT_A_PROGRAM,
    PROGRAM_READ_TRUNCATED,
    PROGRAM_READ_TOO_MANY_SECTIONS,
    /* A section claiming bytes the file does not have. */
    PROGRAM_READ_SECTION_OUT_OF_RANGE,
    PROGRAM_READ_RESULT_COUNT
} ProgramReadResult;

const char *programReadResultGetName(ProgramReadResult result);

typedef struct ProgramLayout
{
    Unsigned32 sectionCount;
    /* Where the last section's bytes end, which is where anything appended
       starts. */
    Unsigned64 endOfProgramInBytes;
} ProgramLayout;

/* Reads the layout from the front of the file.
 *
 * fileSizeInBytes is what the section table is checked against. A ceiling
 * invented here would have to be generous enough to allow anything real, which
 * makes it too generous to catch a misread; the file's own length is exact and
 * the caller always knows it. */
ProgramReadResult programReadLayout(const Unsigned8 *bytes, MemorySize byteCount,
                                    Unsigned64 fileSizeInBytes, ProgramLayout *layout);

#endif
