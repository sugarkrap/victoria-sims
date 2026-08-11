
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

#define BUILT_CAPACITY 4096UL

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

static void putIndexArray(Builder *builder, const Unsigned32 *values, Unsigned32 count,
                          Unsigned32 version)
{
    Unsigned32 index;

    putUnsigned32(builder, count);
    for (index = 0U; index < count; index++)
    {
        if (version < 3U)
        {
            putUnsigned32(builder, values[index]);
        }
        else
        {
            putUnsigned8(builder, (Unsigned8)(values[index] & 0xFFU));
            putUnsigned8(builder, (Unsigned8)((values[index] >> 8) & 0xFFU));
        }
    }
}

static void putFloatElement(Builder *builder, Unsigned32 identifier, const Real32 *values,
                            Unsigned32 vertexCount, Unsigned32 valuesPerVertex, Unsigned32 version)
{
    Unsigned32 index;

    putUnsigned32(builder, 0U);
    putUnsigned32(builder, identifier);
    putUnsigned32(builder, 0U);
    putUnsigned32(builder, (valuesPerVertex == 2U) ? 1U : 2U);
    putUnsigned32(builder, 0U);
    putUnsigned32(builder, vertexCount * valuesPerVertex * 4U);
    for (index = 0U; index < vertexCount * valuesPerVertex; index++)
    {
        putReal32(builder, values[index]);
    }
    putIndexArray(builder, NULL_POINTER, 0U, version);
}

static void putComponent(Builder *builder, const Unsigned32 *elementIndices, Unsigned32 elementCount,
                         Unsigned32 vertexCount, Unsigned32 version)
{
    putIndexArray(builder, elementIndices, elementCount, version);
    putUnsigned32(builder, vertexCount);
    putUnsigned32(builder, 0U);
    putIndexArray(builder, NULL_POINTER, 0U, version);
    putIndexArray(builder, NULL_POINTER, 0U, version);
    putIndexArray(builder, NULL_POINTER, 0U, version);
}

static void putPrimitive(Builder *builder, Unsigned32 componentIndex, const char *name,
                         const Unsigned32 *faces, Unsigned32 faceCount, Unsigned32 version)
{
    putUnsigned32(builder, 0U);
    putUnsigned32(builder, componentIndex);
    putString(builder, name);
    putIndexArray(builder, faces, faceCount, version);
    putUnsigned32(builder, 0U);
    if (version > 1U)
    {
        putIndexArray(builder, NULL_POINTER, 0U, version);
    }
}

static void buildTwoPartContainer(Builder *builder, Unsigned32 blockVersion)
{
    static const Real32 framePositions[9] = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    static const Real32 frameNormals[9] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f };
    static const Real32 beddingPositions[12] = { 10.0f, 0.0f, 0.0f, 11.0f, 0.0f, 0.0f,
                                                 10.0f, 1.0f, 0.0f, 11.0f, 1.0f, 0.0f };
    static const Real32 beddingNormals[12] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
                                               0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f };
    static const Unsigned32 frameElements[2] = { 0U, 1U };
    static const Unsigned32 beddingElements[2] = { 2U, 3U };
    static const Unsigned32 frameFaces[3] = { 0U, 1U, 2U };
    static const Unsigned32 beddingFaces[6] = { 0U, 1U, 2U, 1U, 3U, 2U };

    builder->length = 0UL;

    putUnsigned32(builder, 0xFFFF0001UL);
    putUnsigned32(builder, 0U);
    putUnsigned32(builder, 1U);
    putUnsigned32(builder, 0xAC4F8687UL);

    putTypeInformation(builder, "cGeometryDataContainer", 0xAC4F8687UL, blockVersion);
    putTypeInformation(builder, "cSGResource", 0xACE46235UL, 2U);
    putString(builder, "bed_tslocator_gmdc");

    putUnsigned32(builder, 4U);
    putFloatElement(builder, 0x5B830781UL, framePositions, 3U, 3U, blockVersion);
    putFloatElement(builder, 0x3B83078BUL, frameNormals, 3U, 3U, blockVersion);
    putFloatElement(builder, 0x5B830781UL, beddingPositions, 4U, 3U, blockVersion);
    putFloatElement(builder, 0x3B83078BUL, beddingNormals, 4U, 3U, blockVersion);

    putUnsigned32(builder, 2U);
    putComponent(builder, frameElements, 2U, 3U, blockVersion);
    putComponent(builder, beddingElements, 2U, 4U, blockVersion);

    putUnsigned32(builder, 3U);
    putPrimitive(builder, 0U, "frame", frameFaces, 3U, blockVersion);
    putPrimitive(builder, 1U, "bedding", beddingFaces, 6U, blockVersion);
    putPrimitive(builder, 9U, "ghost", frameFaces, 3U, blockVersion);
}

