#define _POSIX_C_SOURCE 200809L

#include "platform/linux/linuxDiscStore.h"

#include "utils/strings.h"
#include "victoria/platformInterface.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define PATH_LIMIT 1024U

/* How deep into a copied disc to walk. A mounted Sims 2 CD is four levels; this
 * is generous without letting a symlink loop run away. */
#define FOLDER_DEPTH_LIMIT 16U

static VirtualReadResult readAt(int descriptor, Unsigned64 offsetInBytes, MemorySize sizeInBytes,
                                Unsigned8 *destination)
{
    MemorySize done = 0UL;

    while (done < sizeInBytes)
    {
        ssize_t taken = pread(descriptor, destination + done, (size_t)(sizeInBytes - done),
                              (off_t)(offsetInBytes + (Unsigned64)done));

        if (taken <= 0)
        {
            return VIRTUAL_READ_FAILED;
        }
        done += (MemorySize)taken;
    }
    return VIRTUAL_READ_OK;
}

static VirtualReadResult discStoreRead(void *context, Unsigned64 offsetInBytes, MemorySize sizeInBytes,
                                       Unsigned8 *destination)
{
    DiscStore *store = (DiscStore *)context;
    Unsigned32 index;

    if (store->isSingleImage)
    {
        return readAt(store->imageDescriptor, offsetInBytes, sizeInBytes, destination);
    }

    /* Folder mode: find whichever file covers this offset. A read never spans
     * two of them, because every catalogue entry was built from one file. */
    for (index = 0U; index < store->fileCount; index++)
    {
        const DiscStoreFile *file = &store->files[index];

        if (offsetInBytes >= file->baseOffsetInBytes &&
            offsetInBytes + (Unsigned64)sizeInBytes <= file->baseOffsetInBytes + file->sizeInBytes)
        {
            return readAt(file->descriptor, offsetInBytes - file->baseOffsetInBytes, sizeInBytes,
                          destination);
        }
    }
    return VIRTUAL_READ_OUT_OF_RANGE;
}

static MemorySize joinPath(char *destination, MemorySize capacity, const char *directory,
                           const char *name)
{
    MemorySize length = 0UL;

    destination[0] = '\0';
    length = stringAppend(destination, capacity, directory);
    if (length > 0UL && destination[length - 1UL] != '/')
    {
        length = stringAppend(destination, capacity, "/");
    }
    return stringAppend(destination, capacity, name);
}

/* Walks a directory, opening every file it finds and giving each a slot in the
 * invented address space. Directories are recursed; anything else is skipped. */
