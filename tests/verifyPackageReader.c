/* Reads the real fixtures in testAssets/ and checks the package reader against
   what is actually inside them.

   These are the packages upstream authored for testing, not mocks: a Utah
   teapot and the Creative Commons Zero mark, carrying the same DBPF structures
   a retail disc does. Reading them here is the point — a hand-written mock
   would only prove the reader agrees with my own assumptions.

   File I/O lives here rather than in the engine. The reader is handed bytes,
   which is what lets it be identical on a platform with no filesystem. */

#include <stdio.h>

#include "utils/assert.h"
#include "victoria/memoryArena.h"
#include "victoria/packageReader.h"

/* Large enough for the biggest fixture, which is the teapot's geometry at just
   under 700 KiB. */
#define FILE_BUFFER_CAPACITY (2UL * 1024UL * 1024UL)
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

/* Opens a fixture and reports how many resources of a given type it holds. */
static Boolean openFixture(const char *path, Package *package, MemoryArena *arena)
{
    MemorySize sizeInBytes = loadFile(path);
    PackageReadResult result;

    if (sizeInBytes == 0UL)
    {
        return BOOLEAN_FALSE;
    }

    memoryArenaInitialize(arena, arenaStorage, ARENA_CAPACITY);
    result = packageReaderOpen(package, fileBuffer, sizeInBytes, arena);
    if (result != PACKAGE_READ_OK)
    {
        printf("FAIL  %s: %s\n", path, packageReadResultGetName(result));
        failureCount += 1;
        return BOOLEAN_FALSE;
    }
    return BOOLEAN_TRUE;
}

static void checkTypeCount(const Package *package, const char *typeName, Unsigned32 typeIdentifier,
                           Unsigned32 expectedCount)
{
    char description[96];
    Unsigned32 actual = packageReaderCountResourcesOfType(package, typeIdentifier);

    /* snprintf is available here because this is a host program, not engine
       code; the engine formats without it. */
    snprintf(description, sizeof(description), "holds %u %s (found %u)", expectedCount, typeName, actual);
    checkThat(&failureCount, description, actual == expectedCount);
}

static void verifyTeapotModel(void)
{
    Package package;
    MemoryArena arena;

    printf("\n-- teapot_model.package --\n");
    if (openFixture("testAssets/scenegraph/teapot_model.package", &package, &arena) == BOOLEAN_FALSE)
    {
        return;
    }

    checkThat(&failureCount, "reports DBPF 1.1", package.majorVersion == 1U && package.minorVersion == 1U);
    checkThat(&failureCount, "finds four resources", package.resourceCount == 4U);

    /* The chain the renderer has to walk, all four in one package. */
    checkTypeCount(&package, "CRES", (Unsigned32)PACKAGE_TYPE_CRES, 1U);
    checkTypeCount(&package, "SHPE", (Unsigned32)PACKAGE_TYPE_SHPE, 1U);
    checkTypeCount(&package, "GMND", (Unsigned32)PACKAGE_TYPE_GMND, 1U);
    checkTypeCount(&package, "GMDC", (Unsigned32)PACKAGE_TYPE_GMDC, 1U);

    checkThat(&failureCount, "has no compression directory",
          packageReaderHasCompressedResources(&package) == BOOLEAN_FALSE);

    {
        const PackageResource *geometry =
            packageReaderFindFirstOfType(&package, (Unsigned32)PACKAGE_TYPE_GMDC);
        checkThat(&failureCount, "finds the geometry resource", geometry != NULL_POINTER);
        if (geometry != NULL_POINTER)
        {
            /* The geometry is nearly the whole file; anything much smaller
               would mean the index was misread. */
            checkThat(&failureCount, "geometry is the bulk of the package",
                  geometry->sizeInBytes > 600000U && geometry->sizeInBytes < package.sizeInBytes);
            checkThat(&failureCount, "geometry bytes are inside the file",
                  packageReaderGetResourceBytes(&package, geometry) != NULL_POINTER);
        }
    }

    {
        /* Every resource shares a group in this package, and each has its own
           instance. Distinct instances matter: identity is the whole tuple. */
        Unsigned32 index;
        Boolean allShareGroup = BOOLEAN_TRUE;
        Boolean allInstancesDiffer = BOOLEAN_TRUE;

        for (index = 1U; index < package.resourceCount; index += 1U)
        {
            if (package.resources[index].key.groupIdentifier !=
                package.resources[0].key.groupIdentifier)
            {
                allShareGroup = BOOLEAN_FALSE;
            }
            if (package.resources[index].key.instanceIdentifier ==
                package.resources[0].key.instanceIdentifier)
            {
                allInstancesDiffer = BOOLEAN_FALSE;
            }
        }
        checkThat(&failureCount, "all resources share one group", allShareGroup == BOOLEAN_TRUE);
        checkThat(&failureCount, "every resource has its own instance", allInstancesDiffer == BOOLEAN_TRUE);
    }
}

