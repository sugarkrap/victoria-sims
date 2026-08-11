#include "victoria/discReader.h"

#include "utils/strings.h"
#include "victoria/freestandingRuntime.h"

#define FIRST_DESCRIPTOR_SECTOR 16UL

#define LAST_DESCRIPTOR_SECTOR 80UL

#define DESCRIPTOR_PRIMARY 1
#define DESCRIPTOR_SUPPLEMENTARY 2
#define DESCRIPTOR_TERMINATOR 255

#define FILE_FLAG_DIRECTORY 0x02

#define OFFSET_VOLUME_IDENTIFIER 40UL
#define OFFSET_ESCAPE_SEQUENCES 88UL
#define OFFSET_ROOT_RECORD 156UL

#define RECORD_OFFSET_EXTENT 2UL
#define RECORD_OFFSET_LENGTH 10UL
#define RECORD_OFFSET_FLAGS 25UL
#define RECORD_OFFSET_IDENTIFIER_LENGTH 32UL
#define RECORD_HEADER_SIZE 33UL

#define STAGE_DESCRIPTORS 0
#define STAGE_DIRECTORIES 1
#define STAGE_DONE 2

static Unsigned32 readUnsigned32(const Unsigned8 *bytes, MemorySize offset)
{
    return (Unsigned32)bytes[offset] | ((Unsigned32)bytes[offset + 1UL] << 8) |
           ((Unsigned32)bytes[offset + 2UL] << 16) | ((Unsigned32)bytes[offset + 3UL] << 24);
}

const char *discReadStatusGetName(DiscReadStatus status)
{
    switch (status)
    {
    case DISC_READ_COMPLETE:
        return "complete";
    case DISC_READ_PENDING:
        return "more to read";
    case DISC_READ_NOT_A_DISC:
        return "not an ISO 9660 image";
    case DISC_READ_OUT_OF_ARENA:
        return "not enough arena space for the catalogue";
    case DISC_READ_TOO_MANY_FILES:
        return "more files on the disc than the catalogue can hold";
    case DISC_READ_DIRECTORY_TOO_LARGE:
        return "a directory is larger than the reader can parse in one piece";
    case DISC_READ_FAILED:
        return "the image could not be read";
    default:
        return "unknown";
    }
}

static Boolean escapesAreJoliet(const Unsigned8 *escapes)
{
    if (escapes[0] != (Unsigned8)'%' || escapes[1] != (Unsigned8)'/')
    {
        return BOOLEAN_FALSE;
    }
    return (escapes[2] == (Unsigned8)'@' || escapes[2] == (Unsigned8)'C' || escapes[2] == (Unsigned8)'E')
               ? BOOLEAN_TRUE
               : BOOLEAN_FALSE;
}

static MemorySize writeUTF8(char *destination, MemorySize capacity, Unsigned32 codePoint)
{
    if (codePoint < 0x80UL)
    {
        if (capacity < 1UL)
        {
            return 0UL;
        }
        destination[0] = (char)codePoint;
        return 1UL;
    }
    if (codePoint < 0x800UL)
    {
        if (capacity < 2UL)
        {
            return 0UL;
        }
        destination[0] = (char)(0xC0UL | (codePoint >> 6));
        destination[1] = (char)(0x80UL | (codePoint & 0x3FUL));
        return 2UL;
    }
    if (capacity < 3UL)
    {
        return 0UL;
    }
    destination[0] = (char)(0xE0UL | (codePoint >> 12));
    destination[1] = (char)(0x80UL | ((codePoint >> 6) & 0x3FUL));
    destination[2] = (char)(0x80UL | (codePoint & 0x3FUL));
    return 3UL;
}

static MemorySize decodeIdentifier(char *destination, MemorySize capacity, const Unsigned8 *raw,
                                   MemorySize rawLength, Boolean namesAreUCS2, Boolean isDirectory)
{
    MemorySize written = 0UL;
    MemorySize index;

    if (namesAreUCS2)
    {
        for (index = 0UL; index + 1UL < rawLength; index += 2UL)
        {
            Unsigned32 unit = ((Unsigned32)raw[index] << 8) | (Unsigned32)raw[index + 1UL];
            MemorySize taken;

            if (!isDirectory && unit == (Unsigned32)';')
            {
                break;
            }
            taken = writeUTF8(destination + written, capacity - written, unit);
            if (taken == 0UL)
            {
                return 0UL;
            }
            written += taken;
        }
    }
    else
    {
        for (index = 0UL; index < rawLength; index++)
        {
            if (!isDirectory && raw[index] == (Unsigned8)';')
            {
                break;
            }
            if (written + 1UL > capacity)
            {
                return 0UL;
            }
            destination[written] = (char)raw[index];
            written++;
        }
    }
    return written;
}

