/* Reads the teapot's geometry out of the fixture package and checks it against
   what the model actually is.

   The numbers below were established by decoding the file independently before
   the reader existed, not by running the reader and writing down what it said.
   That distinction is the whole value of the test: a reader checked against its
   own output agrees with itself and nothing else.

   The teapot is upstream's test model, carrying the same GMDC structures a
   retail mesh does — the same elements, the same component and primitive
   layout, the same index width. What it does not carry is compression, which is
   the one thing standing between this and a mesh off a retail disc. */

#include <stdio.h>

#include "utils/assert.h"
#include "utils/strings.h"
#include "victoria/geometryReader.h"
#include "victoria/memoryArena.h"
#include "victoria/packageReader.h"

#define FILE_BUFFER_CAPACITY (2UL * 1024UL * 1024UL)
#define ARENA_CAPACITY (4UL * 1024UL * 1024UL)

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

static Boolean nearly(Real32 value, Real32 expected)
{
    Real32 difference = value - expected;

    if (difference < 0.0f)
    {
        difference = -difference;
    }
    return difference < 0.001f ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

/* An authored container, for the layouts the fixture cannot reach.

   The teapot is block version 4, so it exercises half word indices and nothing
   else. Retail character meshes are older and store their indices as full
   words — 238 of the 282 meshes on the user's disc were refused for being that
   version — and there is no fixture on hand that is one. So one is written
   here, byte by byte, from the layout rather than from anything this reader
   produced. */

#define BUILT_CAPACITY 512UL

typedef struct Builder
{
    Unsigned8 bytes[BUILT_CAPACITY];
    MemorySize length;
} Builder;

static void putUnsigned8(Builder *builder, Unsigned8 value)
{
    if (builder->length < BUILT_CAPACITY)
    {
        builder->bytes[builder->length] = value;
        builder->length++;
    }
}

static void putUnsigned32(Builder *builder, Unsigned32 value)
{
    putUnsigned8(builder, (Unsigned8)(value & 0xFFU));
    putUnsigned8(builder, (Unsigned8)((value >> 8) & 0xFFU));
    putUnsigned8(builder, (Unsigned8)((value >> 16) & 0xFFU));
    putUnsigned8(builder, (Unsigned8)((value >> 24) & 0xFFU));
}

static void putReal32(Builder *builder, Real32 value)
{
    union
    {
        Unsigned32 word;
        Real32 value;
    } converter;

    converter.value = value;
    putUnsigned32(builder, converter.word);
}

/* Short names only, which is every name here, so one length byte is right. */
static void putString(Builder *builder, const char *text)
{
    MemorySize length = stringLength(text);
    MemorySize index;

    putUnsigned8(builder, (Unsigned8)length);
    for (index = 0UL; index < length; index++)
    {
        putUnsigned8(builder, (Unsigned8)text[index]);
    }
}

static void putTypeInformation(Builder *builder, const char *name, Unsigned32 identifier,
                               Unsigned32 version)
{
    putString(builder, name);
    putUnsigned32(builder, identifier);
    putUnsigned32(builder, version);
}

/* Word wide, which is what a container below block version 3 uses. */
static void putWordIndexArray(Builder *builder, const Unsigned32 *values, Unsigned32 count)
{
    Unsigned32 index;

    putUnsigned32(builder, count);
    for (index = 0U; index < count; index++)
    {
        putUnsigned32(builder, values[index]);
    }
}

static void buildWordIndexedContainer(Builder *builder, Unsigned32 blockVersion)
{
    static const Real32 positions[9] = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 2.0f, 0.0f };
    static const Unsigned32 elementIndices[2] = { 0U, 1U };
    static const Unsigned32 faces[3] = { 0U, 1U, 2U };
    Unsigned32 index;

    builder->length = 0UL;

    putUnsigned32(builder, 0xFFFF0001UL);
    putUnsigned32(builder, 0U); /* no file links */
    putUnsigned32(builder, 1U); /* one block */
    putUnsigned32(builder, 0xAC4F8687UL);

    putTypeInformation(builder, "cGeometryDataContainer", 0xAC4F8687UL, blockVersion);
    putTypeInformation(builder, "cSGResource", 0xACE46235UL, 2U);
    putString(builder, "body_tslocator_gmdc");

    putUnsigned32(builder, 2U); /* two elements */

    /* Positions: three vertices of three floats. */
    putUnsigned32(builder, 0U);
    putUnsigned32(builder, 0x5B830781UL);
    putUnsigned32(builder, 0U);
    putUnsigned32(builder, 2U); /* three floats */
    putUnsigned32(builder, 0U);
    putUnsigned32(builder, 36U);
    for (index = 0U; index < 9U; index++)
    {
        putReal32(builder, positions[index]);
    }
    putWordIndexArray(builder, NULL_POINTER, 0U);

    /* Normals, all straight up, so unit length proves the stride. */
    putUnsigned32(builder, 0U);
    putUnsigned32(builder, 0x3B83078BUL);
    putUnsigned32(builder, 0U);
    putUnsigned32(builder, 2U);
    putUnsigned32(builder, 0U);
    putUnsigned32(builder, 36U);
    for (index = 0U; index < 3U; index++)
    {
        putReal32(builder, 0.0f);
        putReal32(builder, 0.0f);
        putReal32(builder, 1.0f);
    }
    putWordIndexArray(builder, NULL_POINTER, 0U);

    putUnsigned32(builder, 1U); /* one component */
    putWordIndexArray(builder, elementIndices, 2U);
    putUnsigned32(builder, 3U); /* three vertices */
    putUnsigned32(builder, 0U);
    putWordIndexArray(builder, NULL_POINTER, 0U);
    putWordIndexArray(builder, NULL_POINTER, 0U);
    putWordIndexArray(builder, NULL_POINTER, 0U);

    putUnsigned32(builder, 1U); /* one primitive */
    putUnsigned32(builder, 0U);
    putUnsigned32(builder, 0U);
    putString(builder, "body");
    putWordIndexArray(builder, faces, 3U);
}

