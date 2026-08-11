#ifndef VICTORIA_LINUX_DISC_STORE_HEADER
#define VICTORIA_LINUX_DISC_STORE_HEADER

#include "victoria/coreTypes.h"
#include "victoria/memoryArena.h"
#include "victoria/virtualFileSystem.h"

#define DISC_STORE_MAXIMUM_FILES 4096U

typedef struct DiscStoreFile
{
    int descriptor;
    Unsigned64 baseOffsetInBytes;
    Unsigned64 sizeInBytes;
} DiscStoreFile;

typedef struct DiscStore
{
    Boolean isSingleImage;
    int imageDescriptor;
    Unsigned64 totalSizeInBytes;

    DiscStoreFile *files;
    Unsigned32 fileCount;
} DiscStore;

Boolean discStoreOpen(DiscStore *store, VirtualFileSystem *fileSystem, const char *path,
                      MemoryArena *arena);

void discStoreClose(DiscStore *store);

const char *discStoreDescribe(const DiscStore *store);

#endif
