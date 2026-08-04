#ifndef VICTORIA_LINUX_DISC_STORE_HEADER
#define VICTORIA_LINUX_DISC_STORE_HEADER

#include "victoria/coreTypes.h"
#include "victoria/memoryArena.h"
#include "victoria/virtualFileSystem.h"

/* Backs a virtual file system with something on this machine: a disc image, or
 * a directory that a disc has been copied or mounted into.
 *
 * A folder is not one store the way an image is, so it is made to look like
 * one. Each file is given a base offset in an address space this invents, and a
 * read is dispatched to whichever file covers the offset it asks for. That way
 * the engine sees exactly the same interface either way, and neither the disc
 * reader nor anything above it learns which it got.
 *
 * Reads here never answer PENDING. A file descriptor answers immediately, which
 * is the whole difference between this and the browser. */

#define DISC_STORE_MAXIMUM_FILES 4096U

typedef struct DiscStoreFile
{
    int descriptor;
    Unsigned64 baseOffsetInBytes;
    Unsigned64 sizeInBytes;
} DiscStoreFile;

typedef struct DiscStore
{
    /* Set when the store is one image rather than a directory of files. */
    Boolean isSingleImage;
    int imageDescriptor;
    Unsigned64 totalSizeInBytes;

    DiscStoreFile *files;
    Unsigned32 fileCount;
} DiscStore;

/* Opens a path as whichever of the two it turns out to be, and leaves
 * fileSystem ready to read. For a directory the catalogue is filled here; for
 * an image the disc reader fills it by walking the image. */
Boolean discStoreOpen(DiscStore *store, VirtualFileSystem *fileSystem, const char *path,
                      MemoryArena *arena);

void discStoreClose(DiscStore *store);

/* Named so a caller can say what it opened. */
const char *discStoreDescribe(const DiscStore *store);

#endif
