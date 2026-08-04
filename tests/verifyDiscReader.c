/* Walks the fixture disc and checks the catalogue against what is on it.

   The image is a real ISO 9660 one built by scripts/makeTestDisc.sh, carrying
   the same scenegraph fixtures the package reader test uses, laid out the way a
   retail disc lays them out. So this does not stop at "the walk found seven
   files": it goes on to open one of them through the virtual file system and
   read its resources, which is the path the engine will actually take.

   It also checks how the disc is read, not only what is found. A reader that
   pulled the image into memory would pass every check about paths and then be
   unusable on the platform it exists for, where the image is a browser's File
   and twenty times the memory budget.

   File I/O lives here rather than in the engine. The engine is handed bytes,
   which is what lets it be identical on a platform with no filesystem. */

#include <stdio.h>

#include "victoria/discReader.h"
#include "victoria/freestandingRuntime.h"
#include "victoria/memoryArena.h"
#include "victoria/packageReader.h"
#include "victoria/virtualFileSystem.h"

#define IMAGE_CAPACITY (4UL * 1024UL * 1024UL)
#define ARENA_CAPACITY (1024UL * 1024UL)
#define PACKAGE_CAPACITY (1024UL * 1024UL)
#define DISC_FILE_LIMIT 64U

static Unsigned8 imageBuffer[IMAGE_CAPACITY];
static Unsigned8 arenaStorage[ARENA_CAPACITY];
static Unsigned8 packageBuffer[PACKAGE_CAPACITY];
static MemorySize imageSizeInBytes = 0UL;

static int failureCount = 0;

static void check(const char *description, int condition)
{
    printf("%s  %s\n", condition ? "ok  " : "FAIL", description);
    if (!condition)
    {
        failureCount += 1;
    }
}

/* Stands in for whatever holds the image. Counts what it is asked for, because
   what the reader does not ask for is as much the point as what it does. */
typedef struct TestStore
{
    Unsigned64 bytesRead;
    Unsigned64 largestReadInBytes;
    Unsigned32 readCount;
    Unsigned32 pendingCount;
    /* When set, every other read answers PENDING, the way a browser's does
       until the host has fetched the range. */
    Boolean stutters;
    Boolean withhold;
} TestStore;

static VirtualReadResult testStoreRead(void *context, Unsigned64 offsetInBytes, MemorySize sizeInBytes,
                                       Unsigned8 *destination)
{
    TestStore *store = (TestStore *)context;
    MemorySize index;

    if (store->stutters)
    {
        store->withhold = store->withhold ? BOOLEAN_FALSE : BOOLEAN_TRUE;
        if (store->withhold)
        {
            store->pendingCount++;
            return VIRTUAL_READ_PENDING;
        }
    }

    if (offsetInBytes + (Unsigned64)sizeInBytes > (Unsigned64)imageSizeInBytes)
    {
        return VIRTUAL_READ_OUT_OF_RANGE;
    }
    for (index = 0UL; index < sizeInBytes; index++)
    {
        destination[index] = imageBuffer[(MemorySize)offsetInBytes + index];
    }

    store->bytesRead += (Unsigned64)sizeInBytes;
    store->readCount++;
    if ((Unsigned64)sizeInBytes > store->largestReadInBytes)
    {
        store->largestReadInBytes = (Unsigned64)sizeInBytes;
    }
    return VIRTUAL_READ_OK;
}

static void resetStore(TestStore *store, Boolean stutters)
{
    store->bytesRead = 0U;
    store->largestReadInBytes = 0U;
    store->readCount = 0U;
    store->pendingCount = 0U;
    store->stutters = stutters;
    store->withhold = BOOLEAN_FALSE;
}

static MemorySize loadImage(const char *path)
{
    FILE *inputFile = fopen(path, "rb");
    MemorySize sizeInBytes;

    if (inputFile == NULL)
    {
        printf("FAIL  cannot open %s\n", path);
        failureCount += 1;
        return 0UL;
    }
    sizeInBytes = (MemorySize)fread(imageBuffer, 1, IMAGE_CAPACITY, inputFile);
    fclose(inputFile);
    return sizeInBytes;
}

