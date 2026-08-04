/* Where a Windows program stops.

   A file carrying an archive by pretending to be a program starts with a real
   program, so the payload is past the end of it — and the program's own section
   table says where that is. The disc this was written for has a 2.7 gibibyte
   file of that shape whose front thirty three mebibytes hold no mark anybody
   recognises, which is a reason to stop searching the front.

   The programs here are built rather than captured, because no installer may
   live in this repository. Building them is also the only way to test the
   answers that matter: a section table whose entries are deliberately out of
   order, and one carrying a section that occupies no file at all. */

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

/* What the section table is measured against. Every program built below fits
   inside this, and the section deliberately built to overrun it does not. */
#define FILE_SIZE 0x100000ULL

#define HEADER_AT 0x100UL
#define OPTIONAL_HEADER_BYTES 224UL
#define SECTION_TABLE_AT (HEADER_AT + 4UL + 20UL + OPTIONAL_HEADER_BYTES)

/* A program with the given sections, each a file offset and a length. */
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
    /* Two bytes of machine, then the section count; twelve bytes of symbol
       table fields, then the optional header's size. Written here from the
       field list rather than from the reader, which is the only way this test
       can disagree with the reader when the reader is wrong. */
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
        /* Deliberately not in order. Nothing requires a section table to be
           sorted, and a reader that took the last entry rather than the
           furthest would answer 0x1400 here — putting the payload's start
           inside the program. */
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
        /* Uninitialised data has a length in memory and none on disc, and its
           file offset means nothing. Counting it would put the end of the
           program at an address that holds nothing. */
        static const Unsigned32 offsets[3] = { 0x400UL, 0x900UL, 0x8000000UL };
        static const Unsigned32 sizes[3] = { 0x200UL, 0x100UL, 0UL };

        buildProgram(offsets, sizes, 3U);
        checkThat(&failureCount, "it is skipped rather than counted",
                  programReadLayout(program, sizeof(program), FILE_SIZE, &layout) == PROGRAM_READ_OK &&
                      layout.endOfProgramInBytes == 0xA00ULL);
    }

    printf("\n-- a range that wraps --\n");
    {
        /* Offset and size that sum past what a thirty-two bit number holds.
           Added as sixty-four bit values here, so the sum is a real address
           rather than a small one, and it is past the file. */
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
        /* Enough for the DOS header but not for the section table the header
           points past, which is a different truncation and must not be read as
           a program with no sections. */
        checkThat(&failureCount, "or to hold the section table it points at",
                  programReadLayout(program, HEADER_AT + 32UL, FILE_SIZE, &layout) == PROGRAM_READ_TRUNCATED);
    }

    return checkSummarize(failureCount, "program");
}
