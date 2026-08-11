
#include <stdio.h>

#include "utils/assert.h"
#include "utils/checksum.h"
#include "utils/strings.h"
#include "victoria/installerReader.h"

static Integer32 failureCount = 0;

static Unsigned8 fileHead[256];

static void writeUnsigned32(Unsigned8 *bytes, MemorySize offset, Unsigned32 value)
{
    bytes[offset] = (Unsigned8)(value & 0xFFUL);
    bytes[offset + 1UL] = (Unsigned8)((value >> 8) & 0xFFUL);
    bytes[offset + 2UL] = (Unsigned8)((value >> 16) & 0xFFUL);
    bytes[offset + 3UL] = (Unsigned8)((value >> 24) & 0xFFUL);
}

static MemorySize buildTable(Unsigned8 *bytes, const char *identifier, const Unsigned32 *words,
                             MemorySize wordCount)
{
    MemorySize index;
    MemorySize checksumAt;

    for (index = 0UL; index < INSTALLER_TABLE_IDENTIFIER_BYTES; index += 1UL)
    {
        bytes[index] = (Unsigned8)identifier[index];
    }
    for (index = 0UL; index < wordCount; index += 1UL)
    {
        writeUnsigned32(bytes, INSTALLER_TABLE_IDENTIFIER_BYTES + index * 4UL, words[index]);
    }
    checksumAt = INSTALLER_TABLE_IDENTIFIER_BYTES + wordCount * 4UL;
    writeUnsigned32(bytes, checksumAt, checksumCrc32(bytes, checksumAt));
    return checksumAt + 4UL;
}

static void clearHead(void)
{
    MemorySize index;

    for (index = 0UL; index < sizeof(fileHead); index += 1UL)
    {
        fileHead[index] = 0U;
    }
    fileHead[0] = (Unsigned8)'M';
    fileHead[1] = (Unsigned8)'Z';
    fileHead[2] = (Unsigned8)'P';
}

