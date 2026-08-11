
#include <stdio.h>

#include "utils/assert.h"
#include "utils/strings.h"
#include "victoria/memoryArena.h"
#include "victoria/packageReader.h"
#include "victoria/resourceCollection.h"

#define FILE_BUFFER_CAPACITY (1024UL * 1024UL)
#define ARENA_CAPACITY (256UL * 1024UL)

static Unsigned8 fileBuffer[FILE_BUFFER_CAPACITY];
static Unsigned8 arenaStorage[ARENA_CAPACITY];

static Integer32 failureCount = 0;

static MemorySize loadFile(const char *path)
{
    FILE *inputFile = fopen(path, "rb");
    MemorySize sizeInBytes;

    if (inputFile == NULL)
    {
        printf("FAIL  cannot open %s\n", path);
        failureCount += 1;
        return 0UL;
    }
    sizeInBytes = (MemorySize)fread(fileBuffer, 1, FILE_BUFFER_CAPACITY, inputFile);
    fclose(inputFile);
    return sizeInBytes;
}

static Boolean keysMatch(const PackageResourceKey *first, const PackageResourceKey *second)
{
    return (first->typeIdentifier == second->typeIdentifier &&
            first->groupIdentifier == second->groupIdentifier &&
            first->instanceIdentifier == second->instanceIdentifier &&
            first->instanceIdentifierHigh == second->instanceIdentifierHigh)
               ? BOOLEAN_TRUE
               : BOOLEAN_FALSE;
}

static Boolean openResource(const Package *package, Unsigned32 typeIdentifier,
                            ResourceCollection *collection, ResourceCursor *cursor)
{
    const PackageResource *resource = packageReaderFindFirstOfType(package, typeIdentifier);
    const Unsigned8 *resourceBytes = packageReaderGetResourceBytes(package, resource);

    if (resource == NULL_POINTER || resourceBytes == NULL_POINTER)
    {
        return BOOLEAN_FALSE;
    }
    return (resourceCollectionOpen(collection, cursor, resourceBytes,
                                   (MemorySize)resource->sizeInBytes) == RESOURCE_COLLECTION_OK)
               ? BOOLEAN_TRUE
               : BOOLEAN_FALSE;
}

