
#include <stdio.h>

#include "utils/assert.h"
#include "utils/strings.h"
#include "victoria/geometryReader.h"
#include "victoria/memoryArena.h"
#include "victoria/packageReader.h"
#include "utils/resourceHash.h"
#include "victoria/scenegraph.h"

#define FILE_BUFFER_CAPACITY (2UL * 1024UL * 1024UL)
#define ARENA_CAPACITY (4UL * 1024UL * 1024UL)

static Unsigned8 fileBuffer[FILE_BUFFER_CAPACITY];
static Unsigned8 arenaStorage[ARENA_CAPACITY];

static Integer32 failureCount = 0;

int main(void)
{
    MemoryArena arena;
    Package package;
    MemorySize sizeInBytes;
    FILE *inputFile;

    memoryArenaInitialize(&arena, arenaStorage, ARENA_CAPACITY);

    inputFile = fopen("testAssets/scenegraph/teapot_model.package", "rb");
    if (inputFile == NULL)
    {
        printf("FAIL  cannot open the fixture package\n");
        return 1;
    }
    sizeInBytes = (MemorySize)fread(fileBuffer, 1, FILE_BUFFER_CAPACITY, inputFile);
    fclose(inputFile);

    if (packageReaderOpen(&package, fileBuffer, sizeInBytes, &arena) != PACKAGE_READ_OK)
    {
        printf("FAIL  the fixture package would not open\n");
        return 1;
    }

    printf("-- the shape --\n");
    {
        ShapeDescription shape;
        const PackageResource *resource =
            packageReaderFindFirstOfType(&package, (Unsigned32)PACKAGE_TYPE_SHPE);
        ScenegraphReadResult result;

        checkThat(&failureCount, "the package holds a shape", resource != NULL_POINTER);
        if (resource == NULL_POINTER)
        {
            return checkSummarize(failureCount, "scenegraph");
        }

        result = scenegraphReadShape(&shape, packageReaderGetResourceBytes(&package, resource),
                                     (MemorySize)resource->sizeInBytes);
        checkThat(&failureCount, "the shape reader accepts it", result == SCENEGRAPH_READ_OK);
        if (result != SCENEGRAPH_READ_OK)
        {
            printf("  result: %s\n", scenegraphReadResultGetName(result));
            return checkSummarize(failureCount, "scenegraph");
        }

        checkThat(&failureCount, "at block version 8", shape.blockVersion == 8U);
        checkThat(&failureCount, "named ufoCrash_ufo_shpe",
                  stringEquals(shape.resourceName, "ufoCrash_ufo_shpe"));
        checkThat(&failureCount, "naming one mesh", shape.meshCount == 1U);
        checkThat(&failureCount, "which is ufoCrash_tslocator_gmnd",
                  stringEquals(shape.meshNames[0], "ufoCrash_tslocator_gmnd"));

        checkThat(&failureCount, "binding three materials", shape.materialCount == 3U);
        checkThat(&failureCount, "the body to its own material",
                  stringEquals(shape.materials[0].primitiveName, "ufocrash_body") &&
                      stringEquals(shape.materials[0].materialName, "ufocrash_body"));
        checkThat(&failureCount, "the cabin to its own",
                  stringEquals(shape.materials[1].primitiveName, "ufocrash_cabin"));
        checkThat(&failureCount, "and the shadow to its own",
                  stringEquals(shape.materials[2].primitiveName, "neighborhood_roundshadow"));
    }

    printf("\n-- the geometry node, and the hop to the container --\n");
    {
        GeometryNodeDescription node;
        const PackageResource *resource =
            packageReaderFindFirstOfType(&package, (Unsigned32)PACKAGE_TYPE_GMND);
        const PackageResource *geometry;
        ScenegraphReadResult result;

        checkThat(&failureCount, "the package holds a geometry node", resource != NULL_POINTER);
        if (resource == NULL_POINTER)
        {
            return checkSummarize(failureCount, "scenegraph");
        }

        result = scenegraphReadGeometryNode(&node, packageReaderGetResourceBytes(&package, resource),
                                            (MemorySize)resource->sizeInBytes);
        checkThat(&failureCount, "the geometry node reader accepts it", result == SCENEGRAPH_READ_OK);
        if (result != SCENEGRAPH_READ_OK)
        {
            printf("  result: %s\n", scenegraphReadResultGetName(result));
            return checkSummarize(failureCount, "scenegraph");
        }

        checkThat(&failureCount, "at block version 12", node.blockVersion == 12U);
        checkThat(&failureCount, "named teapot_tslocator_gmnd",
                  stringEquals(node.resourceName, "teapot_tslocator_gmnd"));
        checkThat(&failureCount, "carrying a reference to its geometry", node.hasGeometry);

        checkThat(&failureCount, "which points at a geometry container",
                  node.geometryKey.typeIdentifier == 0xAC4F8687UL);
        checkThat(&failureCount, "in group 1C0532FA", node.geometryKey.groupIdentifier == 0x1C0532FAUL);
        checkThat(&failureCount, "at instance FF515126",
                  node.geometryKey.instanceIdentifier == 0xFF515126UL);
        checkThat(&failureCount, "high word 3AFDFD2C",
                  node.geometryKey.instanceIdentifierHigh == 0x3AFDFD2CUL);

        printf("\n-- and the key finds the resource --\n");
        geometry = scenegraphFindResource(&package, &node.geometryKey);
        checkThat(&failureCount, "the key resolves to a resource in the package",
                  geometry != NULL_POINTER);
        checkThat(&failureCount, "and it is the same one a type search finds",
                  geometry == packageReaderFindFirstOfType(&package, (Unsigned32)PACKAGE_TYPE_GMDC));

        if (geometry != NULL_POINTER)
        {
            GeometryMesh mesh;

            checkThat(&failureCount, "which reads as geometry",
                      geometryReaderOpen(&mesh, packageReaderGetResourceBytes(&package, geometry),
                                         (MemorySize)geometry->sizeInBytes, &arena) ==
                          GEOMETRY_READ_OK);
            checkThat(&failureCount, "and is the teapot", mesh.vertexCount == 13248U);
        }
    }

    printf("\n-- is a resource's key its name, hashed --\n");
    {
        checkThat(&failureCount, "the geometry container's instance is its name hashed",
                  resourceHashInstance("teapot_tslocator_gmdc") == 0xFF515126UL);
        checkThat(&failureCount, "and its high word too",
                  resourceHashInstanceHigh("teapot_tslocator_gmdc") == 0x3AFDFD2CUL);
        checkThat(&failureCount, "the geometry node's instance likewise",
                  resourceHashInstance("teapot_tslocator_gmnd") == 0xFF3BD317UL);
        checkThat(&failureCount, "and its high word",
                  resourceHashInstanceHigh("teapot_tslocator_gmnd") == 0x0E007074UL);
        checkThat(&failureCount, "the shape's instance",
                  resourceHashInstance("ufoCrash_ufo_shpe") == 0xFFA11ECBUL);
        checkThat(&failureCount, "and the shape's high word",
                  resourceHashInstanceHigh("ufoCrash_ufo_shpe") == 0x78BFA44EUL);

        checkThat(&failureCount, "case makes no difference to the hash",
                  resourceHashInstance("UFOCRASH_UFO_SHPE") == 0xFFA11ECBUL &&
                      resourceHashInstanceHigh("ufocrash_ufo_shpe") == 0x78BFA44EUL);

        checkThat(&failureCount, "a group hash is the same CRC under a different mask",
                  (resourceHashGroup("teapot_tslocator_gmdc") & 0x00FFFFFFUL) ==
                          (resourceHashInstance("teapot_tslocator_gmdc") & 0x00FFFFFFUL) &&
                      (resourceHashGroup("teapot_tslocator_gmdc") >> 24) == 0x7FUL);

        checkThat(&failureCount, "a one character difference changes the key",
                  resourceHashInstance("teapot_tslocator_gmdc") !=
                      resourceHashInstance("teapot_tslocator_gmdd"));
    }

    printf("\n-- looking a container up by the name a shape would give --\n");
    {
        const PackageResource *found =
            scenegraphFindGeometryNamed(&arena, &package, "teapot_tslocator_gmnd");

        checkThat(&failureCount, "a node named by a shape resolves to its container",
                  found == packageReaderFindFirstOfType(&package, (Unsigned32)PACKAGE_TYPE_GMDC));
        checkThat(&failureCount, "and a name no node carries resolves to nothing",
                  scenegraphFindGeometryNamed(&arena, &package, "no_such_gmnd") == NULL_POINTER);
        checkThat(&failureCount, "a name that is only a prefix does not match",
                  scenegraphFindGeometryNamed(&arena, &package, "teapot") == NULL_POINTER);

        checkThat(&failureCount, "the node is findable by its hashed key alone",
                  scenegraphFindResourceByInstance(
                      &package, (Unsigned32)PACKAGE_TYPE_GMND,
                      resourceHashInstance("teapot_tslocator_gmnd"),
                      resourceHashInstanceHigh("teapot_tslocator_gmnd")) ==
                      packageReaderFindFirstOfType(&package, (Unsigned32)PACKAGE_TYPE_GMND));
        checkThat(&failureCount, "and a name no resource carries hashes to nothing here",
                  scenegraphFindResourceByInstance(&package, (Unsigned32)PACKAGE_TYPE_GMND,
                                                   resourceHashInstance("no_such_gmnd"),
                                                   resourceHashInstanceHigh("no_such_gmnd")) ==
                      NULL_POINTER);
    }

    printf("\n-- the arena is left where it was found --\n");
    {
        MemorySize before = memoryArenaGetMarker(&arena);

        (void)scenegraphFindGeometryNamed(&arena, &package, "teapot_tslocator_gmnd");
        (void)scenegraphFindGeometryNamed(&arena, &package, "no_such_gmnd");
        checkThat(&failureCount, "scanning for a name costs the arena nothing",
                  memoryArenaGetMarker(&arena) == before);
    }

    printf("\n-- refusing what it should --\n");
    {
        ShapeDescription shape;
        GeometryNodeDescription node;
        const PackageResource *geometry =
            packageReaderFindFirstOfType(&package, (Unsigned32)PACKAGE_TYPE_GMDC);
        const Unsigned8 *geometryBytes = packageReaderGetResourceBytes(&package, geometry);
        static const Unsigned8 notAResource[16] = { 0 };
        PackageResourceKey absent;

        checkThat(&failureCount, "a shape reader refuses a geometry container",
                  scenegraphReadShape(&shape, geometryBytes, (MemorySize)geometry->sizeInBytes) ==
                      SCENEGRAPH_READ_WRONG_TYPE);
        checkThat(&failureCount, "a geometry node reader refuses one too",
                  scenegraphReadGeometryNode(&node, geometryBytes,
                                             (MemorySize)geometry->sizeInBytes) ==
                      SCENEGRAPH_READ_WRONG_TYPE);
        checkThat(&failureCount, "and both refuse bytes that are not a resource",
                  scenegraphReadShape(&shape, notAResource, sizeof(notAResource)) ==
                      SCENEGRAPH_READ_NOT_A_RESOURCE);
        checkThat(&failureCount, "a truncated resource is refused, not guessed at",
                  scenegraphReadGeometryNode(&node, geometryBytes, 8UL) != SCENEGRAPH_READ_OK);

        absent.typeIdentifier = 0xAC4F8687UL;
        absent.groupIdentifier = 0x1C0532FAUL;
        absent.instanceIdentifier = 0xFF515126UL;
        absent.instanceIdentifierHigh = 0x00000001UL;
        checkThat(&failureCount, "a key differing only in the high word finds nothing",
                  scenegraphFindResource(&package, &absent) == NULL_POINTER);
    }

    return checkSummarize(failureCount, "scenegraph");
}