static Integer32 poolJoin(DiscReader *reader, MemorySize prefixOffset, const char *name,
                          MemorySize nameLength, MemorySize *resultOffset)
{
    const char *prefix = &reader->pathPool[prefixOffset];
    MemorySize prefixLength = stringLength(prefix);
    MemorySize separator = (prefixLength > 0UL) ? 1UL : 0UL;
    MemorySize total = prefixLength + separator + nameLength;
    char *destination;
    MemorySize index;

    if (total + 1UL > DISC_READER_PATH_LIMIT)
    {
        return -1;
    }
    if (reader->pathPoolUsed + total + 1UL > reader->pathPoolCapacity)
    {
        return -1;
    }

    destination = &reader->pathPool[reader->pathPoolUsed];
    memoryCopy(destination, prefix, prefixLength);
    if (separator == 1UL)
    {
        destination[prefixLength] = '/';
    }
    for (index = 0UL; index < nameLength; index++)
    {
        destination[prefixLength + separator + index] = name[index];
    }
    destination[total] = '\0';

    *resultOffset = reader->pathPoolUsed;
    reader->pathPoolUsed += total + 1UL;
    return 0;
}

DiscReadStatus discReaderBegin(DiscReader *reader, VirtualFileSystem *fileSystem, MemoryArena *arena,
                               Unsigned32 fileLimit)
{
    VirtualFileEntry *entries;
    MemorySize pathPoolBytes;

    reader->fileSystem = fileSystem;
    reader->stage = STAGE_DESCRIPTORS;
    reader->descriptorSector = (Unsigned32)FIRST_DESCRIPTOR_SECTOR;
    reader->namesAreUCS2 = BOOLEAN_FALSE;
    reader->usesJoliet = BOOLEAN_FALSE;
    reader->primaryFound = BOOLEAN_FALSE;
    reader->volumeIdentifier[0] = '\0';
    reader->rootSector = 0U;
    reader->rootLength = 0U;
    reader->pathPoolUsed = 0UL;
    reader->pendingCount = 0U;
    reader->directoriesWalked = 0U;

    if (fileLimit == 0U)
    {
        return DISC_READ_OUT_OF_ARENA;
    }

    entries = (VirtualFileEntry *)memoryArenaAllocate(arena, (MemorySize)fileLimit * sizeof(VirtualFileEntry),
                                                      sizeof(void *));
    reader->sectorBuffer = (Unsigned8 *)memoryArenaAllocate(arena, DISC_READER_SECTOR_SIZE, 8UL);
    reader->directoryBuffer =
        (Unsigned8 *)memoryArenaAllocate(arena, DISC_READER_DIRECTORY_BUFFER_BYTES, 8UL);
    pathPoolBytes = (MemorySize)fileLimit * DISC_READER_PATH_BYTES_PER_FILE;
    reader->pathPool = (char *)memoryArenaAllocate(arena, pathPoolBytes, 8UL);
    reader->pending = (DiscPendingDirectory *)memoryArenaAllocate(
        arena, (MemorySize)DISC_READER_PENDING_LIMIT * sizeof(DiscPendingDirectory), sizeof(void *));

    if (entries == NULL_POINTER || reader->sectorBuffer == NULL_POINTER ||
        reader->directoryBuffer == NULL_POINTER || reader->pathPool == NULL_POINTER ||
        reader->pending == NULL_POINTER)
    {
        return DISC_READ_OUT_OF_ARENA;
    }

    reader->pathPoolCapacity = pathPoolBytes;
    reader->pathPool[0] = '\0';
    reader->pathPoolUsed = 1UL;

    fileSystem->entries = entries;
    fileSystem->entryCount = 0U;
    fileSystem->entryCapacity = fileLimit;

    return DISC_READ_PENDING;
}