static void putIgnoredElement(Builder *builder, Unsigned32 version)
{
    putUnsigned32(builder, 0U);
    putUnsigned32(builder, 0x89D92BA0UL);
    putUnsigned32(builder, 0U);
    putUnsigned32(builder, 2U);
    putUnsigned32(builder, 0U);
    putUnsigned32(builder, 0U);
    putIndexArray(builder, NULL_POINTER, 0U, version);
}

static void buildContainer(Builder *builder, Unsigned32 blockVersion, Unsigned32 padElements)
{
    static const Real32 positions[9] = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 2.0f, 0.0f };
    static const Unsigned32 elementIndices[2] = { 0U, 1U };
    static const Unsigned32 faces[3] = { 0U, 1U, 2U };
    Unsigned32 index;

    builder->length = 0UL;

    putUnsigned32(builder, 0xFFFF0001UL);
    putUnsigned32(builder, 0U);
    putUnsigned32(builder, 1U);
    putUnsigned32(builder, 0xAC4F8687UL);

    putTypeInformation(builder, "cGeometryDataContainer", 0xAC4F8687UL, blockVersion);
    putTypeInformation(builder, "cSGResource", 0xACE46235UL, 2U);
    putString(builder, "body_tslocator_gmdc");

    putUnsigned32(builder, 2U + padElements);

    putUnsigned32(builder, 0U);
    putUnsigned32(builder, 0x5B830781UL);
    putUnsigned32(builder, 0U);
    putUnsigned32(builder, 2U);
    putUnsigned32(builder, 0U);
    putUnsigned32(builder, 36U);
    for (index = 0U; index < 9U; index++)
    {
        putReal32(builder, positions[index]);
    }
    putIndexArray(builder, NULL_POINTER, 0U, blockVersion);

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
    putIndexArray(builder, NULL_POINTER, 0U, blockVersion);

    for (index = 0U; index < padElements; index++)
    {
        putIgnoredElement(builder, blockVersion);
    }

    putUnsigned32(builder, 1U);
    putIndexArray(builder, elementIndices, 2U, blockVersion);
    putUnsigned32(builder, 3U);
    putUnsigned32(builder, 0U);
    putIndexArray(builder, NULL_POINTER, 0U, blockVersion);
    putIndexArray(builder, NULL_POINTER, 0U, blockVersion);
    putIndexArray(builder, NULL_POINTER, 0U, blockVersion);

    putUnsigned32(builder, 1U);
    putUnsigned32(builder, 0U);
    putUnsigned32(builder, 0U);
    putString(builder, "body");
    putIndexArray(builder, faces, 3U, blockVersion);
    putUnsigned32(builder, 0U);
    if (blockVersion > 1U)
    {
        putIndexArray(builder, NULL_POINTER, 0U, blockVersion);
    }
}