static void verifyTextures(void)
{
    Package package;
    MemoryArena arena;

    printf("\n-- textures.package --\n");
    if (openFixture("testAssets/scenegraph/textures.package", &package, &arena) == BOOLEAN_FALSE)
    {
        return;
    }

    checkThat(&failureCount, "finds five resources", package.resourceCount == 5U);
    checkTypeCount(&package, "TXTR", (Unsigned32)PACKAGE_TYPE_TXTR, 4U);
    checkTypeCount(&package, "LIFO", (Unsigned32)PACKAGE_TYPE_LIFO, 1U);
}

static void verifyMaterialDefinition(void)
{
    Package package;
    MemoryArena arena;

    printf("\n-- material_definition.package --\n");
    if (openFixture("testAssets/scenegraph/material_definition.package", &package, &arena) ==
        BOOLEAN_FALSE)
    {
        return;
    }

    checkThat(&failureCount, "finds one resource", package.resourceCount == 1U);
    checkTypeCount(&package, "TXMT", (Unsigned32)PACKAGE_TYPE_TXMT, 1U);
}

static void verifyAnimation(void)
{
    Package package;
    MemoryArena arena;

    printf("\n-- animation.package --\n");
    if (openFixture("testAssets/scenegraph/animation.package", &package, &arena) == BOOLEAN_FALSE)
    {
        return;
    }

    checkThat(&failureCount, "finds three resources", package.resourceCount == 3U);
    checkTypeCount(&package, "ANIM", (Unsigned32)PACKAGE_TYPE_ANIM, 3U);
}

/* The reader has to reject bad input rather than believe it. Nothing here is
   hypothetical: a truncated download and a mistaken path both produce these. */
static void verifyRejections(void)
{
    Package package;
    MemoryArena arena;
    Unsigned8 scratch[128];
    Unsigned32 index;

    printf("\n-- rejections --\n");

    memoryArenaInitialize(&arena, arenaStorage, ARENA_CAPACITY);
    checkThat(&failureCount, "rejects a buffer too small to hold a header",
          packageReaderOpen(&package, scratch, 8UL, &arena) == PACKAGE_READ_TRUNCATED);

    for (index = 0U; index < sizeof(scratch); index += 1U)
    {
        scratch[index] = 0U;
    }
    memoryArenaInitialize(&arena, arenaStorage, ARENA_CAPACITY);
    checkThat(&failureCount, "rejects something that is not a package",
          packageReaderOpen(&package, scratch, sizeof(scratch), &arena) == PACKAGE_READ_NOT_A_PACKAGE);

    /* A valid header claiming an index beyond the end of the file. */
    scratch[0] = 'D';
    scratch[1] = 'B';
    scratch[2] = 'P';
    scratch[3] = 'F';
    scratch[0x24] = 4U;      /* four entries */
    scratch[0x28] = 0x60U;   /* index at 96 */
    scratch[0x2C] = 0xFFU;   /* index size far past the end */
    scratch[0x2D] = 0xFFU;
    memoryArenaInitialize(&arena, arenaStorage, ARENA_CAPACITY);
    checkThat(&failureCount, "rejects an index that runs past the end of the file",
          packageReaderOpen(&package, scratch, sizeof(scratch), &arena) == PACKAGE_READ_TRUNCATED);

    /* An arena with no room left must fail cleanly rather than overrun. */
    {
        MemorySize sizeInBytes = loadFile("testAssets/scenegraph/teapot_model.package");
        MemoryArena tinyArena;
        static Unsigned8 tinyStorage[8];

        memoryArenaInitialize(&tinyArena, tinyStorage, sizeof(tinyStorage));
        checkThat(&failureCount, "refuses when the arena cannot hold the index",
              packageReaderOpen(&package, fileBuffer, sizeInBytes, &tinyArena) ==
                  PACKAGE_READ_OUT_OF_ARENA);
    }
}

int main(void)
{
    printf("package reader checks\n");

    verifyTeapotModel();
    verifyTextures();
    verifyMaterialDefinition();
    verifyAnimation();
    verifyRejections();

    return checkSummarize(failureCount, "package reader");
}