static Boolean walkFolder(DiscStore *store, VirtualFileSystem *fileSystem, MemoryArena *arena,
                          const char *realDirectory, const char *virtualPrefix, Unsigned32 depth)
{
    DIR *directory;
    struct dirent *entry;

    if (depth > FOLDER_DEPTH_LIMIT)
    {
        return BOOLEAN_TRUE;
    }
    directory = opendir(realDirectory);
    if (directory == NULL_POINTER)
    {
        return BOOLEAN_FALSE;
    }

    while ((entry = readdir(directory)) != NULL_POINTER)
    {
        char realPath[PATH_LIMIT];
        char virtualPath[PATH_LIMIT];
        struct stat information;
        char *storedPath;
        MemorySize storedLength;
        int descriptor;

        if (entry->d_name[0] == '.')
        {
            continue;
        }
        joinPath(realPath, sizeof(realPath), realDirectory, entry->d_name);
        joinPath(virtualPath, sizeof(virtualPath), virtualPrefix, entry->d_name);

        if (stat(realPath, &information) != 0)
        {
            continue;
        }
        if (S_ISDIR(information.st_mode))
        {
            if (!walkFolder(store, fileSystem, arena, realPath, virtualPath, depth + 1U))
            {
                closedir(directory);
                return BOOLEAN_FALSE;
            }
            continue;
        }
        if (!S_ISREG(information.st_mode) || store->fileCount >= DISC_STORE_MAXIMUM_FILES)
        {
            continue;
        }

        descriptor = open(realPath, O_RDONLY);
        if (descriptor < 0)
        {
            continue;
        }

        storedLength = stringLength(virtualPath) + 1UL;
        storedPath = (char *)memoryArenaAllocate(arena, storedLength, 1UL);
        if (storedPath == NULL_POINTER)
        {
            close(descriptor);
            closedir(directory);
            return BOOLEAN_FALSE;
        }
        storedPath[0] = '\0';
        stringAppend(storedPath, storedLength, virtualPath);

        store->files[store->fileCount].descriptor = descriptor;
        store->files[store->fileCount].baseOffsetInBytes = store->totalSizeInBytes;
        store->files[store->fileCount].sizeInBytes = (Unsigned64)information.st_size;

        if (!virtualFileSystemAddEntry(fileSystem, storedPath, store->totalSizeInBytes,
                                       (Unsigned64)information.st_size))
        {
            close(descriptor);
            closedir(directory);
            return BOOLEAN_FALSE;
        }

        /* Files are laid end to end, so no two share an offset and a read can
         * only ever fall inside one of them. */
        store->totalSizeInBytes += (Unsigned64)information.st_size;
        store->fileCount++;
    }

    closedir(directory);
    return BOOLEAN_TRUE;
}

Boolean discStoreOpen(DiscStore *store, VirtualFileSystem *fileSystem, const char *path,
                      MemoryArena *arena)
{
    struct stat information;

    store->isSingleImage = BOOLEAN_FALSE;
    store->imageDescriptor = -1;
    store->totalSizeInBytes = 0U;
    store->files = NULL_POINTER;
    store->fileCount = 0U;

    if (stat(path, &information) != 0)
    {
        return BOOLEAN_FALSE;
    }

    if (S_ISDIR(information.st_mode))
    {
        VirtualFileEntry *entries;

        store->files = (DiscStoreFile *)memoryArenaAllocate(
            arena, (MemorySize)DISC_STORE_MAXIMUM_FILES * sizeof(DiscStoreFile), sizeof(void *));
        entries = (VirtualFileEntry *)memoryArenaAllocate(
            arena, (MemorySize)DISC_STORE_MAXIMUM_FILES * sizeof(VirtualFileEntry), sizeof(void *));
        if (store->files == NULL_POINTER || entries == NULL_POINTER)
        {
            return BOOLEAN_FALSE;
        }

        virtualFileSystemInitialize(fileSystem, discStoreRead, store, 0U);
        fileSystem->entries = entries;
        fileSystem->entryCapacity = DISC_STORE_MAXIMUM_FILES;

        if (!walkFolder(store, fileSystem, arena, path, "", 0U))
        {
            return BOOLEAN_FALSE;
        }
        /* Only known once every file has been counted. */
        fileSystem->storeSizeInBytes = store->totalSizeInBytes;
        return BOOLEAN_TRUE;
    }

    store->imageDescriptor = open(path, O_RDONLY);
    if (store->imageDescriptor < 0)
    {
        return BOOLEAN_FALSE;
    }
    store->isSingleImage = BOOLEAN_TRUE;
    store->totalSizeInBytes = (Unsigned64)information.st_size;
    virtualFileSystemInitialize(fileSystem, discStoreRead, store, store->totalSizeInBytes);
    return BOOLEAN_TRUE;
}

void discStoreClose(DiscStore *store)
{
    Unsigned32 index;

    if (store->imageDescriptor >= 0)
    {
        close(store->imageDescriptor);
        store->imageDescriptor = -1;
    }
    for (index = 0U; index < store->fileCount; index++)
    {
        close(store->files[index].descriptor);
    }
    store->fileCount = 0U;
}

const char *discStoreDescribe(const DiscStore *store)
{
    return store->isSingleImage ? "disc image" : "folder";
}
