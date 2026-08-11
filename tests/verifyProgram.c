
#include <stdio.h>

#include "utils/assert.h"
#include "victoria/programReader.h"

static Integer32 failureCount = 0;

static Unsigned8 program[PROGRAM_LAYOUT_BYTES_NEEDED];

static void writeUnsigned16(MemorySize offset, Unsigned16 value)
{
    program[offset] = (Unsigned8)(value & 0xFFU);
    program[offset + 1UL] = (Unsigned8)((value >> 8) & 0xFFU);
}

static void writeUnsigned32(MemorySize offset, Unsigned32 value)
{
    program[offset] = (Unsigned8)(value & 0xFFUL);
    program[offset + 1UL] = (Unsigned8)((value >> 8) & 0xFFUL);
    program[offset + 2UL] = (Unsigned8)((value >> 16) & 0xFFUL);
    program[offset + 3UL] = (Unsigned8)((value >> 24) & 0xFFUL);
}

#define FILE_SIZE 0x100000ULL

#define HEADER_AT 0x100UL
#define OPTIONAL_HEADER_BYTES 224UL
#define SECTION_TABLE_AT (HEADER_AT + 4UL + 20UL + OPTIONAL_HEADER_BYTES)

static void buildProgram(const Unsigned32 *offsets, const Unsigned32 *sizes, Unsigned16 sectionCount)
{
    MemorySize index;

    for (index = 0UL; index < sizeof(program); index += 1UL)
    {
        program[index] = 0U;
    }
    program[0] = (Unsigned8)'M';
    program[1] = (Unsigned8)'Z';
    program[2] = (Unsigned8)'P';
    writeUnsigned32(PROGRAM_HEADER_POINTER_OFFSET, (Unsigned32)HEADER_AT);
    program[HEADER_AT] = (Unsigned8)'P';
    program[HEADER_AT + 1UL] = (Unsigned8)'E';
    writeUnsigned16(HEADER_AT + 4UL + 2UL, sectionCount);
    writeUnsigned16(HEADER_AT + 4UL + 16UL, (Unsigned16)OPTIONAL_HEADER_BYTES);

    for (index = 0UL; index < (MemorySize)sectionCount; index += 1UL)
    {
        MemorySize entryAt = SECTION_TABLE_AT + index * 40UL;

        writeUnsigned32(entryAt + 16UL, sizes[index]);
        writeUnsigned32(entryAt + 20UL, offsets[index]);
    }
}

int main(void)
{
    ProgramLayout layout;

    printf("-- where the program ends --\n");
    {
        static const Unsigned32 offsets[3] = { 0x400UL, 0x1800UL, 0x1000UL };
        static const Unsigned32 sizes[3] = { 0x200UL, 0x600UL, 0x400UL };

        buildProgram(offsets, sizes, 3U);
        checkThat(&failureCount, "the layout reads",
                  programReadLayout(program, sizeof(program), FILE_SIZE, &layout) == PROGRAM_READ_OK);
        checkThat(&failureCount, "counting the sections", layout.sectionCount == 3U);
        checkThat(&failureCount, "and ending at the furthest, not the last",
                  layout.endOfProgramInBytes == 0x1E00ULL);
    }

    printf("\n-- a section that occupies no file --\n");
    {
        static const Unsigned32 offsets[3] = { 0x400UL, 0x900UL, 0x8000000UL };
        static const Unsigned32 sizes[3] = { 0x200UL, 0x100UL, 0UL };

        buildProgram(offsets, sizes, 3U);
        checkThat(&failureCount, "it is skipped rather than counted",
                  programReadLayout(program, sizeof(program), FILE_SIZE, &layout) == PROGRAM_READ_OK &&
                      layout.endOfProgramInBytes == 0xA00ULL);
    }

    printf("\n-- a range that wraps --\n");
    {
        static const Unsigned32 offsets[1] = { 0xFFFFFF00UL };
        static const Unsigned32 sizes[1] = { 0x200UL };

        buildProgram(offsets, sizes, 1U);
        checkThat(&failureCount, "is refused rather than wrapping to a small one",
                  programReadLayout(program, sizeof(program), FILE_SIZE, &layout) ==
                      PROGRAM_READ_SECTION_OUT_OF_RANGE);
    }

    printf("\n-- refusing what it should --\n");
    {
        static const Unsigned32 offsets[1] = { 0x400UL };
        static const Unsigned32 sizes[1] = { 0x200UL };

        buildProgram(offsets, sizes, 1U);
        program[1] = (Unsigned8)'X';
        checkThat(&failureCount, "something that is not a program",
                  programReadLayout(program, sizeof(program), FILE_SIZE, &layout) == PROGRAM_READ_NOT_A_PROGRAM);

        buildProgram(offsets, sizes, 1U);
        program[HEADER_AT] = (Unsigned8)'N';
        checkThat(&failureCount, "a DOS stub pointing at something that is not a header",
                  programReadLayout(program, sizeof(program), FILE_SIZE, &layout) == PROGRAM_READ_NOT_A_PROGRAM);

        buildProgram(offsets, sizes, 1U);
        writeUnsigned16(HEADER_AT + 4UL + 2UL, 4096U);
        checkThat(&failureCount, "more sections than a program has",
                  programReadLayout(program, sizeof(program), FILE_SIZE, &layout) ==
                      PROGRAM_READ_TOO_MANY_SECTIONS);

        buildProgram(offsets, sizes, 1U);
        writeUnsigned32(SECTION_TABLE_AT + 20UL, 0x400UL);
        writeUnsigned32(SECTION_TABLE_AT + 16UL, (Unsigned32)FILE_SIZE);
        checkThat(&failureCount, "a section reaching past the end of the file",
                  programReadLayout(program, sizeof(program), FILE_SIZE, &layout) ==
                      PROGRAM_READ_SECTION_OUT_OF_RANGE);

        buildProgram(offsets, sizes, 1U);
        checkThat(&failureCount, "and too little of the front to hold a header",
                  programReadLayout(program, 8UL, FILE_SIZE, &layout) == PROGRAM_READ_TRUNCATED);
        checkThat(&failureCount, "or to hold the section table it points at",
                  programReadLayout(program, HEADER_AT + 32UL, FILE_SIZE, &layout) == PROGRAM_READ_TRUNCATED);
    }

    return checkSummarize(failureCount, "program");
}
