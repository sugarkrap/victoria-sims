/* Walking a RAR archive's block chain.

   The disc's TSData.exe is a program with 2.7 gibibytes appended past it, and
   what is appended starts Rar!. A package mark turned up a kibibyte into that,
   which is where a file's data begins if the file was stored rather than
   compressed — so what this reader has to get right, above everything, is where
   each entry's data starts and whether it was packed at all.

   The archives here are built rather than captured. The lengths are written
   from the block layout rather than taken from the reader, so a reader that
   miscounts a field disagrees with them instead of agreeing with itself. */

#include <stdio.h>

#include "utils/assert.h"
#include "utils/strings.h"
#include "victoria/archiveReader.h"

static Integer32 failureCount = 0;

static Unsigned8 archive[1024];

static void writeUnsigned16(MemorySize offset, Unsigned16 value)
{
    archive[offset] = (Unsigned8)(value & 0xFFU);
    archive[offset + 1UL] = (Unsigned8)((value >> 8) & 0xFFU);
}

static void writeUnsigned32(MemorySize offset, Unsigned32 value)
{
    archive[offset] = (Unsigned8)(value & 0xFFUL);
    archive[offset + 1UL] = (Unsigned8)((value >> 8) & 0xFFUL);
    archive[offset + 2UL] = (Unsigned8)((value >> 16) & 0xFFUL);
    archive[offset + 3UL] = (Unsigned8)((value >> 24) & 0xFFUL);
}

static void clearArchive(void)
{
    MemorySize index;

    for (index = 0UL; index < sizeof(archive); index += 1UL)
    {
        archive[index] = 0U;
    }
}

/* A file header block at the given offset. Returns its header length. */
static MemorySize buildFileHeader(MemorySize at, const char *name, Unsigned16 flags,
                                  Unsigned32 packedSize, Unsigned32 unpackedSize, Unsigned8 method)
{
    MemorySize nameLength = stringLength(name);
    MemorySize nameAt = 32UL + (((flags & 0x0100U) != 0U) ? 8UL : 0UL);
    MemorySize headerSize = nameAt + nameLength;
    MemorySize index;

    archive[at + 2UL] = 0x74U;
    writeUnsigned16(at + 3UL, flags);
    writeUnsigned16(at + 5UL, (Unsigned16)headerSize);
    writeUnsigned32(at + 7UL, packedSize);
    writeUnsigned32(at + 11UL, unpackedSize);
    archive[at + 24UL] = 20U;
    archive[at + 25UL] = method;
    writeUnsigned16(at + 26UL, (Unsigned16)nameLength);
    for (index = 0UL; index < nameLength; index += 1UL)
    {
        archive[at + nameAt + index] = (Unsigned8)name[index];
    }
    return headerSize;
}