static void buildSkinnedContainer(Builder *builder, Unsigned32 blockVersion,
                                  Boolean withWeights, Boolean emptyMorphMap)
{
    static const Real32 positions[9] = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    static const Unsigned32 assignments[3] = { 0xFF020100UL, 0xFFFFFF00UL, 0xFF010002UL };
    static const Real32 weights[6] = { 0.25f, 0.5f, 1.0f, 0.0f, 0.5f, 0.25f };
    static const Unsigned32 elementIndices[6] = { 0U, 1U, 2U, 3U, 4U, 5U };
    static const Unsigned32 faces[3] = { 0U, 1U, 2U };
    Unsigned32 index;

    builder->length = 0UL;

    putUnsigned32(builder, 0xFFFF0001UL);
    putUnsigned32(builder, 0U);
    putUnsigned32(builder, 1U);
    putUnsigned32(builder, 0xAC4F8687UL);

    putTypeInformation(builder, "cGeometryDataContainer", 0xAC4F8687UL, blockVersion);
    putTypeInformation(builder, "cSGResource", 0xACE46235UL, 2U);
    putString(builder, "abodynude_gmdc");

    putUnsigned32(builder, withWeights ? 6U : 2U);
    putFloatElement(builder, 0x5B830781UL, positions, 3U, 3U, blockVersion);

    putUnsigned32(builder, 0U);
    putUnsigned32(builder, 0xFBD70111UL);
    putUnsigned32(builder, 0U);
    putUnsigned32(builder, 4U);
    putUnsigned32(builder, 0U);
    putUnsigned32(builder, 12U);
    for (index = 0U; index < 3U; index++)
    {
        putUnsigned32(builder, assignments[index]);
    }
    putIndexArray(builder, NULL_POINTER, 0U, blockVersion);

    if (withWeights)
    {
        putUnsigned32(builder, 0U);
        putUnsigned32(builder, 0x3BD70105UL);
        putUnsigned32(builder, 0U);
        putUnsigned32(builder, 1U);
        putUnsigned32(builder, 0U);
        putUnsigned32(builder, 24U);
        for (index = 0U; index < 6U; index++)
        {
            putReal32(builder, weights[index]);
        }
        putIndexArray(builder, NULL_POINTER, 0U, blockVersion);
    }

    if (withWeights)
    {
        static const Unsigned32 assignedMap[3] = { 0x01000000UL, 0x00020000UL, 0x00000000UL };
        static const Unsigned32 blankMap[3] = { 0x00000000UL, 0x00000000UL, 0x00000000UL };
        const Unsigned32 *morphMap = (emptyMorphMap == BOOLEAN_TRUE) ? blankMap : assignedMap;
        static const Real32 firstDeltas[9] = { 0.5f, 0.0f, 0.0f, 9.0f, 9.0f, 9.0f,
                                               9.0f, 9.0f, 9.0f };
        static const Real32 secondDeltas[9] = { 9.0f, 9.0f, 9.0f, 0.0f, 0.25f, 0.0f,
                                                9.0f, 9.0f, 9.0f };

        putUnsigned32(builder, 0U);
        putUnsigned32(builder, 0xDCF2CFDCUL);
        putUnsigned32(builder, 0U);
        putUnsigned32(builder, 4U);
        putUnsigned32(builder, 0U);
        putUnsigned32(builder, 12U);
        for (index = 0U; index < 3U; index++)
        {
            putUnsigned32(builder, morphMap[index]);
        }
        putIndexArray(builder, NULL_POINTER, 0U, blockVersion);

        putFloatElement(builder, 0x5CF2CFE1UL, firstDeltas, 3U, 3U, blockVersion);
        putFloatElement(builder, 0x5CF2CFE1UL, secondDeltas, 3U, 3U, blockVersion);
    }

    putUnsigned32(builder, 1U);
    putIndexArray(builder, elementIndices, withWeights ? 6U : 2U, blockVersion);
    putUnsigned32(builder, 3U);
    putUnsigned32(builder, 0U);
    putIndexArray(builder, NULL_POINTER, 0U, blockVersion);
    putIndexArray(builder, NULL_POINTER, 0U, blockVersion);
    putIndexArray(builder, NULL_POINTER, 0U, blockVersion);

    putUnsigned32(builder, 1U);
    putUnsigned32(builder, 0U);
    putUnsigned32(builder, 0U);
    putString(builder, "body");
    putIndexArray(builder, faces, 3U, blockVersion);
    putUnsigned32(builder, 0U);
    if (blockVersion > 1U)
    {
        static const Unsigned32 bones[3] = { 4U, 9U, 2U };

        putIndexArray(builder, bones, 3U, blockVersion);
    }

    putUnsigned32(builder, 10U);
    for (index = 0U; index < 10U; index++)
    {
        putReal32(builder, 0.0f);
        putReal32(builder, 0.0f);
        putReal32(builder, 0.0f);
        putReal32(builder, 1.0f);
        putReal32(builder, (Real32)index);
        putReal32(builder, 0.0f);
        putReal32(builder, 0.0f);
    }

    putUnsigned32(builder, 3U);
    putString(builder, "");
    putString(builder, "");
    putString(builder, "botmorphs");
    putString(builder, "fatbot");
    putString(builder, "botmorphs");
    putString(builder, "pregbot");
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
    checkThat(&failureCount, "says it is a 0xFFFF0001 collection", mesh.versionMark == 0xFFFF0001UL);
    checkThat(&failureCount, "at container version 4", mesh.containerVersion == 4U);

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

        for (version = 1U; version <= 2U; version++)
        {
            buildContainer(&builder, version, 0U);
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
            checkThat(&failureCount, "reports the version it read",
                      older.containerVersion == version);
            checkThat(&failureCount, "finds its three vertices", older.vertexCount == 3U);
            checkThat(&failureCount, "and its one triangle", older.indexCount == 3U);
            checkThat(&failureCount, "with the faces in order",
                      older.indices[0] == 0U && older.indices[1] == 1U && older.indices[2] == 2U);
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

    printf("\n-- a model in two parts --\n");
    {
        static Builder builder;
        GeometryMesh model;

        buildTwoPartContainer(&builder, 4U);
        result = geometryReaderOpen(&model, builder.bytes, builder.length, &arena);
        checkThat(&failureCount, "reads a container in two parts", result == GEOMETRY_READ_OK);
        if (result != GEOMETRY_READ_OK)
        {
            printf("  result: %s\n", geometryReadResultGetName(result));
            return checkSummarize(failureCount, "geometry reader");
        }

        checkThat(&failureCount, "reports two components", model.componentCount == 2U);
        checkThat(&failureCount, "and the three primitives the file held",
                  model.primitiveCount == 3U);
        checkThat(&failureCount, "keeping the two that name a real component",
                  model.storedPrimitiveCount == 2U);

        checkThat(&failureCount, "merging both vertex arrays", model.vertexCount == 7U);
        checkThat(&failureCount, "and both face arrays", model.indexCount == 9U);
        checkThat(&failureCount, "naming the first part frame",
                  stringEquals(model.primitives[0].name, "frame"));
        checkThat(&failureCount, "and the second bedding",
                  stringEquals(model.primitives[1].name, "bedding"));

        printf("\n-- do the parts tile the index array --\n");
        checkThat(&failureCount, "the first part starts at the beginning",
                  model.primitives[0].firstIndex == 0U && model.primitives[0].indexCount == 3U);
        checkThat(&failureCount, "the second picks up where it left off",
                  model.primitives[1].firstIndex == 3U && model.primitives[1].indexCount == 6U);

        printf("\n-- are the second part's indices moved to match --\n");
        {
            Unsigned32 inner;
            Boolean firstInRange = BOOLEAN_TRUE;
            Boolean secondMoved = BOOLEAN_TRUE;

            for (inner = 0U; inner < 3U; inner++)
            {
                if (model.indices[inner] >= 3U)
                {
                    firstInRange = BOOLEAN_FALSE;
                }
            }
            for (inner = 3U; inner < 9U; inner++)
            {
                if (model.indices[inner] < 3U || model.indices[inner] >= 7U)
                {
                    secondMoved = BOOLEAN_FALSE;
                }
            }
            checkThat(&failureCount, "the first part's indices address its own vertices",
                      firstInRange);
            checkThat(&failureCount, "and the second part's are shifted past them", secondMoved);
        }

        printf("\n-- did the second part's vertices actually arrive --\n");
        {
            Real32 minimum[3];
            Real32 maximum[3];

            geometryMeshGetBounds(&model, minimum, maximum);
            checkThat(&failureCount, "the box spans both parts",
                      nearly(minimum[0], 0.0f) && nearly(maximum[0], 11.0f));
        }
    }

    printf("\n-- a skinned model --\n");
    {
        static Builder builder;
        GeometryMesh skinned;

        buildSkinnedContainer(&builder, 4U, BOOLEAN_TRUE, BOOLEAN_FALSE);
        result = geometryReaderOpen(&skinned, builder.bytes, builder.length, &arena);
        checkThat(&failureCount, "reads a container carrying bone data", result == GEOMETRY_READ_OK);
        if (result != GEOMETRY_READ_OK)
        {
            printf("  result: %s\n", geometryReadResultGetName(result));
            return checkSummarize(failureCount, "geometry reader");
        }

        checkThat(&failureCount, "keeps the bone assignments",
                  skinned.boneAssignments != NULL_POINTER);
        checkThat(&failureCount, "and the weights", skinned.boneWeights != NULL_POINTER);
        checkThat(&failureCount, "reporting how many were stored per vertex",
                  skinned.weightsStoredPerVertex == 2U);
        checkThat(&failureCount, "and that every vertex is weighted",
                  skinned.skinnedVertexCount == 3U);

        printf("\n-- is the assignment word unpacked low byte first --\n");
        checkThat(&failureCount, "the first slot is the low byte",
                  skinned.boneAssignments[0] == 0U);
        checkThat(&failureCount, "the second is the one above it",
                  skinned.boneAssignments[1] == 1U);
        checkThat(&failureCount, "and the third above that",
                  skinned.boneAssignments[2] == 2U);
        checkThat(&failureCount, "with the unused slot saying so",
                  skinned.boneAssignments[3] == 255U);
        checkThat(&failureCount, "a vertex on one bone leaves the rest unassigned",
                  skinned.boneAssignments[4] == 0U && skinned.boneAssignments[5] == 255U);

        printf("\n-- is the weight the file left out worked back out --\n");
        checkThat(&failureCount, "the stored weights arrive",
                  nearly(skinned.boneWeights[0], 0.25f) && nearly(skinned.boneWeights[1], 0.5f));
        checkThat(&failureCount, "the implied one is the remainder",
                  nearly(skinned.boneWeights[2], 0.25f));
        checkThat(&failureCount, "and the fourth slot is empty",
                  nearly(skinned.boneWeights[3], 0.0f));
        checkThat(&failureCount, "a vertex whose stored weights already sum to one implies nothing",
                  nearly(skinned.boneWeights[4], 1.0f) && nearly(skinned.boneWeights[6], 0.0f));

        printf("\n-- half of a skeleton is not a skeleton --\n");
        {
            GeometryMesh lopsided;

            buildSkinnedContainer(&builder, 4U, BOOLEAN_FALSE, BOOLEAN_FALSE);
            result = geometryReaderOpen(&lopsided, builder.bytes, builder.length, &arena);
            checkThat(&failureCount, "still reads the mesh", result == GEOMETRY_READ_OK);
            checkThat(&failureCount, "but keeps no bone data from it",
                      lopsided.boneAssignments == NULL_POINTER &&
                          lopsided.boneWeights == NULL_POINTER);
            checkThat(&failureCount, "and reports the assignments it passed over",
                      lopsided.unusedElementCount == 1U &&
                          lopsided.unusedElements[0] == 0xFBD70111UL);
        }

        printf("\n-- does the primitive keep the bones it names --\n");
        checkThat(&failureCount, "keeps the bone list", skinned.primitives[0].boneRemap != NULL_POINTER);
        checkThat(&failureCount, "with the count the file gave",
                  skinned.primitives[0].boneRemapCount == 3U);
        checkThat(&failureCount, "and the bones in it",
                  skinned.primitives[0].boneRemap[0] == 4U &&
                      skinned.primitives[0].boneRemap[1] == 9U &&
                      skinned.primitives[0].boneRemap[2] == 2U);

        printf("\n-- does the container's own bind pose arrive --\n");
        checkThat(&failureCount, "the bind pose is read", skinned.bindPoses != NULL_POINTER);
        checkThat(&failureCount, "with one entry per bone the file described",
                  skinned.bindPoseCount == 10U);
        checkThat(&failureCount, "indexed by the bone number and not by the slot that named it",
                  nearly(skinned.bindPoses[9].translation[0], 9.0f) &&
                      nearly(skinned.bindPoses[4].translation[0], 4.0f));
        checkThat(&failureCount, "reading the quaternion in the order the file writes it",
                  nearly(skinned.bindPoses[0].rotation[3], 1.0f) &&
                      nearly(skinned.bindPoses[0].rotation[0], 0.0f));

        printf("\n-- and the deformation channels straight after it --\n");
        checkThat(&failureCount, "the morph targets are read",
                  skinned.morphTargets != NULL_POINTER && skinned.morphTargetCount == 3U);
        if (skinned.morphTargets != NULL_POINTER && skinned.morphTargetCount == 3U)
        {
            checkThat(&failureCount, "the blank first channel is kept, not compacted away",
                      skinned.morphTargets[0].groupName[0] == '\0' &&
                          skinned.morphTargets[0].channelName[0] == '\0');
            checkThat(&failureCount, "so the channels keep the numbers the vertex map uses",
                      stringEqualsIgnoringCase(skinned.morphTargets[1].channelName, "fatbot") &&
                          stringEqualsIgnoringCase(skinned.morphTargets[2].channelName, "pregbot"));
            checkThat(&failureCount, "and both of a channel's two names are kept",
                      stringEqualsIgnoringCase(skinned.morphTargets[1].groupName, "botmorphs") &&
                          stringEqualsIgnoringCase(skinned.morphTargets[2].groupName, "botmorphs"));
        }

        printf("\n-- and what the channels actually move --\n");
        checkThat(&failureCount, "the two delta sets are found",
                  skinned.morphSlotCount == 2U && skinned.morphSlotDeltas != NULL_POINTER);
        checkThat(&failureCount, "and the map was read for every vertex, not merely sized for",
                  skinned.morphMappedVertexCount == 3U);
        if (skinned.morphSlotCount == 2U && skinned.morphSlotChannels != NULL_POINTER)
        {
            checkThat(&failureCount, "slot nought comes from the word's top byte",
                      skinned.morphSlotChannels[0] == 1U && skinned.morphSlotChannels[1] == 0U);
            checkThat(&failureCount, "and slot one from the byte below it",
                      skinned.morphSlotChannels[2] == 0U && skinned.morphSlotChannels[3] == 2U);
            checkThat(&failureCount, "a vertex in no channel names none",
                      skinned.morphSlotChannels[4] == 0U && skinned.morphSlotChannels[5] == 0U);
            checkThat(&failureCount, "each slot's deltas come from its own element",
                      nearly(skinned.morphSlotDeltas[0], 0.5f) &&
                          nearly(skinned.morphSlotDeltas[10], 0.25f));
        }
        {
            static Builder morphBuilder;
            static GeometryMesh deforming;
            static const Real32 weights[3] = { 0.0f, 1.0f, 2.0f };
            Unsigned32 deformed;

            buildSkinnedContainer(&morphBuilder, 4U, BOOLEAN_TRUE, BOOLEAN_FALSE);
            (void)geometryReaderOpen(&deforming, morphBuilder.bytes, morphBuilder.length, &arena);
            deformed = geometryMeshApplyMorph(&deforming, weights, 3U);

            checkThat(&failureCount, "applying them moves the two vertices in a channel",
                      deformed == 2U);
            checkThat(&failureCount, "the first by its channel's delta times its weight",
                      nearly(deforming.positions[0], 0.5f) && nearly(deforming.positions[1], 0.0f));
            checkThat(&failureCount, "the second by the other channel's, weighted twice",
                      nearly(deforming.positions[3], 1.0f) && nearly(deforming.positions[4], 0.5f));
            checkThat(&failureCount, "and the vertex in no channel does not move",
                      nearly(deforming.positions[6], 0.0f) && nearly(deforming.positions[7], 1.0f));

            checkThat(&failureCount, "a channel at rest moves nothing",
                      geometryMeshApplyMorph(&deforming, weights, 1U) == 0U);
        }

        printf("\n-- a body's map, which is empty over deltas that are not --\n");
        {
            static Builder blankBuilder;
            static GeometryMesh blankMapped;
            static const Real32 weights[3] = { 0.0f, 1.0f, 2.0f };

            buildSkinnedContainer(&blankBuilder, 4U, BOOLEAN_TRUE, BOOLEAN_TRUE);
            (void)geometryReaderOpen(&blankMapped, blankBuilder.bytes, blankBuilder.length, &arena);

            checkThat(&failureCount, "an empty map over deltas is filled in",
                      blankMapped.morphChannelsInferred == BOOLEAN_TRUE);
            checkThat(&failureCount, "slot nought driving channel one",
                      blankMapped.morphSlotCount == 2U &&
                          blankMapped.morphSlotChannels[0] == 1U);
            checkThat(&failureCount, "and slot one driving channel two",
                      blankMapped.morphSlotChannels[1] == 2U);
            checkThat(&failureCount, "so every vertex deforms rather than none",
                      geometryMeshApplyMorph(&blankMapped, weights, 3U) == 3U);

            checkThat(&failureCount, "a map that says something is left alone",
                      skinned.morphChannelsInferred == BOOLEAN_FALSE);
            checkThat(&failureCount, "keeping the slots it actually named",
                      skinned.morphSlotChannels[0] == 1U && skinned.morphSlotChannels[1] == 0U);
        }

        printf("\n-- joining several containers into one model --\n");
        {
            static GeometryMesh whole;
            const GeometryMesh *parts[2];
            GeometryReadResult joined;
            Unsigned32 before = skinned.vertexCount;

            parts[0] = &skinned;
            parts[1] = &mesh;
            joined = geometryMeshMerge(&whole, parts, 2U, &arena);

            checkThat(&failureCount, "the join succeeds", joined == GEOMETRY_READ_OK);
            if (joined == GEOMETRY_READ_OK)
            {
                checkThat(&failureCount, "every vertex of both arrives",
                          whole.vertexCount == before + mesh.vertexCount);
                checkThat(&failureCount, "and every index",
                          whole.indexCount == skinned.indexCount + mesh.indexCount);
                checkThat(&failureCount, "and every part stays a part",
                          whole.storedPrimitiveCount ==
                              skinned.storedPrimitiveCount + mesh.storedPrimitiveCount);

                {
                    Unsigned32 index;
                    Boolean allInRange = BOOLEAN_TRUE;
                    Unsigned32 lowestOfSecond = 0xFFFFU;

                    for (index = 0U; index < whole.indexCount; index++)
                    {
                        if ((Unsigned32)whole.indices[index] >= whole.vertexCount)
                        {
                            allInRange = BOOLEAN_FALSE;
                        }
                    }
                    for (index = skinned.indexCount; index < whole.indexCount; index++)
                    {
                        if ((Unsigned32)whole.indices[index] < lowestOfSecond)
                        {
                            lowestOfSecond = (Unsigned32)whole.indices[index];
                        }
                    }
                    checkThat(&failureCount, "every index addresses a vertex of the joined model",
                              allInRange);
                    checkThat(&failureCount, "and the second model's reach past the first's",
                              lowestOfSecond >= before);
                }

                checkThat(&failureCount, "the first part still starts at the beginning",
                          whole.primitives[0].firstIndex == 0U);
                checkThat(&failureCount, "and the part after it starts where that one ended",
                          whole.primitives[skinned.storedPrimitiveCount].firstIndex ==
                              skinned.indexCount);

                checkThat(&failureCount, "a part with no bones is left unassigned, not on bone 0",
                          whole.boneAssignments != NULL_POINTER &&
                              whole.boneAssignments[before * 4U] == 255U);
                checkThat(&failureCount, "while the skinned part keeps its own bones",
                          whole.boneAssignments[0] == 0U && whole.boneAssignments[1] == 1U);
                checkThat(&failureCount, "and the bind pose comes from whichever part had one",
                          whole.bindPoseCount == skinned.bindPoseCount);

                checkThat(&failureCount, "both parts' channels arrive in one list",
                          whole.morphTargetCount ==
                              skinned.morphTargetCount + mesh.morphTargetCount);
                if (whole.morphSlotChannels != NULL_POINTER && whole.morphSlotCount > 0U)
                {
                    Unsigned32 highest = 0U;
                    Unsigned32 slot;

                    for (slot = 0U; slot < whole.vertexCount * whole.morphSlotCount; slot++)
                    {
                        if ((Unsigned32)whole.morphSlotChannels[slot] > highest)
                        {
                            highest = (Unsigned32)whole.morphSlotChannels[slot];
                        }
                    }
                    checkThat(&failureCount, "the first part's channels keep their numbers",
                              whole.morphSlotChannels[0] == 1U);
                    checkThat(&failureCount, "and no channel is numbered past the joined list",
                              highest < whole.morphTargetCount);
                }

                {
                    static Builder firstBuilder;
                    static Builder secondBuilder;
                    static GeometryMesh firstPart;
                    static GeometryMesh secondPart;
                    static GeometryMesh bothParts;
                    const GeometryMesh *bothOfThem[2];

                    buildSkinnedContainer(&firstBuilder, 4U, BOOLEAN_TRUE, BOOLEAN_FALSE);
                    buildSkinnedContainer(&secondBuilder, 4U, BOOLEAN_TRUE, BOOLEAN_FALSE);
                    (void)geometryReaderOpen(&firstPart, firstBuilder.bytes, firstBuilder.length,
                                             &arena);
                    (void)geometryReaderOpen(&secondPart, secondBuilder.bytes, secondBuilder.length,
                                             &arena);
                    bothOfThem[0] = &firstPart;
                    bothOfThem[1] = &secondPart;

                    if (geometryMeshMerge(&bothParts, bothOfThem, 2U, &arena) == GEOMETRY_READ_OK &&
                        bothParts.morphSlotChannels != NULL_POINTER)
                    {
                        Unsigned32 stride = bothParts.morphSlotCount;
                        Unsigned32 secondBase = firstPart.vertexCount;

                        checkThat(&failureCount, "two deforming parts join their channel lists",
                                  bothParts.morphTargetCount == 6U);
                        checkThat(&failureCount, "the first part's stay where they were",
                                  bothParts.morphSlotChannels[0] == 1U &&
                                      bothParts.morphSlotChannels[stride + 1U] == 2U);
                        checkThat(&failureCount, "and the second part's move up by the first's count",
                                  bothParts.morphSlotChannels[secondBase * stride] == 4U &&
                                      bothParts.morphSlotChannels[(secondBase + 1U) * stride + 1U] ==
                                          5U);
                        checkThat(&failureCount, "while an unused slot stays unused rather than "
                                                 "shifting onto a real channel",
                                  bothParts.morphSlotChannels[secondBase * stride + 1U] == 0U);
                        checkThat(&failureCount, "the joined list names channel 4 as the second "
                                                 "part's first",
                                  bothParts.morphTargetCount == 6U &&
                                      stringEqualsIgnoringCase(bothParts.morphTargets[4].channelName,
                                                               "fatbot"));
                    }
                }

                checkThat(&failureCount, "no two joined parts claim the same component",
                          whole.primitives[0].componentIndex !=
                              whole.primitives[skinned.storedPrimitiveCount].componentIndex);
            }
        }

        printf("\n-- posing by the skeleton --\n");
        {
            static Real32 palette[10 * 16];
            Unsigned32 bone;
            Unsigned32 cell;

            for (bone = 0U; bone < 10U; bone++)
            {
                for (cell = 0U; cell < 16U; cell++)
                {
                    palette[bone * 16U + cell] = (cell % 5U == 0U) ? 1.0f : 0.0f;
                }
            }
            palette[4U * 16U + 12U] = 10.0f;
            palette[9U * 16U + 13U] = 100.0f;

            checkThat(&failureCount, "moves every weighted vertex",
                      geometryMeshApplySkin(&skinned, palette, 10U) == 3U);
            checkThat(&failureCount, "blending the translations by their weights",
                      nearly(skinned.positions[0], 2.5f) && nearly(skinned.positions[1], 50.0f));

            printf("\n-- a bone that undoes itself moves nothing --\n");
            {
                GeometryMesh resting;
                static Real32 bindPalette[10 * 16];

                buildSkinnedContainer(&builder, 4U, BOOLEAN_TRUE, BOOLEAN_FALSE);
                (void)geometryReaderOpen(&resting, builder.bytes, builder.length, &arena);
                for (bone = 0U; bone < 10U; bone++)
                {
                    for (cell = 0U; cell < 16U; cell++)
                    {
                        bindPalette[bone * 16U + cell] = (cell % 5U == 0U) ? 1.0f : 0.0f;
                    }
                }
                checkThat(&failureCount, "every vertex is still moved",
                          geometryMeshApplySkin(&resting, bindPalette, 10U) == 3U);
                checkThat(&failureCount, "and every one of them lands where it started",
                          nearly(resting.positions[0], 0.0f) && nearly(resting.positions[3], 1.0f) &&
                              nearly(resting.positions[7], 1.0f));
            }

            printf("\n-- refusing to pose what it cannot --\n");
            {
                GeometryMesh again;

                buildSkinnedContainer(&builder, 4U, BOOLEAN_TRUE, BOOLEAN_FALSE);
                (void)geometryReaderOpen(&again, builder.bytes, builder.length, &arena);
                checkThat(&failureCount, "moves nothing when the bones are out of range",
                          geometryMeshApplySkin(&again, palette, 2U) == 0U);
                checkThat(&failureCount, "leaving the vertices where they were",
                          nearly(again.positions[3], 1.0f) && nearly(again.positions[4], 0.0f));
                checkThat(&failureCount, "and nothing at all without a palette",
                          geometryMeshApplySkin(&again, NULL_POINTER, 10U) == 0U);
            }
        }

        printf("\n-- are the element names the ones the format uses --\n");
        checkThat(&failureCount, "names the tangent element",
                  stringEquals(geometryElementGetName(0x89D92BA0UL), "tangents"));
        checkThat(&failureCount, "names bone assignments",
                  stringEquals(geometryElementGetName(0xFBD70111UL), "bone assignments"));
        checkThat(&failureCount, "and admits when it has no name for one",
                  geometryElementGetName(0x00000001UL) == NULL_POINTER);
    }

    printf("\n-- a container with more elements than a fixed array would hold --\n");
    {
        static Builder builder;
        GeometryMesh crowded;

        buildContainer(&builder, 4U, 58U);
        result = geometryReaderOpen(&crowded, builder.bytes, builder.length, &arena);
        checkThat(&failureCount, "reads a container carrying sixty elements",
                  result == GEOMETRY_READ_OK);
        if (result != GEOMETRY_READ_OK)
        {
            printf("  result: %s\n", geometryReadResultGetName(result));
        }
        checkThat(&failureCount, "and reports how many it found", crowded.elementCount == 60U);
        checkThat(&failureCount, "still finding the positions past the padding",
                  crowded.vertexCount == 3U);
        checkThat(&failureCount, "and the normals", crowded.normals != NULL_POINTER);

        {
            static const Unsigned8 wildCount[] = {
                0x01U, 0x00U, 0xFFU, 0xFFU,
                0x00U, 0x00U, 0x00U, 0x00U,
                0x01U, 0x00U, 0x00U, 0x00U,
                0x87U, 0x86U, 0x4FU, 0xACU,
                0x01U, 'x',  0x87U, 0x86U, 0x4FU, 0xACU, 0x04U, 0x00U, 0x00U, 0x00U,
                0x01U, 'y',  0x00U, 0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U,
                0x01U, 'z',
                0xFFU, 0xFFU, 0xFFU, 0x0FU
            };
            GeometryMesh other;

            checkThat(&failureCount, "refuses a count the resource has no room for",
                      geometryReaderOpen(&other, wildCount, sizeof(wildCount), &arena) ==
                          GEOMETRY_READ_TOO_MANY_ELEMENTS);
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
            static const Unsigned8 olderMark[16] = { 0x01U, 0x00U, 0xFEU, 0xFFU, 0U };

            checkThat(&failureCount, "calls an older collection mark a version, not rubbish",
                      geometryReaderOpen(&other, olderMark, sizeof(olderMark), &arena) ==
                          GEOMETRY_READ_OLDER_COLLECTION);
            checkThat(&failureCount, "and reports which mark it was",
                      other.versionMark == 0xFFFE0001UL);
        }

        {
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

    printf("\n-- moving a mesh by a transform --\n");
    {
        static const Real32 quarterTurnThenShift[16] = {
            0.0F, 1.0F, 0.0F, 0.0F,
            -1.0F, 0.0F, 0.0F, 0.0F,
            0.0F, 0.0F, 1.0F, 0.0F,
            2.0F, 0.0F, 0.0F, 1.0F
        };
        static Real32 positions[6] = { 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F };
        static Real32 normals[6] = { 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F };
        GeometryMesh moved;

        moved.positions = positions;
        moved.normals = normals;
        moved.textureCoordinates = NULL_POINTER;
        moved.vertexCount = 2U;

        geometryMeshApplyTransform(&moved, quarterTurnThenShift);

        checkThat(&failureCount, "a point on x turns onto y and shifts",
                  nearly(positions[0], 2.0F) && nearly(positions[1], 1.0F) &&
                      nearly(positions[2], 0.0F));
        checkThat(&failureCount, "and a point on z only shifts",
                  nearly(positions[3], 2.0F) && nearly(positions[4], 0.0F) &&
                      nearly(positions[5], 1.0F));
        checkThat(&failureCount, "a normal turns without moving",
                  nearly(normals[0], 0.0F) && nearly(normals[1], 1.0F) &&
                      nearly(normals[2], 0.0F));
        checkThat(&failureCount, "and one along the axis of the turn is unchanged",
                  nearly(normals[3], 0.0F) && nearly(normals[4], 0.0F) &&
                      nearly(normals[5], 1.0F));
    }

    return checkSummarize(failureCount, "geometry reader");
}