/* Runs a whole walk and leaves the catalogue in fileSystem. */
static DiscReadStatus walkDisc(VirtualFileSystem *fileSystem, MemoryArena *arena, TestStore *store,
                               DiscReader *reader)
{
    DiscReadStatus status;

    virtualFileSystemInitialize(fileSystem, testStoreRead, store, (Unsigned64)imageSizeInBytes);
    status = discReaderBegin(reader, fileSystem, arena, DISC_FILE_LIMIT);
    if (status != DISC_READ_PENDING)
    {
        return status;
    }
    return discReaderRunToCompletion(reader);
}

static Boolean catalogueHas(const VirtualFileSystem *fileSystem, const char *path)
{
    return virtualFileSystemFind(fileSystem, path) >= 0 ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

static void checkTypeCount(const Package *package, const char *name, Unsigned32 typeIdentifier,
                           Unsigned32 expectedCount)
{
    char description[96];
    Unsigned32 actual = packageReaderCountResourcesOfType(package, typeIdentifier);

    sprintf(description, "the package read off the disc holds %u %s", (unsigned)expectedCount, name);
    check(description, actual == expectedCount);
}

int main(void)
{
    MemoryArena arena;
    VirtualFileSystem fileSystem;
    DiscReader reader;
    TestStore store;
    DiscReadStatus status;
    Unsigned64 walkBytes;

    imageSizeInBytes = loadImage("testAssets/discs/testDisc.iso");
    if (imageSizeInBytes == 0UL)
    {
        return 1;
    }

    printf("-- walking the disc --\n");
    memoryArenaInitialize(&arena, arenaStorage, ARENA_CAPACITY);
    resetStore(&store, BOOLEAN_FALSE);
    status = walkDisc(&fileSystem, &arena, &store, &reader);
    check("the walk completes", status == DISC_READ_COMPLETE);
    if (status != DISC_READ_COMPLETE)
    {
        printf("  status: %s\n", discReadStatusGetName(status));
        return 1;
    }

    check("reads the volume identifier", stringEquals(reader.volumeIdentifier, "VICTORIA_TEST"));
    check("prefers the Joliet name tree", reader.usesJoliet == BOOLEAN_TRUE);
    check("finds every file", fileSystem.entryCount == 7U);

    check("finds a nested package", catalogueHas(&fileSystem, "TSData/Res/Sims3D/teapot_model.package"));
    check("finds the material definition",
          catalogueHas(&fileSystem, "TSData/Res/Materials/material_definition.package"));
    check("finds the root level file", catalogueHas(&fileSystem, "Autorun.inf"));
    check("finds the installer archive", catalogueHas(&fileSystem, "Support/data1.cab"));
    check("keeps a file that only looks like a package",
          catalogueHas(&fileSystem, "TSData/Res/NotReally.package"));
    check("does not invent files", catalogueHas(&fileSystem, "TSData/Res/Sims3D/nothing.package") == BOOLEAN_FALSE);

    /* A disc's two name trees disagree about case for the same file, and a host
       may hand us a path with the other separator. Neither should decide
       whether a file is found. */
    check("finds a file whatever its case",
          catalogueHas(&fileSystem, "tsdata/res/sims3d/TEAPOT_MODEL.PACKAGE"));
    check("finds a file written with backslashes",
          catalogueHas(&fileSystem, "TSData\\Res\\Sims3D\\teapot_model.package"));

    printf("\n-- what the walk cost --\n");
    walkBytes = store.bytesRead;
    check("does not read the image to catalogue it", walkBytes < (Unsigned64)imageSizeInBytes / 4U);
    check("never asks for more than one directory at a time",
          store.largestReadInBytes <= DISC_READER_DIRECTORY_BUFFER_BYTES);
    printf("  read %lu of %lu bytes in %u reads, largest %lu\n", (unsigned long)walkBytes,
           (unsigned long)imageSizeInBytes, (unsigned)store.readCount,
           (unsigned long)store.largestReadInBytes);

    printf("\n-- opening a package found on the disc --\n");
    {
        Integer32 index = virtualFileSystemFind(&fileSystem, "TSData/Res/Sims3D/teapot_model.package");
        const VirtualFileEntry *entry = virtualFileSystemGetEntry(&fileSystem, (Unsigned32)index);
        Package package;
        PackageReadResult result;
        VirtualReadResult read;

        check("the catalogue knows where the package is", entry != NULL_POINTER);
        if (entry != NULL_POINTER)
        {
            check("the catalogue has its size", entry->sizeInBytes == 699103U);

            read = virtualFileSystemReadFile(&fileSystem, (Unsigned32)index, 0U,
                                             (MemorySize)entry->sizeInBytes, packageBuffer);
            check("reads the package out of the image", read == VIRTUAL_READ_OK);

            result = packageReaderOpen(&package, packageBuffer, (MemorySize)entry->sizeInBytes, &arena);
            check("the package reader accepts what the disc reader found", result == PACKAGE_READ_OK);
            if (result == PACKAGE_READ_OK)
            {
                check("reports DBPF 1.1", package.majorVersion == 1U && package.minorVersion == 1U);
                checkTypeCount(&package, "CRES", (Unsigned32)PACKAGE_TYPE_CRES, 1U);
                checkTypeCount(&package, "SHPE", (Unsigned32)PACKAGE_TYPE_SHPE, 1U);
                checkTypeCount(&package, "GMND", (Unsigned32)PACKAGE_TYPE_GMND, 1U);
                checkTypeCount(&package, "GMDC", (Unsigned32)PACKAGE_TYPE_GMDC, 1U);
            }

            check("refuses a read past the end of a file",
                  virtualFileSystemReadFile(&fileSystem, (Unsigned32)index, entry->sizeInBytes - 4U, 8UL,
                                            packageBuffer) == VIRTUAL_READ_OUT_OF_RANGE);
        }

        check("the decoy is not a package",
              packageReaderOpen(&package, (const Unsigned8 *)"this is not a package at all", 28UL, &arena) ==
                  PACKAGE_READ_TRUNCATED);
    }

    printf("\n-- a store that cannot answer at once --\n");
    {
        /* What a browser does: the bytes are not there yet, ask again. A reader
           that assumed reads always succeed would lose whichever directory it
           was part way through. */
        MemoryArena secondArena;
        VirtualFileSystem secondFileSystem;
        DiscReader secondReader;
        TestStore stutteringStore;

        memoryArenaInitialize(&secondArena, arenaStorage, ARENA_CAPACITY);
        resetStore(&stutteringStore, BOOLEAN_TRUE);
        status = walkDisc(&secondFileSystem, &secondArena, &stutteringStore, &secondReader);

        check("the walk completes even so", status == DISC_READ_COMPLETE);
        check("it really did have to wait", stutteringStore.pendingCount > 0U);
        check("finds exactly the same files", secondFileSystem.entryCount == 7U);
        check("and the same nested package",
              catalogueHas(&secondFileSystem, "TSData/Res/Sims3D/teapot_model.package"));
        check("reads no more than it did before", stutteringStore.bytesRead == walkBytes);
        printf("  waited %u times\n", (unsigned)stutteringStore.pendingCount);
    }

    printf("\n-- refusing what is not a disc --\n");
    {
        MemoryArena thirdArena;
        VirtualFileSystem thirdFileSystem;
        DiscReader thirdReader;
        TestStore thirdStore;
        MemorySize savedSize = imageSizeInBytes;
        MemorySize index;

        for (index = 0UL; index < 128UL * 1024UL; index++)
        {
            imageBuffer[index] = 0U;
        }
        imageSizeInBytes = 128UL * 1024UL;

        memoryArenaInitialize(&thirdArena, arenaStorage, ARENA_CAPACITY);
        resetStore(&thirdStore, BOOLEAN_FALSE);
        status = walkDisc(&thirdFileSystem, &thirdArena, &thirdStore, &thirdReader);
        check("rejects an image with no descriptors", status == DISC_READ_NOT_A_DISC);

        imageSizeInBytes = savedSize;
    }

    if (failureCount > 0)
    {
        printf("\n%d check(s) failed\n", failureCount);
        return 1;
    }
    printf("\nall disc reader checks passed\n");
    return 0;
}