int main(void)
{
    MemoryArena arena;
    Package package;
    GeometryMesh mesh;
    const PackageResource *resource;
    const Unsigned8 *resourceBytes;
    GeometryReadResult result;
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

    printf("-- finding the geometry --\n");
    resource = packageReaderFindFirstOfType(&package, (Unsigned32)PACKAGE_TYPE_GMDC);
    checkThat(&failureCount, "the package holds a geometry container", resource != NULL_POINTER);
    if (resource == NULL_POINTER)
    {
        return checkSummarize(failureCount, "geometry reader");
    }
    checkThat(&failureCount, "nothing in the fixture is compressed",
              !packageReaderHasCompressedResources(&package));

    resourceBytes = packageReaderGetResourceBytes(&package, resource);
    checkThat(&failureCount, "its bytes are inside the file", resourceBytes != NULL_POINTER);

    printf("\n-- reading it --\n");
    result = geometryReaderOpen(&mesh, resourceBytes, (MemorySize)resource->sizeInBytes, &arena);
    checkThat(&failureCount, "the geometry reader accepts it", result == GEOMETRY_READ_OK);
    if (result != GEOMETRY_READ_OK)
    {
        printf("  result: %s\n", geometryReadResultGetName(result));
        return checkSummarize(failureCount, "geometry reader");
    }

    checkThat(&failureCount, "names the resource teapot_tslocator_gmdc",
              stringEquals(mesh.resourceName, "teapot_tslocator_gmdc"));
    checkThat(&failureCount, "names the primitive teapot", stringEquals(mesh.name, "teapot"));
    checkThat(&failureCount, "the model is one primitive", mesh.primitiveCount == 1U);

    checkThat(&failureCount, "finds 13248 vertices", mesh.vertexCount == 13248U);
    checkThat(&failureCount, "finds 18960 indices", mesh.indexCount == 18960U);
    checkThat(&failureCount, "which is 6320 triangles", mesh.indexCount % 3U == 0U);
    checkThat(&failureCount, "carries normals", mesh.normals != NULL_POINTER);
    checkThat(&failureCount, "carries texture coordinates", mesh.textureCoordinates != NULL_POINTER);

    printf("\n-- is it the right shape --\n");
    {
        Real32 minimum[3];
        Real32 maximum[3];

        geometryMeshGetBounds(&mesh, minimum, maximum);
        printf("  bounds x[%.3f, %.3f] y[%.3f, %.3f] z[%.3f, %.3f]\n", (double)minimum[0],
               (double)maximum[0], (double)minimum[1], (double)maximum[1], (double)minimum[2],
               (double)maximum[2]);

        /* A teapot four units wide, six and a half deep, and sitting on z = 0
           with its spout and handle reaching out either side. Getting the stride
           or the component count wrong scrambles these long before it produces
           a plausible box. */
        checkThat(&failureCount, "spans four units across", nearly(minimum[0], -2.0f) && nearly(maximum[0], 2.0f));
        checkThat(&failureCount, "reaches from -3 to 3.434 front to back",
                  nearly(minimum[1], -3.0f) && nearly(maximum[1], 3.434f));
        checkThat(&failureCount, "sits on the ground", nearly(minimum[2], 0.0f));
        checkThat(&failureCount, "stands 3.15 units tall", nearly(maximum[2], 3.15f));
    }

    printf("\n-- are the triangles usable --\n");
    {
        Unsigned32 index;
        Boolean allInRange = BOOLEAN_TRUE;
        Boolean anyDegenerate = BOOLEAN_FALSE;

        for (index = 0U; index + 2U < mesh.indexCount; index += 3U)
        {
            Unsigned16 first = mesh.indices[index];
            Unsigned16 second = mesh.indices[index + 1U];
            Unsigned16 third = mesh.indices[index + 2U];

            if (first >= mesh.vertexCount || second >= mesh.vertexCount || third >= mesh.vertexCount)
            {
                allInRange = BOOLEAN_FALSE;
            }
            if (first == second || second == third || first == third)
            {
                anyDegenerate = BOOLEAN_TRUE;
            }
        }
        checkThat(&failureCount, "every index addresses a vertex that exists", allInRange);
        checkThat(&failureCount, "no triangle collapses to a line or a point", !anyDegenerate);
    }

    printf("\n-- are the normals normals --\n");
    {
        Unsigned32 index;
        Unsigned32 offCount = 0U;

        for (index = 0U; index < mesh.vertexCount; index++)
        {
            const Real32 *normal = &mesh.normals[(MemorySize)index * 3UL];
            Real32 lengthSquared =
                normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2];

            if (lengthSquared < 0.98f || lengthSquared > 1.02f)
            {
                offCount++;
            }
        }
        /* Unit length is not something a misread buffer produces by accident,
           which makes it the cheapest proof that the floats are being assembled
           the right way round. */
        checkThat(&failureCount, "every normal is unit length", offCount == 0U);
        if (offCount != 0U)
        {
            printf("  %u of %u were not\n", (unsigned)offCount, (unsigned)mesh.vertexCount);
        }
    }

    printf("\n-- an older container, indices a word wide --\n");
    {
        static Builder builder;
        GeometryMesh older;
        Unsigned32 version;

        /* Both versions below the narrowing, so a reader that only handles the
           new width fails here rather than on one of them by luck. */
        for (version = 1U; version <= 2U; version++)
        {
            buildWordIndexedContainer(&builder, version);
            result = geometryReaderOpen(&older, builder.bytes, builder.length, &arena);
            checkThat(&failureCount, "reads a container at block version 1 or 2",
                      result == GEOMETRY_READ_OK);
            if (result != GEOMETRY_READ_OK)
            {
                printf("  version %u: %s\n", (unsigned)version, geometryReadResultGetName(result));
                continue;
            }
            checkThat(&failureCount, "names the resource body_tslocator_gmdc",
                      stringEquals(older.resourceName, "body_tslocator_gmdc"));
            checkThat(&failureCount, "finds its three vertices", older.vertexCount == 3U);
            checkThat(&failureCount, "and its one triangle", older.indexCount == 3U);
            checkThat(&failureCount, "with the faces in order",
                      older.indices[0] == 0U && older.indices[1] == 1U && older.indices[2] == 2U);
            /* Reading the element index array two bytes at a time would find
               element 0 twice and never reach the normals. */
            checkThat(&failureCount, "follows the word wide element list to the normals",
                      older.normals != NULL_POINTER);
            {
                Real32 minimum[3];
                Real32 maximum[3];

                geometryMeshGetBounds(&older, minimum, maximum);
                checkThat(&failureCount, "with the positions where they were written",
                          nearly(maximum[0], 1.0f) && nearly(maximum[1], 2.0f) &&
                              nearly(maximum[2], 0.0f));
            }
        }
    }

    printf("\n-- refusing what it cannot read --\n");
    {
        GeometryMesh other;
        static const Unsigned8 notAResource[16] = { 0 };

        checkThat(&failureCount, "rejects bytes that are not a scenegraph resource",
                  geometryReaderOpen(&other, notAResource, sizeof(notAResource), &arena) ==
                      GEOMETRY_READ_NOT_A_RESOURCE);
        checkThat(&failureCount, "rejects a resource that stops part way",
                  geometryReaderOpen(&other, resourceBytes, 64UL, &arena) == GEOMETRY_READ_TRUNCATED);

        {
            /* An older collection mark. Not readable here, but saying so as a
               version rather than as rubbish is the difference between knowing
               what a disc holds and guessing. */
            static const Unsigned8 olderMark[16] = { 0x01U, 0x00U, 0xFEU, 0xFFU, 0U };

            checkThat(&failureCount, "calls an older collection mark a version, not rubbish",
                      geometryReaderOpen(&other, olderMark, sizeof(olderMark), &arena) ==
                          GEOMETRY_READ_UNSUPPORTED_VERSION);
        }

        {
            /* A material definition is a scenegraph resource too, and reading it
               as geometry would produce nonsense rather than an error unless the
               block type is actually checked. */
            MemorySize materialSize = loadFile("testAssets/scenegraph/material_definition.package");
            Package materialPackage;

            if (materialSize > 0UL &&
                packageReaderOpen(&materialPackage, fileBuffer, materialSize, &arena) == PACKAGE_READ_OK)
            {
                const PackageResource *material =
                    packageReaderFindFirstOfType(&materialPackage, (Unsigned32)PACKAGE_TYPE_TXMT);
                const Unsigned8 *materialBytes =
                    packageReaderGetResourceBytes(&materialPackage, material);

                checkThat(&failureCount, "rejects a material definition",
                          geometryReaderOpen(&other, materialBytes, (MemorySize)material->sizeInBytes,
                                             &arena) == GEOMETRY_READ_WRONG_TYPE);
            }
        }
    }

    return checkSummarize(failureCount, "geometry reader");
}