int main(void)
{
    ArchiveEntry entry;
    Unsigned64 firstBlock = 0ULL;

    printf("-- the mark --\n");
    {
        static const Unsigned8 fourth[7] = { 'R', 'a', 'r', '!', 0x1AU, 0x07U, 0x00U };
        static const Unsigned8 fifth[8] = { 'R', 'a', 'r', '!', 0x1AU, 0x07U, 0x01U, 0x00U };
        static const Unsigned8 neither[7] = { 'M', 'Z', 'P', 0x00U, 0x00U, 0x00U, 0x00U };

        checkThat(&failureCount, "the fourth generation is recognised",
                  archiveReadMark(fourth, sizeof(fourth), 0x1000ULL, &firstBlock) ==
                      ARCHIVE_READ_OK);
        /* Past the mark, in the file's own addresses rather than the buffer's. */
        checkThat(&failureCount, "and the first block is past it", firstBlock == 0x1007ULL);
        /* The fifth generation shares six bytes with the fourth and nothing
           else. Reading it as the fourth would produce block lengths that walk
           into the middle of things, which reads as a corrupt archive. */
        checkThat(&failureCount, "the fifth is refused by name",
                  archiveReadMark(fifth, sizeof(fifth), 0ULL, &firstBlock) ==
                      ARCHIVE_READ_NEWER_GENERATION);
        checkThat(&failureCount, "and something else is not an archive",
                  archiveReadMark(neither, sizeof(neither), 0ULL, &firstBlock) ==
                      ARCHIVE_READ_NOT_AN_ARCHIVE);
    }

    printf("\n-- a block that is not a file --\n");
    {
        clearArchive();
        archive[2] = (Unsigned8)ARCHIVE_BLOCK_ARCHIVE_HEADER;
        writeUnsigned16(5UL, 13U);

        checkThat(&failureCount, "is reported as such rather than refused",
                  archiveReadBlock(archive, sizeof(archive), 0x100ULL, &entry) ==
                      ARCHIVE_READ_NOT_A_FILE);
        checkThat(&failureCount, "and still says where the next block is",
                  entry.nextBlockOffsetInBytes == 0x10DULL);

        /* One carrying data — a comment, a recovery record — whose length must
           be counted too, or the walk lands in the middle of it. */
        clearArchive();
        archive[2] = 0x7AU;
        writeUnsigned16(3UL, 0x8000U);
        writeUnsigned16(5UL, 20U);
        writeUnsigned32(7UL, 0x500UL);
        checkThat(&failureCount, "a block with data is stepped over entirely",
                  archiveReadBlock(archive, sizeof(archive), 0ULL, &entry) ==
                          ARCHIVE_READ_NOT_A_FILE &&
                      entry.nextBlockOffsetInBytes == 0x514ULL);
    }

    printf("\n-- a stored entry --\n");
    {
        MemorySize headerSize;

        clearArchive();
        headerSize = buildFileHeader(0UL, "TSData/Res/Sims3D/Sims01.package", 0x8000U, 0x2000UL,
                                     0x2000UL, (Unsigned8)ARCHIVE_METHOD_STORED);

        checkThat(&failureCount, "reads",
                  archiveReadBlock(archive, sizeof(archive), 0xCE14ULL, &entry) == ARCHIVE_READ_OK);
        checkThat(&failureCount, "with its name",
                  stringEquals(entry.name, "TSData/Res/Sims3D/Sims01.package"));
        checkThat(&failureCount, "stored rather than packed",
                  entry.method == (Unsigned8)ARCHIVE_METHOD_STORED);
        /* The whole point of the exercise: a stored entry's data is a range of
           the file, and this is the range. */
        checkThat(&failureCount, "its data starting where the header ends",
                  entry.dataOffsetInBytes == 0xCE14ULL + (Unsigned64)headerSize);
        checkThat(&failureCount, "with packed and unpacked sizes that agree",
                  entry.packedSizeInBytes == 0x2000ULL &&
                      entry.unpackedSizeInBytes == 0x2000ULL);
        checkThat(&failureCount, "and the next block past its data",
                  entry.nextBlockOffsetInBytes == entry.dataOffsetInBytes + 0x2000ULL);
    }

    printf("\n-- a packed entry, and a name written the other way --\n");
    {
        clearArchive();
        buildFileHeader(0UL, "TSData\\Res\\Sims3D\\Sims02.package", 0x8000U, 0x1000UL, 0x4000UL, 0x35U);

        checkThat(&failureCount, "reads",
                  archiveReadBlock(archive, sizeof(archive), 0ULL, &entry) == ARCHIVE_READ_OK);
        checkThat(&failureCount, "packed rather than stored",
                  entry.method != (Unsigned8)ARCHIVE_METHOD_STORED);
        checkThat(&failureCount, "with sizes that differ, which is what says so",
                  entry.packedSizeInBytes == 0x1000ULL &&
                      entry.unpackedSizeInBytes == 0x4000ULL);
        /* An archive written on Windows separates with backslashes and the
           catalogue everything else here uses does not. */
        checkThat(&failureCount, "and its separators turned the right way",
                  stringEquals(entry.name, "TSData/Res/Sims3D/Sims02.package"));
    }

    printf("\n-- an entry too big for a word --\n");
    {
        clearArchive();
        buildFileHeader(0UL, "big.package", (Unsigned16)(0x8000U | 0x0100U), 0x10UL, 0x20UL,
                        (Unsigned8)ARCHIVE_METHOD_STORED);
        /* The high halves, which sit between the fixed fields and the name. */
        writeUnsigned32(32UL, 2UL);
        writeUnsigned32(36UL, 3UL);

        checkThat(&failureCount, "reads",
                  archiveReadBlock(archive, sizeof(archive), 0ULL, &entry) == ARCHIVE_READ_OK);
        checkThat(&failureCount, "joining both halves of each size",
                  entry.packedSizeInBytes == 0x200000010ULL &&
                      entry.unpackedSizeInBytes == 0x300000020ULL);
        /* And the name is past them. A reader that missed the high halves would
           read the last four bytes of a number as the first four of a path. */
        checkThat(&failureCount, "and finding the name past them",
                  stringEquals(entry.name, "big.package"));
    }

    printf("\n-- refusing what it should --\n");
    {
        clearArchive();
        archive[2] = 0x74U;
        writeUnsigned16(5UL, 0U);
        /* A walk that believed this would advance by nothing and read the same
           bytes for ever. */
        checkThat(&failureCount, "a header shorter than a header",
                  archiveReadBlock(archive, sizeof(archive), 0ULL, &entry) == ARCHIVE_READ_BAD_BLOCK);

        clearArchive();
        archive[2] = 0x74U;
        writeUnsigned16(5UL, 4096U);
        writeUnsigned16(26UL, (Unsigned16)ARCHIVE_NAME_LIMIT);
        checkThat(&failureCount, "a name longer than this reader keeps",
                  archiveReadBlock(archive, sizeof(archive), 0ULL, &entry) ==
                      ARCHIVE_READ_NAME_TOO_LONG);

        clearArchive();
        archive[2] = 0x74U;
        writeUnsigned16(5UL, 40U);
        checkThat(&failureCount, "and too few bytes to hold one",
                  archiveReadBlock(archive, 8UL, 0ULL, &entry) == ARCHIVE_READ_TRUNCATED);
    }

    return checkSummarize(failureCount, "archive");
}
