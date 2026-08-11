
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
        checkThat(&failureCount, "and the first block is past it", firstBlock == 0x1007ULL);
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
        checkThat(&failureCount, "and its separators turned the right way",
                  stringEquals(entry.name, "TSData/Res/Sims3D/Sims02.package"));
    }

    printf("\n-- an entry too big for a word --\n");
    {
        clearArchive();
        buildFileHeader(0UL, "big.package", (Unsigned16)(0x8000U | 0x0100U), 0x10UL, 0x20UL,
                        (Unsigned8)ARCHIVE_METHOD_STORED);
        writeUnsigned32(32UL, 2UL);
        writeUnsigned32(36UL, 3UL);

        checkThat(&failureCount, "reads",
                  archiveReadBlock(archive, sizeof(archive), 0ULL, &entry) == ARCHIVE_READ_OK);
        checkThat(&failureCount, "joining both halves of each size",
                  entry.packedSizeInBytes == 0x200000010ULL &&
                      entry.unpackedSizeInBytes == 0x300000020ULL);
        checkThat(&failureCount, "and finding the name past them",
                  stringEquals(entry.name, "big.package"));
    }

    printf("\n-- refusing what it should --\n");
    {
        clearArchive();
        archive[2] = 0x74U;
        writeUnsigned16(5UL, 0U);
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
