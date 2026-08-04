/* Indexes a small disc and looks resources up across it.

   This exists because a Sim's face texture is not in the Sim's package. Finding
   it means knowing what is in every package on the disc, and the only reason
   that is affordable is that a scenegraph resource's key is its name hashed —
   so this reads each package's header and index and never its contents.

   Two things are checked that a happier test would skip.

   The index is built through a store that withholds every other read, the way a
   browser's does. A stepped reader that quietly forgets where it was is the
   failure this catches, and it is invisible on a store that always answers.

   The bytes read are counted. If this ever starts reading whole packages the
   count says so immediately, and the whole approach depends on it not. */

#include <stdio.h>

#include "utils/assert.h"
#include "utils/resourceHash.h"
#include "utils/strings.h"
#include "victoria/memoryArena.h"
#include "victoria/packageReader.h"
#include "victoria/resourceIndex.h"
#include "victoria/virtualFileSystem.h"

#define FILE_CAPACITY (1024UL * 1024UL)
#define ARENA_CAPACITY (4UL * 1024UL * 1024UL)
#define FILE_LIMIT 8U

static Unsigned8 fileBytes[2][FILE_CAPACITY];
static MemorySize fileSizes[2];
static Unsigned8 arenaStorage[ARENA_CAPACITY];

static Integer32 failureCount = 0;

typedef struct TestStore
{
    Unsigned64 bytesRead;
    Unsigned32 readCount;
    Unsigned32 pendingCount;
    Boolean stutters;
    Boolean withhold;
} TestStore;

/* One flat address space with the two files laid end to end, which is how the
   folder store presents a directory: each file gets a base offset in an
   invented space rather than a descriptor of its own. */
static VirtualReadResult testStoreRead(void *context, Unsigned64 offsetInBytes, MemorySize sizeInBytes,
                                       Unsigned8 *destination)
{
    TestStore *store = (TestStore *)context;
    MemorySize index;
    Unsigned64 base = 0U;
    Unsigned32 which;

    if (store->stutters)
    {
        store->withhold = store->withhold ? BOOLEAN_FALSE : BOOLEAN_TRUE;
        if (store->withhold)
        {
            store->pendingCount++;
            return VIRTUAL_READ_PENDING;
        }
    }

    for (which = 0U; which < 2U; which++)
    {
        if (offsetInBytes >= base && offsetInBytes + sizeInBytes <= base + (Unsigned64)fileSizes[which])
        {
            MemorySize start = (MemorySize)(offsetInBytes - base);

            for (index = 0UL; index < sizeInBytes; index++)
            {
                destination[index] = fileBytes[which][start + index];
            }
            store->bytesRead += (Unsigned64)sizeInBytes;
            store->readCount++;
            return VIRTUAL_READ_OK;
        }
        base += (Unsigned64)fileSizes[which];
    }
    return VIRTUAL_READ_OUT_OF_RANGE;
}

static MemorySize loadFile(const char *path, Unsigned8 *destination)
{
    FILE *inputFile = fopen(path, "rb");
    MemorySize size;

    if (inputFile == NULL)
    {
        printf("FAIL  cannot open %s\n", path);
        failureCount += 1;
        return 0UL;
    }
    size = (MemorySize)fread(destination, 1, FILE_CAPACITY, inputFile);
    fclose(inputFile);
    return size;
}

static Unsigned32 buildIndex(ResourceIndex *index, VirtualFileSystem *fileSystem, MemoryArena *arena,
                             Unsigned32 capacity)
{
    static const Unsigned32 wantedTypes[1] = { (Unsigned32)PACKAGE_TYPE_TXTR };
    Unsigned32 steps = 0U;

    if (!resourceIndexBegin(index, fileSystem, arena, capacity, wantedTypes, 1U))
    {
        printf("FAIL  the index would not begin\n");
        failureCount += 1;
        return 0U;
    }
    while (resourceIndexStep(index) == RESOURCE_INDEX_WORKING && steps < 100000U)
    {
        steps++;
    }
    return steps;
}