static DiscReadStatus stepDescriptors(DiscReader *reader)
{
    const Unsigned8 *sector = reader->sectorBuffer;
    VirtualReadResult read;
    Unsigned32 index;

    if ((MemorySize)reader->descriptorSector > LAST_DESCRIPTOR_SECTOR)
    {
        return reader->primaryFound ? DISC_READ_PENDING : DISC_READ_NOT_A_DISC;
    }

    read = virtualFileSystemReadStore(reader->fileSystem,
                                      (Unsigned64)reader->descriptorSector * DISC_READER_SECTOR_SIZE,
                                      DISC_READER_SECTOR_SIZE, reader->sectorBuffer);
    if (read == VIRTUAL_READ_PENDING)
    {
        return DISC_READ_PENDING;
    }
    if (read == VIRTUAL_READ_OUT_OF_RANGE)
    {
        return reader->primaryFound ? DISC_READ_PENDING : DISC_READ_NOT_A_DISC;
    }
    if (read != VIRTUAL_READ_OK)
    {
        return DISC_READ_FAILED;
    }

    if (sector[1] != (Unsigned8)'C' || sector[2] != (Unsigned8)'D' || sector[3] != (Unsigned8)'0' ||
        sector[4] != (Unsigned8)'0' || sector[5] != (Unsigned8)'1')
    {
        return DISC_READ_NOT_A_DISC;
    }

    if (sector[0] == DESCRIPTOR_PRIMARY)
    {
        MemorySize length = 32UL;

        for (index = 0U; index < 32U; index++)
        {
            reader->volumeIdentifier[index] = (char)sector[OFFSET_VOLUME_IDENTIFIER + index];
        }
        reader->volumeIdentifier[32] = '\0';
        while (length > 0UL && (reader->volumeIdentifier[length - 1UL] == ' ' ||
                                reader->volumeIdentifier[length - 1UL] == '\0' ||
                                (Unsigned8)reader->volumeIdentifier[length - 1UL] < 0x20U))
        {
            length--;
        }
        reader->volumeIdentifier[length] = '\0';

        reader->primaryFound = BOOLEAN_TRUE;
        if (!reader->usesJoliet)
        {
            reader->rootSector = readUnsigned32(sector, OFFSET_ROOT_RECORD + RECORD_OFFSET_EXTENT);
            reader->rootLength = readUnsigned32(sector, OFFSET_ROOT_RECORD + RECORD_OFFSET_LENGTH);
        }
    }
    else if (sector[0] == DESCRIPTOR_SUPPLEMENTARY && escapesAreJoliet(&sector[OFFSET_ESCAPE_SEQUENCES]))
    {
        reader->usesJoliet = BOOLEAN_TRUE;
        reader->namesAreUCS2 = BOOLEAN_TRUE;
        reader->rootSector = readUnsigned32(sector, OFFSET_ROOT_RECORD + RECORD_OFFSET_EXTENT);
        reader->rootLength = readUnsigned32(sector, OFFSET_ROOT_RECORD + RECORD_OFFSET_LENGTH);
    }
    else if (sector[0] == DESCRIPTOR_TERMINATOR)
    {
        reader->descriptorSector = (Unsigned32)LAST_DESCRIPTOR_SECTOR + 1U;
        return DISC_READ_PENDING;
    }

    reader->descriptorSector++;
    return DISC_READ_PENDING;
}

static DiscReadStatus enterDirectoryStage(DiscReader *reader)
{
    if (!reader->primaryFound || reader->rootLength == 0U)
    {
        return DISC_READ_NOT_A_DISC;
    }

    reader->pending[0].firstSector = reader->rootSector;
    reader->pending[0].lengthInBytes = reader->rootLength;
    reader->pending[0].pathOffset = 0UL;
    reader->pending[0].depth = 0U;
    reader->pendingCount = 1U;
    reader->stage = STAGE_DIRECTORIES;
    return DISC_READ_PENDING;
}

