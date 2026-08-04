#include "victoria/virtualFileSystem.h"

static char toLowerCase(char character)
{
    if (character >= 'A' && character <= 'Z')
    {
        return (char)(character - 'A' + 'a');
    }
    return character;
}

/* Paths differ in case between a disc's two name trees, and in separator on a
 * host that hands us Windows paths. Neither difference is meaningful, so
 * neither is allowed to decide whether a file was found. */
static Boolean pathsMatch(const char *first, const char *second)
{
    MemorySize index = 0UL;

    for (;;)
    {
        char left = first[index];
        char right = second[index];

        if (left == '\\')
        {
            left = '/';
        }
        if (right == '\\')
        {
            right = '/';
        }

        if (toLowerCase(left) != toLowerCase(right))
        {
            return BOOLEAN_FALSE;
        }
        if (left == '\0')
        {
            return BOOLEAN_TRUE;
        }
        index++;
    }
}

const char *virtualReadResultGetName(VirtualReadResult result)
{
    switch (result)
    {
    case VIRTUAL_READ_OK:
        return "ok";
    case VIRTUAL_READ_PENDING:
        return "waiting for the host";
    case VIRTUAL_READ_OUT_OF_RANGE:
        return "range lies outside the store";
    case VIRTUAL_READ_FAILED:
        return "the store could not be read";
    default:
        return "unknown";
    }
}

void virtualFileSystemInitialize(VirtualFileSystem *fileSystem, VirtualStoreRead read, void *readContext,
                                 Unsigned64 storeSizeInBytes)
{
    fileSystem->read = read;
    fileSystem->readContext = readContext;
    fileSystem->storeSizeInBytes = storeSizeInBytes;
    fileSystem->entries = NULL_POINTER;
    fileSystem->entryCount = 0U;
    fileSystem->entryCapacity = 0U;
}

VirtualReadResult virtualFileSystemReadStore(VirtualFileSystem *fileSystem, Unsigned64 offsetInBytes,
                                             MemorySize sizeInBytes, Unsigned8 *destination)
{
    if (fileSystem->read == NULL_POINTER)
    {
        return VIRTUAL_READ_FAILED;
    }
    if (sizeInBytes == 0UL)
    {
        return VIRTUAL_READ_OK;
    }
    if (offsetInBytes + (Unsigned64)sizeInBytes > fileSystem->storeSizeInBytes ||
        offsetInBytes + (Unsigned64)sizeInBytes < offsetInBytes)
    {
        return VIRTUAL_READ_OUT_OF_RANGE;
    }
    return fileSystem->read(fileSystem->readContext, offsetInBytes, sizeInBytes, destination);
}

Integer32 virtualFileSystemFind(const VirtualFileSystem *fileSystem, const char *path)
{
    Unsigned32 index;

    for (index = 0U; index < fileSystem->entryCount; index++)
    {
        if (pathsMatch(fileSystem->entries[index].path, path))
        {
            return (Integer32)index;
        }
    }
    return -1;
}

const VirtualFileEntry *virtualFileSystemGetEntry(const VirtualFileSystem *fileSystem, Unsigned32 index)
{
    if (index >= fileSystem->entryCount)
    {
        return NULL_POINTER;
    }
    return &fileSystem->entries[index];
}

VirtualReadResult virtualFileSystemReadFile(VirtualFileSystem *fileSystem, Unsigned32 index,
                                            Unsigned64 offsetInFile, MemorySize sizeInBytes,
                                            Unsigned8 *destination)
{
    const VirtualFileEntry *entry;
    Unsigned64 end;

    if (index >= fileSystem->entryCount)
    {
        return VIRTUAL_READ_OUT_OF_RANGE;
    }
    entry = &fileSystem->entries[index];

    end = offsetInFile + (Unsigned64)sizeInBytes;
    if (end < offsetInFile || end > entry->sizeInBytes)
    {
        return VIRTUAL_READ_OUT_OF_RANGE;
    }
    return virtualFileSystemReadStore(fileSystem, entry->offsetInBytes + offsetInFile, sizeInBytes,
                                      destination);
}

Boolean virtualFileSystemAddEntry(VirtualFileSystem *fileSystem, const char *path, Unsigned64 offsetInBytes,
                                  Unsigned64 sizeInBytes)
{
    VirtualFileEntry *entry;

    if (fileSystem->entries == NULL_POINTER || fileSystem->entryCount >= fileSystem->entryCapacity)
    {
        return BOOLEAN_FALSE;
    }

    entry = &fileSystem->entries[fileSystem->entryCount];
    entry->path = path;
    entry->offsetInBytes = offsetInBytes;
    entry->sizeInBytes = sizeInBytes;
    fileSystem->entryCount++;
    return BOOLEAN_TRUE;
}