int main(void)
{
    MemoryArena arena;
    VirtualFileSystem fileSystem;
    TestStore store;
    ResourceIndex index;
    Unsigned64 totalBytes;

    memoryArenaInitialize(&arena, arenaStorage, ARENA_CAPACITY);

    fileSizes[0] = loadFile("testAssets/scenegraph/textures.package", fileBytes[0]);
    fileSizes[1] = loadFile("testAssets/scenegraph/teapot_model.package", fileBytes[1]);
    if (fileSizes[0] == 0UL || fileSizes[1] == 0UL)
    {
        return 1;
    }
    totalBytes = (Unsigned64)fileSizes[0] + (Unsigned64)fileSizes[1];

    store.bytesRead = 0U;
    store.readCount = 0U;
    store.pendingCount = 0U;
    store.stutters = BOOLEAN_FALSE;
    store.withhold = BOOLEAN_FALSE;

    virtualFileSystemInitialize(&fileSystem, testStoreRead, &store, totalBytes);
    /* The catalogue's storage comes from the caller, the same way the disc
       reader supplies it. */
    fileSystem.entries = (VirtualFileEntry *)memoryArenaAllocate(
        &arena, (MemorySize)FILE_LIMIT * sizeof(VirtualFileEntry), 8UL);
    fileSystem.entryCapacity = FILE_LIMIT;
    virtualFileSystemAddEntry(&fileSystem, "textures.package", 0U, (Unsigned64)fileSizes[0]);
    virtualFileSystemAddEntry(&fileSystem, "teapot_model.package", (Unsigned64)fileSizes[0],
                              (Unsigned64)fileSizes[1]);

    printf("-- indexing the disc --\n");
    {
        Unsigned32 steps = buildIndex(&index, &fileSystem, &arena, 64U);

        checkThat(&failureCount, "the walk finishes", steps > 0U && steps < 100000U);
        checkThat(&failureCount, "both packages were indexed", index.filesIndexed == 2U);
        checkThat(&failureCount, "and neither was refused", index.filesRefused == 0U);
        /* Four in one package and none in the other, which is what the two
           fixtures hold. A count that matched the total resources rather than
           the wanted ones would be visible here. */
        checkThat(&failureCount, "four textures were found", index.count == 4U);
        checkThat(&failureCount, "and nothing was dropped", index.dropped == 0U);

        printf("  read %lu of %lu bytes in %u reads\n", (unsigned long)store.bytesRead,
               (unsigned long)totalBytes, (unsigned)store.readCount);
        /* The whole approach rests on this. Reading the packages themselves
           would work and would also make a six hundred file disc unusable. */
        checkThat(&failureCount, "reading a small fraction of the disc",
                  store.bytesRead * 4U < totalBytes);
        checkThat(&failureCount, "two reads a package: a header and an index",
                  store.readCount == 4U);
    }

    printf("\n-- finding a resource by name across the disc --\n");
    {
        const ResourceIndexEntry *found =
            resourceIndexFindNamed(&index, (Unsigned32)PACKAGE_TYPE_TXTR, "brick_dxt1_txtr");

        checkThat(&failureCount, "a texture is found by its name alone", found != NULL_POINTER);
        if (found != NULL_POINTER)
        {
            /* Straight out of the fixture's own index, dumped before this
               existed: offset 0x60, size 0x2B6D, in the first file. */
            checkThat(&failureCount, "in the package that holds it", found->fileIndex == 0U);
            checkThat(&failureCount, "at the offset its index gives",
                      found->offsetInBytes == 0x60UL);
            checkThat(&failureCount, "with the size its index gives",
                      found->sizeInBytes == 0x2B6DUL);
            checkThat(&failureCount, "and the key really is the name hashed",
                      found->instanceIdentifier == resourceHashInstance("brick_dxt1_txtr") &&
                          found->instanceIdentifierHigh ==
                              resourceHashInstanceHigh("brick_dxt1_txtr"));
        }

        checkThat(&failureCount, "a name nothing carries is not found",
                  resourceIndexFindNamed(&index, (Unsigned32)PACKAGE_TYPE_TXTR, "no_such_txtr") ==
                      NULL_POINTER);
        /* The same name under a type the disc does have, which must not match:
           a lookup that ignored the type would return the texture. */
        checkThat(&failureCount, "and the right name under the wrong type is not either",
                  resourceIndexFindNamed(&index, (Unsigned32)PACKAGE_TYPE_GMDC, "brick_dxt1_txtr") ==
                      NULL_POINTER);
    }

    printf("\n-- and again through a store that answers every other read --\n");
    {
        ResourceIndex stuttered;
        MemorySize marker = memoryArenaGetMarker(&arena);

        store.stutters = BOOLEAN_TRUE;
        store.withhold = BOOLEAN_FALSE;
        store.pendingCount = 0U;
        buildIndex(&stuttered, &fileSystem, &arena, 64U);

        checkThat(&failureCount, "the store did withhold", store.pendingCount > 0U);
        /* The same answer, arrived at through twice as many steps. A reader
           that lost its place on a pend would index fewer packages, or the
           same package twice. */
        checkThat(&failureCount, "the same packages are indexed", stuttered.filesIndexed == 2U);
        checkThat(&failureCount, "the same textures are found", stuttered.count == 4U);
        checkThat(&failureCount, "and the same one is findable",
                  resourceIndexFindNamed(&stuttered, (Unsigned32)PACKAGE_TYPE_TXTR,
                                         "brick_dxt1_txtr") != NULL_POINTER);
        memoryArenaRewindToMarker(&arena, marker);
        store.stutters = BOOLEAN_FALSE;
    }

    printf("\n-- the census of what is actually on the disc --\n");
    {
        Unsigned32 typeIdentifier = 0U;
        Unsigned32 howMany = 0U;
        Unsigned32 rank;
        Unsigned32 tallied = 0U;

        /* Six kinds between them: textures and a mip level in one, and a
           resource node, shape, geometry node and geometry container in the
           other. Counted off the fixtures' own indices rather than guessed —
           the first attempt at this line said five, because the mip level was
           forgotten while the four textures beside it were not. */
        checkThat(&failureCount, "every entry was counted",
                  index.entriesSeen == 5U + 4U);
        checkThat(&failureCount, "and none overflowed the census",
                  index.censusOverflow == 0U);
        checkThat(&failureCount, "six distinct types across the two packages",
                  index.censusCount == 6U);

        checkThat(&failureCount, "the most common is the texture, with four",
                  resourceIndexGetCensusRank(&index, 0U, &typeIdentifier, &howMany) &&
                      typeIdentifier == (Unsigned32)PACKAGE_TYPE_TXTR && howMany == 4U);

        /* Every rank is filled exactly once and the counts never rise, which is
           what makes the ordering an ordering rather than an arbitrary walk. */
        for (rank = 0U; rank < index.censusCount; rank++)
        {
            Unsigned32 thisType = 0U;
            Unsigned32 thisCount = 0U;

            if (!resourceIndexGetCensusRank(&index, rank, &thisType, &thisCount))
            {
                break;
            }
            if (thisCount > howMany)
            {
                checkThat(&failureCount, "the census never rises as rank falls", BOOLEAN_FALSE);
            }
            howMany = thisCount;
            tallied += thisCount;
        }
        checkThat(&failureCount, "every rank is filled", tallied == index.entriesSeen);
        checkThat(&failureCount, "and there is no rank past the last",
                  !resourceIndexGetCensusRank(&index, index.censusCount, &typeIdentifier, &howMany));
    }

    printf("\n-- an index with no room says so --\n");
    {
        ResourceIndex small;
        MemorySize marker = memoryArenaGetMarker(&arena);

        buildIndex(&small, &fileSystem, &arena, 2U);
        checkThat(&failureCount, "it keeps what fits", small.count == 2U);
        /* Two dropped rather than a silent truncation, so a lookup that fails
           because the index was full can be told from one that fails because
           the disc does not hold the resource. */
        checkThat(&failureCount, "and counts what it could not keep", small.dropped == 2U);
        memoryArenaRewindToMarker(&arena, marker);
    }

    return checkSummarize(failureCount, "resource index");
}