int main(void)
{
    printf("-- the checksum itself --\n");
    {
        static const Unsigned8 digits[9] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };

        checkThat(&failureCount, "matches the published check value",
                  checksumCrc32(digits, sizeof(digits)) == 0xCBF43926UL);
        checkThat(&failureCount, "and an empty run checksums to zero",
                  checksumCrc32(digits, 0UL) == 0UL);
    }

    printf("\n-- a loader that keeps the table at 0x30 --\n");
    {
        static const Unsigned32 words[6] = { 0xAABBCCDDUL, 0x1000UL, 0x2000UL,
                                             0x3000UL,    0x4000UL, 0x5000UL };
        Unsigned64 tableOffset = 0U;
        InstallerOffsetTable table;

        clearHead();
        buildTable(&fileHead[INSTALLER_LOADER_HEADER_OFFSET], "rDlPtS02\207eVx", words, 6UL);

        checkThat(&failureCount, "the table is found where it lies",
                  installerFindOffsetTable(fileHead, sizeof(fileHead), &tableOffset) ==
                          INSTALLER_READ_OK &&
                      tableOffset == (Unsigned64)INSTALLER_LOADER_HEADER_OFFSET);
        checkThat(&failureCount, "and reads",
                  installerReadOffsetTable(&fileHead[tableOffset], sizeof(fileHead) - tableOffset,
                                           tableOffset, &table) == INSTALLER_READ_OK);
        checkThat(&failureCount, "with the revision from its identifier", table.tableRevision == 2U);
        checkThat(&failureCount, "six fields, found by checksum rather than assumed",
                  table.wordCount == 6U);
        checkThat(&failureCount, "the first is what the installer accounts for",
                  table.totalSizeInBytes == 0xAABBCCDDUL);
        checkThat(&failureCount, "the header offset is the second from last",
                  table.headerOffsetInBytes == 0x4000UL);
        checkThat(&failureCount, "and the data offset is the last",
                  table.dataOffsetInBytes == 0x5000UL);
    }

    printf("\n-- a longer layout, which the reader has not been told about --\n");
    {
        static const Unsigned32 words[8] = { 0x11111111UL, 0x22222222UL, 0x33333333UL, 0x44444444UL,
                                             0x55555555UL, 0x66666666UL, 0x77777777UL, 0x88888888UL };
        Unsigned64 tableOffset = 0U;
        InstallerOffsetTable table;

        clearHead();
        buildTable(&fileHead[INSTALLER_LOADER_HEADER_OFFSET], "rDlPtS07\207eVx", words, 8UL);

        installerFindOffsetTable(fileHead, sizeof(fileHead), &tableOffset);
        checkThat(&failureCount, "it reads too",
                  installerReadOffsetTable(&fileHead[tableOffset], sizeof(fileHead) - tableOffset,
                                           tableOffset, &table) == INSTALLER_READ_OK);
        checkThat(&failureCount, "eight fields this time", table.wordCount == 8U);
        checkThat(&failureCount, "revision seven", table.tableRevision == 7U);
        checkThat(&failureCount, "and the offsets still come off the end",
                  table.headerOffsetInBytes == 0x77777777UL &&
                      table.dataOffsetInBytes == 0x88888888UL);
    }

    printf("\n-- a loader that points at the table instead --\n");
    {
        static const Unsigned32 words[6] = { 0x900UL, 1UL, 2UL, 3UL, 0xABCUL, 0xDEFUL };
        const Unsigned32 pointer = 0x80UL;
        Unsigned64 tableOffset = 0U;
        InstallerOffsetTable table;

        clearHead();
        writeUnsigned32(fileHead, INSTALLER_LOADER_HEADER_OFFSET,
                        (Unsigned32)INSTALLER_LOADER_POINTER_MARK);
        writeUnsigned32(fileHead, INSTALLER_LOADER_HEADER_OFFSET + 4UL, pointer);
        writeUnsigned32(fileHead, INSTALLER_LOADER_HEADER_OFFSET + 8UL, ~pointer);
        buildTable(&fileHead[pointer], "rDlPtS05\207eVx", words, 6UL);

        checkThat(&failureCount, "the pointer is followed",
                  installerFindOffsetTable(fileHead, sizeof(fileHead), &tableOffset) ==
                          INSTALLER_READ_OK &&
                      tableOffset == (Unsigned64)pointer);
        checkThat(&failureCount, "and the table is there",
                  installerReadOffsetTable(&fileHead[tableOffset], sizeof(fileHead) - tableOffset,
                                           tableOffset, &table) == INSTALLER_READ_OK &&
                      table.dataOffsetInBytes == 0xDEFUL);
        checkThat(&failureCount, "remembering where it was", table.tableOffsetInBytes == 0x80U);
    }

    printf("\n-- refusing what it should --\n");
    {
        static const Unsigned32 words[6] = { 1UL, 2UL, 3UL, 4UL, 5UL, 6UL };
        Unsigned64 tableOffset = 0U;
        InstallerOffsetTable table;
        MemorySize length;

        clearHead();
        checkThat(&failureCount, "an ordinary program is not an installer",
                  installerFindOffsetTable(fileHead, sizeof(fileHead), &tableOffset) ==
                      INSTALLER_READ_NOT_AN_INSTALLER);

        writeUnsigned32(fileHead, INSTALLER_LOADER_HEADER_OFFSET,
                        (Unsigned32)INSTALLER_LOADER_POINTER_MARK);
        writeUnsigned32(fileHead, INSTALLER_LOADER_HEADER_OFFSET + 4UL, 0x80UL);
        writeUnsigned32(fileHead, INSTALLER_LOADER_HEADER_OFFSET + 8UL, 0x80UL);
        checkThat(&failureCount, "a pointer that disagrees with itself is refused",
                  installerFindOffsetTable(fileHead, sizeof(fileHead), &tableOffset) ==
                      INSTALLER_READ_POINTER_INCONSISTENT);

        clearHead();
        length = buildTable(&fileHead[INSTALLER_LOADER_HEADER_OFFSET], "rDlPtS02\207eVx", words, 6UL);
        fileHead[INSTALLER_LOADER_HEADER_OFFSET + INSTALLER_TABLE_IDENTIFIER_BYTES] ^= 0x01U;
        checkThat(&failureCount, "a table whose checksum disagrees is refused",
                  installerReadOffsetTable(&fileHead[INSTALLER_LOADER_HEADER_OFFSET],
                                           length, INSTALLER_LOADER_HEADER_OFFSET,
                                           &table) == INSTALLER_READ_UNKNOWN_LAYOUT);

        clearHead();
        buildTable(&fileHead[INSTALLER_LOADER_HEADER_OFFSET], "rDlPtSxx\207eVx", words, 6UL);
        checkThat(&failureCount, "an identifier without digits is not one",
                  installerFindOffsetTable(fileHead, sizeof(fileHead), &tableOffset) ==
                      INSTALLER_READ_NOT_AN_INSTALLER);

        checkThat(&failureCount, "and too few bytes to hold a table say so",
                  installerReadOffsetTable(fileHead, 8UL, 0U, &table) == INSTALLER_READ_TRUNCATED);
    }

    printf("\n-- finding a table that is not where the old loaders keep it --\n");
    {
        static const Unsigned32 words[6] = { 1UL, 2UL, 3UL, 4UL, 0x111UL, 0x222UL };
        const MemorySize tableAt = 0xC0UL;
        const MemorySize versionAt = 0x60UL;
        Unsigned64 found;
        InstallerOffsetTable table;

        clearHead();
        buildTable(&fileHead[tableAt], "rDlPtS06\207eVx", words, 6UL);
        {
            static const char version[] = "Inno Setup Setup Data (5.5.0)";
            MemorySize index;

            for (index = 0UL; index < stringLength(version); index += 1UL)
            {
                fileHead[versionAt + index] = (Unsigned8)version[index];
            }
        }

        found = installerFindTableMarker(fileHead, sizeof(fileHead), 0U);
        checkThat(&failureCount, "the table is found by looking for it", found == (Unsigned64)tableAt);
        checkThat(&failureCount, "and reads once found",
                  installerReadOffsetTable(&fileHead[found], sizeof(fileHead) - (MemorySize)found,
                                           found, &table) == INSTALLER_READ_OK &&
                      table.dataOffsetInBytes == 0x222UL);
        checkThat(&failureCount, "the version string is found too",
                  installerFindVersionMarker(fileHead, sizeof(fileHead), 0U) ==
                      (Unsigned64)versionAt);

        checkThat(&failureCount, "an offset is reported against the file, not the chunk",
                  installerFindTableMarker(&fileHead[0x80], 0x80UL, 0x80ULL) ==
                      (Unsigned64)tableAt);

        checkThat(&failureCount, "a mark cut in half is not found in the first half",
                  installerFindTableMarker(fileHead, tableAt + 4UL, 0U) ==
                      (Unsigned64)INSTALLER_MARKER_NOT_FOUND);
        {
            MemorySize resumeAt = (tableAt + 4UL) - INSTALLER_MARKER_OVERLAP_BYTES;

            checkThat(&failureCount, "but is found once the next chunk steps back by the overlap",
                      installerFindTableMarker(&fileHead[resumeAt], sizeof(fileHead) - resumeAt,
                                               (Unsigned64)resumeAt) == (Unsigned64)tableAt);
        }

        clearHead();
        checkThat(&failureCount, "and a file with neither says so",
                  installerFindTableMarker(fileHead, sizeof(fileHead), 0U) ==
                          (Unsigned64)INSTALLER_MARKER_NOT_FOUND &&
                      installerFindVersionMarker(fileHead, sizeof(fileHead), 0U) ==
                          (Unsigned64)INSTALLER_MARKER_NOT_FOUND);
    }

    printf("\n-- the version the installer was built with --\n");
    {
        static const char field[INSTALLER_VERSION_STRING_BYTES] = "Inno Setup Setup Data (5.5.0) (u)";
        char version[80];

        checkThat(&failureCount, "the version string reads",
                  installerReadVersionString((const Unsigned8 *)field, sizeof(field), version,
                                             sizeof(version)) == INSTALLER_READ_OK);
        checkThat(&failureCount, "exactly as written",
                  stringEquals(version, "Inno Setup Setup Data (5.5.0) (u)"));
        checkThat(&failureCount, "and stops at the padding rather than running through it",
                  stringLength(version) == 33UL);

        checkThat(&failureCount, "something else in that place is refused",
                  installerReadVersionString((const Unsigned8 *)"MZP\0 not a version string at all",
                                             32UL, version, sizeof(version)) ==
                      INSTALLER_READ_NOT_A_VERSION_STRING);
        checkThat(&failureCount, "and a refused read leaves an empty string, not a stale one",
                  version[0] == '\0');
    }

    return checkSummarize(failureCount, "installer");
}