static DiscReadStatus stepDirectories(DiscReader *reader)
{
    DiscPendingDirectory directory;
    const Unsigned8 *data = reader->directoryBuffer;
    VirtualReadResult read;
    MemorySize cursor = 0UL;
    MemorySize length;

    if (reader->pendingCount == 0U)
    {
        reader->stage = STAGE_DONE;
        return DISC_READ_COMPLETE;
    }

    directory = reader->pending[reader->pendingCount - 1U];
    length = (MemorySize)directory.lengthInBytes;

    if (length > DISC_READER_DIRECTORY_BUFFER_BYTES)
    {
        return DISC_READ_DIRECTORY_TOO_LARGE;
    }

    read = virtualFileSystemReadStore(reader->fileSystem,
                                      (Unsigned64)directory.firstSector * DISC_READER_SECTOR_SIZE, length,
                                      reader->directoryBuffer);
    if (read == VIRTUAL_READ_PENDING)
    {
        return DISC_READ_PENDING;
    }
    if (read == VIRTUAL_READ_OUT_OF_RANGE)
    {
        reader->pendingCount--;
        return DISC_READ_PENDING;
    }
    if (read != VIRTUAL_READ_OK)
    {
        return DISC_READ_FAILED;
    }

    reader->pendingCount--;
    reader->directoriesWalked++;

    while (cursor < length)
    {
        char name[DISC_READER_PATH_LIMIT];
        const Unsigned8 *record;
        MemorySize recordLength = (MemorySize)data[cursor];
        MemorySize identifierLength;
        MemorySize nameLength;
        MemorySize pathOffset;
        Unsigned32 extentSector;
        Unsigned32 dataLength;
        Boolean isDirectory;

        if (recordLength == 0UL)
        {
            cursor = (cursor / DISC_READER_SECTOR_SIZE + 1UL) * DISC_READER_SECTOR_SIZE;
            continue;
        }
        if (recordLength < RECORD_HEADER_SIZE || cursor + recordLength > length)
        {
            break;
        }

        record = &data[cursor];
        extentSector = readUnsigned32(record, RECORD_OFFSET_EXTENT);
        dataLength = readUnsigned32(record, RECORD_OFFSET_LENGTH);
        isDirectory = (record[RECORD_OFFSET_FLAGS] & FILE_FLAG_DIRECTORY) != 0 ? BOOLEAN_TRUE : BOOLEAN_FALSE;
        identifierLength = (MemorySize)record[RECORD_OFFSET_IDENTIFIER_LENGTH];

        if (RECORD_HEADER_SIZE + identifierLength > recordLength)
        {
            break;
        }
        cursor += recordLength;

        if (identifierLength == 1UL &&
            (record[RECORD_HEADER_SIZE] == 0U || record[RECORD_HEADER_SIZE] == 1U))
        {
            continue;
        }

        nameLength = decodeIdentifier(name, sizeof(name), &record[RECORD_HEADER_SIZE], identifierLength,
                                      reader->namesAreUCS2, isDirectory);
        if (nameLength == 0UL)
        {
            continue;
        }

        if (poolJoin(reader, directory.pathOffset, name, nameLength, &pathOffset) != 0)
        {
            return DISC_READ_OUT_OF_ARENA;
        }

        if (isDirectory)
        {
            if (directory.depth + 1U > DISC_READER_MAXIMUM_DEPTH)
            {
                continue;
            }
            if (reader->pendingCount >= DISC_READER_PENDING_LIMIT)
            {
                return DISC_READ_TOO_MANY_FILES;
            }
            reader->pending[reader->pendingCount].firstSector = extentSector;
            reader->pending[reader->pendingCount].lengthInBytes = dataLength;
            reader->pending[reader->pendingCount].pathOffset = pathOffset;
            reader->pending[reader->pendingCount].depth = directory.depth + 1U;
            reader->pendingCount++;
        }
        else if (!virtualFileSystemAddEntry(reader->fileSystem, &reader->pathPool[pathOffset],
                                            (Unsigned64)extentSector * DISC_READER_SECTOR_SIZE,
                                            (Unsigned64)dataLength))
        {
            return DISC_READ_TOO_MANY_FILES;
        }
    }

    return DISC_READ_PENDING;
}

DiscReadStatus discReaderStep(DiscReader *reader)
{
    if (reader->stage == STAGE_DONE)
    {
        return DISC_READ_COMPLETE;
    }
    if (reader->stage == STAGE_DESCRIPTORS)
    {
        if ((MemorySize)reader->descriptorSector > LAST_DESCRIPTOR_SECTOR)
        {
            return enterDirectoryStage(reader);
        }
        return stepDescriptors(reader);
    }
    return stepDirectories(reader);
}

DiscReadStatus discReaderRunToCompletion(DiscReader *reader)
{
    Unsigned32 remaining = 100000U;

    for (;;)
    {
        DiscReadStatus status = discReaderStep(reader);

        if (status != DISC_READ_PENDING)
        {
            return status;
        }
        if (remaining == 0U)
        {
            return DISC_READ_FAILED;
        }
        remaining--;
    }
}
