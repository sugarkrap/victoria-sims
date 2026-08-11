#ifndef VICTORIA_VIRTUAL_FILE_SYSTEM_HEADER
#define VICTORIA_VIRTUAL_FILE_SYSTEM_HEADER

#include "victoria/coreTypes.h"

typedef enum VirtualReadResult
{
    VIRTUAL_READ_OK = 0,
    VIRTUAL_READ_PENDING,
    VIRTUAL_READ_OUT_OF_RANGE,
    VIRTUAL_READ_FAILED
} VirtualReadResult;

const char *virtualReadResultGetName(VirtualReadResult result);

typedef VirtualReadResult (*VirtualStoreRead)(void *context, Unsigned64 offsetInBytes,
                                              MemorySize sizeInBytes, Unsigned8 *destination);

typedef struct VirtualFileEntry
{
    const char *path;
    Unsigned64 offsetInBytes;
    Unsigned64 sizeInBytes;
} VirtualFileEntry;

typedef struct VirtualFileSystem
{
    VirtualStoreRead read;
    void *readContext;
    Unsigned64 storeSizeInBytes;

    VirtualFileEntry *entries;
    Unsigned32 entryCount;
    Unsigned32 entryCapacity;
} VirtualFileSystem;

void virtualFileSystemInitialize(VirtualFileSystem *fileSystem, VirtualStoreRead read, void *readContext,
                                 Unsigned64 storeSizeInBytes);

VirtualReadResult virtualFileSystemReadStore(VirtualFileSystem *fileSystem, Unsigned64 offsetInBytes,
                                             MemorySize sizeInBytes, Unsigned8 *destination);

Integer32 virtualFileSystemFind(const VirtualFileSystem *fileSystem, const char *path);

const VirtualFileEntry *virtualFileSystemGetEntry(const VirtualFileSystem *fileSystem, Unsigned32 index);

VirtualReadResult virtualFileSystemReadFile(VirtualFileSystem *fileSystem, Unsigned32 index,
                                            Unsigned64 offsetInFile, MemorySize sizeInBytes,
                                            Unsigned8 *destination);

Boolean virtualFileSystemAddEntry(VirtualFileSystem *fileSystem, const char *path, Unsigned64 offsetInBytes,
                                  Unsigned64 sizeInBytes);

#endif
