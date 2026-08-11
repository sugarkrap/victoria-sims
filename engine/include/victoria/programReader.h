#ifndef VICTORIA_PROGRAM_READER_HEADER
#define VICTORIA_PROGRAM_READER_HEADER

#include "victoria/coreTypes.h"

#define PROGRAM_HEADER_POINTER_OFFSET 0x3CUL

#define PROGRAM_LAYOUT_BYTES_NEEDED 4096UL

#define PROGRAM_SECTION_LIMIT 96U

typedef enum ProgramReadResult
{
    PROGRAM_READ_OK = 0,
    PROGRAM_READ_NOT_A_PROGRAM,
    PROGRAM_READ_TRUNCATED,
    PROGRAM_READ_TOO_MANY_SECTIONS,
    PROGRAM_READ_SECTION_OUT_OF_RANGE,
    PROGRAM_READ_RESULT_COUNT
} ProgramReadResult;

const char *programReadResultGetName(ProgramReadResult result);

typedef struct ProgramLayout
{
    Unsigned32 sectionCount;
    Unsigned64 endOfProgramInBytes;
} ProgramLayout;

ProgramReadResult programReadLayout(const Unsigned8 *bytes, MemorySize byteCount,
                                    Unsigned64 fileSizeInBytes, ProgramLayout *layout);

#endif