int main(void)
{
    MemoryArena arena;
    Package package;
    MemorySize sizeInBytes;

    memoryArenaInitialize(&arena, arenaStorage, ARENA_CAPACITY);

    sizeInBytes = loadFile("testAssets/scenegraph/teapot_model.package");
    if (sizeInBytes == 0UL)
    {
        return 1;
    }
    if (packageReaderOpen(&package, fileBuffer, sizeInBytes, &arena) != PACKAGE_READ_OK)
    {
        printf("FAIL  the fixture package would not open\n");
        return 1;
    }

    printf("-- the fixture carries a whole model --\n");
    checkThat(&failureCount, "a resource node",
              packageReaderFindFirstOfType(&package, (Unsigned32)PACKAGE_TYPE_CRES) != NULL_POINTER);
    checkThat(&failureCount, "a shape",
              packageReaderFindFirstOfType(&package, (Unsigned32)PACKAGE_TYPE_SHPE) != NULL_POINTER);
    checkThat(&failureCount, "a geometry node",
              packageReaderFindFirstOfType(&package, (Unsigned32)PACKAGE_TYPE_GMND) != NULL_POINTER);
    checkThat(&failureCount, "and the geometry itself",
              packageReaderFindFirstOfType(&package, (Unsigned32)PACKAGE_TYPE_GMDC) != NULL_POINTER);

    printf("\n-- reading the geometry node's wrapper --\n");
    {
        ResourceCollection collection;
        ResourceCursor cursor;

        if (!openResource(&package, (Unsigned32)PACKAGE_TYPE_GMND, &collection, &cursor))
        {
            printf("FAIL  the geometry node's collection would not open\n");
            return checkSummarize(failureCount + 1, "resource collection");
        }

        checkThat(&failureCount, "marked 0xFFFF0001",
                  collection.versionMark == (Unsigned32)RESOURCE_COLLECTION_MARK);
        checkThat(&failureCount, "holding one block", collection.blockCount == 1U);
        checkThat(&failureCount, "and that block is a geometry node",
                  collection.firstBlockType == (Unsigned32)PACKAGE_TYPE_GMND);
        checkThat(&failureCount, "with one link out", collection.linkCount == 1U);
        checkThat(&failureCount, "which was kept", collection.storedLinkCount == 1U);

        printf("\n-- does the link land on the geometry --\n");
        {
            const PackageResource *geometry =
                packageReaderFindFirstOfType(&package, (Unsigned32)PACKAGE_TYPE_GMDC);
            ObjectReference reference;
            const PackageResourceKey *link;

            reference.kind = OBJECT_REFERENCE_EXTERNAL;
            reference.index = 0;
            link = resourceCollectionGetLink(&collection, reference);

            checkThat(&failureCount, "the link resolves", link != NULL_POINTER);
            if (link != NULL_POINTER && geometry != NULL_POINTER)
            {
                printf("  link  %08lX %08lX %08lX %08lX\n", (unsigned long)link->typeIdentifier,
                       (unsigned long)link->groupIdentifier, (unsigned long)link->instanceIdentifier,
                       (unsigned long)link->instanceIdentifierHigh);
                checkThat(&failureCount, "onto the geometry container's index entry",
                          keysMatch(link, &geometry->key));
            }

            reference.kind = OBJECT_REFERENCE_EXTERNAL;
            reference.index = 7;
            checkThat(&failureCount, "a link past the end resolves to nothing",
                      resourceCollectionGetLink(&collection, reference) == NULL_POINTER);
            reference.kind = OBJECT_REFERENCE_INTERNAL;
            reference.index = 0;
            checkThat(&failureCount, "and an internal reference is not a link",
                      resourceCollectionGetLink(&collection, reference) == NULL_POINTER);
        }
    }

    printf("\n-- reading the shape's wrapper --\n");
    {
        ResourceCollection collection;
        ResourceCursor cursor;
        PersistTypeInfo blockType;

        if (!openResource(&package, (Unsigned32)PACKAGE_TYPE_SHPE, &collection, &cursor))
        {
            printf("FAIL  the shape's collection would not open\n");
            return checkSummarize(failureCount + 1, "resource collection");
        }
        checkThat(&failureCount, "the shape names no other file", collection.linkCount == 0U);

        resourceCursorReadTypeInformation(&cursor, &blockType);
        checkThat(&failureCount, "the cursor stops at the first block",
                  blockType.typeIdentifier == collection.firstBlockType);
        checkThat(&failureCount, "which calls itself cShape",
                  stringEquals(blockType.name, "cShape"));
        checkThat(&failureCount, "at version 8", blockType.version == 8U);
    }

    printf("\n-- reading the resource node's wrapper --\n");
    {
        ResourceCollection collection;
        ResourceCursor cursor;

        if (!openResource(&package, (Unsigned32)PACKAGE_TYPE_CRES, &collection, &cursor))
        {
            printf("FAIL  the resource node's collection would not open\n");
            return checkSummarize(failureCount + 1, "resource collection");
        }
        checkThat(&failureCount, "the resource node holds three blocks", collection.blockCount == 3U);
        checkThat(&failureCount, "the first of which is the resource node",
                  collection.firstBlockType == (Unsigned32)PACKAGE_TYPE_CRES);
        checkThat(&failureCount, "and it links out to the shape", collection.linkCount == 1U);
        {
            ObjectReference reference;
            const PackageResourceKey *link;
            const PackageResource *shape =
                packageReaderFindFirstOfType(&package, (Unsigned32)PACKAGE_TYPE_SHPE);

            reference.kind = OBJECT_REFERENCE_EXTERNAL;
            reference.index = 0;
            link = resourceCollectionGetLink(&collection, reference);
            checkThat(&failureCount, "which is the shape in this same package",
                      link != NULL_POINTER && shape != NULL_POINTER && keysMatch(link, &shape->key));
        }
    }

    printf("\n-- refusing what it should --\n");
    {
        ResourceCollection collection;
        ResourceCursor cursor;
        static const Unsigned8 rubbish[16] = { 0 };
        static const Unsigned8 olderMark[16] = { 0x01U, 0x00U, 0xFEU, 0xFFU, 0U };
        static const Unsigned8 tooShort[2] = { 0x01U, 0x00U };

        checkThat(&failureCount, "rejects bytes that are not a collection",
                  resourceCollectionOpen(&collection, &cursor, rubbish, sizeof(rubbish)) ==
                      RESOURCE_COLLECTION_NOT_A_RESOURCE);
        checkThat(&failureCount, "names an older collection rather than calling it rubbish",
                  resourceCollectionOpen(&collection, &cursor, olderMark, sizeof(olderMark)) ==
                      RESOURCE_COLLECTION_OLDER);
        checkThat(&failureCount, "and still reports its mark", collection.versionMark == 0xFFFE0001UL);
        checkThat(&failureCount, "rejects a resource too short to hold a mark",
                  resourceCollectionOpen(&collection, &cursor, tooShort, sizeof(tooShort)) ==
                      RESOURCE_COLLECTION_TRUNCATED);
        checkThat(&failureCount, "rejects no bytes at all",
                  resourceCollectionOpen(&collection, &cursor, NULL_POINTER, 0UL) ==
                      RESOURCE_COLLECTION_TRUNCATED);
    }

    printf("\n-- a link count no resource could hold --\n");
    {
        static Unsigned8 claimsTooMany[12] = { 0x01U, 0x00U, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
                                               0xFFU, 0x7FU, 0x00U, 0x00U, 0x00U, 0x00U };
        ResourceCollection collection;
        ResourceCursor cursor;

        checkThat(&failureCount, "refuses a link count the resource cannot hold",
                  resourceCollectionOpen(&collection, &cursor, claimsTooMany,
                                         sizeof(claimsTooMany)) == RESOURCE_COLLECTION_TRUNCATED);
    }

    return checkSummarize(failureCount, "resource collection");
}
