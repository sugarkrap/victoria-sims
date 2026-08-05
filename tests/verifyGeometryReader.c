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

   The teapot is one container: block version 4, two elements, half word
   indices. Everything either side of that — the word wide indices of versions 1
   and 2, and the crowded element lists a retail body mesh carries — has no
   fixture on hand, so one is written here byte by byte from the layout rather
   than from anything this reader produced.

   The index width has to be right in both directions or the fixture tests only
   itself: word arrays labelled version 4 read back as an empty container, which
   is indistinguishable from a reader bug until you look. */

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

/* Indices are a word wide below block version 3 and a half word from 3 up. The
   builder has to agree with the reader about that or the fixture tests nothing
   but the builder — writing word arrays and labelling them version 4 produced a
   container that read as empty, which is what a mismatch looks like. */
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
    putUnsigned32(builder, 0U); /* draw order */
    if (version > 1U)
    {
        putIndexArray(builder, NULL_POINTER, 0U, version);
    }
}

/* Two components with vertices of their own and three primitives, one of which
   names a component that is not there. What an assembled model looks like. */
static void buildTwoPartContainer(Builder *builder, Unsigned32 blockVersion)
{
    static const Real32 framePositions[9] = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    static const Real32 frameNormals[9] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f };
    /* Ten units along x, so the bounding box says plainly whether these arrived. */
    static const Real32 beddingPositions[12] = { 10.0f, 0.0f, 0.0f, 11.0f, 0.0f, 0.0f,
                                                 10.0f, 1.0f, 0.0f, 11.0f, 1.0f, 0.0f };
    static const Real32 beddingNormals[12] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
                                               0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f };
    static const Unsigned32 frameElements[2] = { 0U, 1U };
    static const Unsigned32 beddingElements[2] = { 2U, 3U };
    static const Unsigned32 frameFaces[3] = { 0U, 1U, 2U };
    /* Numbered from zero within their own component, as the file does it. */
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
    /* Names a component that does not exist. */
    putPrimitive(builder, 9U, "ghost", frameFaces, 3U, blockVersion);
}

/* Elements the reader does not use, to push the count past any ceiling. Tangents
   are real and ignored, which makes them the honest thing to pad with. */
static void putIgnoredElement(Builder *builder, Unsigned32 version)
{
    putUnsigned32(builder, 0U);
    putUnsigned32(builder, 0x89D92BA0UL);
    putUnsigned32(builder, 0U);
    putUnsigned32(builder, 2U);
    putUnsigned32(builder, 0U);
    putUnsigned32(builder, 0U); /* no payload */
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
    putUnsigned32(builder, 0U); /* no file links */
    putUnsigned32(builder, 1U); /* one block */
    putUnsigned32(builder, 0xAC4F8687UL);

    putTypeInformation(builder, "cGeometryDataContainer", 0xAC4F8687UL, blockVersion);
    putTypeInformation(builder, "cSGResource", 0xACE46235UL, 2U);
    putString(builder, "body_tslocator_gmdc");

    putUnsigned32(builder, 2U + padElements);

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
    putIndexArray(builder, NULL_POINTER, 0U, blockVersion);

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
    putIndexArray(builder, NULL_POINTER, 0U, blockVersion);

    for (index = 0U; index < padElements; index++)
    {
        putIgnoredElement(builder, blockVersion);
    }

    putUnsigned32(builder, 1U); /* one component */
    putIndexArray(builder, elementIndices, 2U, blockVersion);
    putUnsigned32(builder, 3U); /* three vertices */
    putUnsigned32(builder, 0U);
    putIndexArray(builder, NULL_POINTER, 0U, blockVersion);
    putIndexArray(builder, NULL_POINTER, 0U, blockVersion);
    putIndexArray(builder, NULL_POINTER, 0U, blockVersion);

    /* A primitive is six fields, not four: a type, the component it draws from,
       a name, its faces, the order it is drawn in among transparent parts, and
       the bone indices it remaps. This fixture used to write the first four,
       which matched a reader that only ever looked at primitive zero and so
       never had to find primitive one. Both were wrong in the same way, which
       is the failure mode of writing a fixture from the same understanding as
       the code it checks. */
    putUnsigned32(builder, 1U); /* one primitive */
    putUnsigned32(builder, 0U); /* type */
    putUnsigned32(builder, 0U); /* the component it draws from */
    putString(builder, "body");
    putIndexArray(builder, faces, 3U, blockVersion);
    putUnsigned32(builder, 0U); /* draw order */
    if (blockVersion > 1U)
    {
        putIndexArray(builder, NULL_POINTER, 0U, blockVersion);
    }
}

/* A skinned container: positions, bone assignments and bone weights.
 *
 * Written from the format's own table rather than from the reader, because a
 * fixture that encodes the same understanding as the code it checks agrees with
 * it whether or not either is right — which is exactly how the section offsets
 * in the program reader stayed wrong through a passing test.
 *
 * The assignment word packs four byte indices, low byte first, and spells an
 * unused slot 255. Two weights are stored and the third implied, so the reader
 * has to work the missing one back out rather than leave the vertex weighing
 * three quarters of itself. */
static void buildSkinnedContainer(Builder *builder, Unsigned32 blockVersion, Boolean withWeights)
{
    static const Real32 positions[9] = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    /* Slots into the primitive's bone list below, not bones: the first three
       slots then the first alone then all three in another order. */
    static const Unsigned32 assignments[3] = { 0xFF020100UL, 0xFFFFFF00UL, 0xFF010002UL };
    static const Real32 weights[6] = { 0.25f, 0.5f, 1.0f, 0.0f, 0.5f, 0.25f };
    static const Unsigned32 elementIndices[3] = { 0U, 1U, 2U };
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

    putUnsigned32(builder, withWeights ? 3U : 2U);
    putFloatElement(builder, 0x5B830781UL, positions, 3U, 3U, blockVersion);

    /* Bone assignments: one word per vertex. The format code is deliberately
       not two — the reader must take this element on its identifier and the
       length of its payload, not on a layout number it was told to expect. */
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
        /* Format one: two floats per vertex, the third worked back out. */
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

    putUnsigned32(builder, 1U);
    putIndexArray(builder, elementIndices, withWeights ? 3U : 2U, blockVersion);
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
        /* The bones this primitive's slots stand for. Deliberately not 0, 1, 2:
           a reader that ignored this list and used the slot directly would pass
           every check against an identity mapping and fail on every real
           mesh. */
        static const Unsigned32 bones[3] = { 4U, 9U, 2U };

        putIndexArray(builder, bones, 3U, blockVersion);
    }

    /* The bind pose, which the container carries itself in a section straight
     * after the primitives.
     *
     * Ten entries because the bone list above names bone 9, and these are
     * indexed by the bone number rather than by the slot that named it — a
     * reader that indexed by the slot would read entry 1 for bone 9 and find a
     * plausible transform there, which is why each entry is given a translation
     * equal to its own index and nothing else. */
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
    /* Reported so a disc can be described by what it holds rather than by what
       was made of it. The teapot is the newest layout of the four. */
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

    printf("\n-- a model in two parts --\n");
    {
        static Builder builder;
        GeometryMesh model;

        /* Two components with separate vertex arrays, and three primitives: one
           per component plus one naming a component that is not there. This is
           the shape of a real assembled model — a bed's frame and its bedding —
           and the case the reader used to answer by drawing the first part and
           reporting the count of the rest. */
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
        /* The third names component nine, which does not exist. Dropped rather
           than pointed at component zero, which would put one part's faces onto
           another part's vertices and look almost right. */
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

            /* The file numbers each part's faces from zero. Merged into one
               array they have to be shifted, or the bedding draws itself on the
               frame's vertices. */
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

            /* The second component sits ten units along x. If only the first
               had been read the box would stop at one, which is exactly what
               the old reader produced and called a model. */
            geometryMeshGetBounds(&model, minimum, maximum);
            checkThat(&failureCount, "the box spans both parts",
                      nearly(minimum[0], 0.0f) && nearly(maximum[0], 11.0f));
        }
    }

    printf("\n-- a skinned model --\n");
    {
        static Builder builder;
        GeometryMesh skinned;

        buildSkinnedContainer(&builder, 4U, BOOLEAN_TRUE);
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
        /* Packed 0xFFFF0502: bone 2 in the first slot, bone 5 in the second and
           nothing after. Read the other way round this is bone 255 twice, which
           would silently unweight the vertex rather than fail. */
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
        /* Stored 0.25 and 0.5, so the third bone carries the remaining quarter.
           A reader that copies only what is written leaves this vertex at three
           quarters of its own weight and shrinks the mesh towards the origin. */
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

            /* Assignments with no weights name bones in no proportion. Taking
               them anyway and weighting each vertex fully to its first bone
               would draw a body rigidly welded to its own joints, which looks
               like a skinning bug rather than a missing element. */
            buildSkinnedContainer(&builder, 4U, BOOLEAN_FALSE);
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
        /* Without this the assignment slots mean nothing: slot 1 is a different
           bone on the head than on the hands, and a mesh skinned as though it
           were the same folds itself inside out. */
        checkThat(&failureCount, "keeps the bone list", skinned.primitives[0].boneRemap != NULL_POINTER);
        checkThat(&failureCount, "with the count the file gave",
                  skinned.primitives[0].boneRemapCount == 3U);
        checkThat(&failureCount, "and the bones in it",
                  skinned.primitives[0].boneRemap[0] == 4U &&
                      skinned.primitives[0].boneRemap[1] == 9U &&
                      skinned.primitives[0].boneRemap[2] == 2U);

        printf("\n-- does the container's own bind pose arrive --\n");
        /* The section after the primitives, which an earlier reader stopped
           short of entirely. What is in it is the inverse bind — measured on a
           retail container, where a bone's world transform out of the tree
           times the stored transform landed on the identity — so reading it is
           what lets a pose palette be built without a matrix inverse. */
        checkThat(&failureCount, "the bind pose is read", skinned.bindPoses != NULL_POINTER);
        checkThat(&failureCount, "with one entry per bone the file described",
                  skinned.bindPoseCount == 10U);
        checkThat(&failureCount, "indexed by the bone number and not by the slot that named it",
                  nearly(skinned.bindPoses[9].translation[0], 9.0f) &&
                      nearly(skinned.bindPoses[4].translation[0], 4.0f));
        checkThat(&failureCount, "reading the quaternion in the order the file writes it",
                  nearly(skinned.bindPoses[0].rotation[3], 1.0f) &&
                      nearly(skinned.bindPoses[0].rotation[0], 0.0f));

        printf("\n-- joining several containers into one model --\n");
        /* A Sim is a body, a face and hair: three containers that have to end up
           as one mesh, because one upload painted a part at a time is what the
           renderer can do and three meshes is not. */
        {
            static GeometryMesh whole;
            const GeometryMesh *parts[2];
            GeometryReadResult joined;
            Unsigned32 before = skinned.vertexCount;

            parts[0] = &skinned;
            parts[1] = &mesh; /* the teapot, which carries no bones at all */
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

                /* The second model's indices addressed its own vertices, which
                   are no longer at nought. Unshifted they would reach into the
                   first model and draw triangles between the two. */
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

                /* The ranges are what a per-part draw will use, so a part whose
                   range was not shifted would paint another part's triangles. */
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
            }
        }

        printf("\n-- posing by the skeleton --\n");
        {
            /* Two bones that only translate, so where a vertex lands is
               arithmetic anyone can check by hand rather than a rotation to be
               taken on trust. Bone 4 moves ten along x, bone 9 moves a hundred
               along y, and the rest are identity. */
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

            /* Vertex 0 sits at the origin on slots 0, 1 and 2 — bones 4, 9 and
               2 — weighted a quarter, a half, and the quarter the file left
               implied. Bone 2 is identity, so x moves a quarter of ten and y
               half of a hundred, and both are arithmetic rather than trust. */
            checkThat(&failureCount, "moves every weighted vertex",
                      geometryMeshApplySkin(&skinned, palette, 10U) == 3U);
            checkThat(&failureCount, "blending the translations by their weights",
                      nearly(skinned.positions[0], 2.5f) && nearly(skinned.positions[1], 50.0f));

            printf("\n-- a bone that undoes itself moves nothing --\n");
            {
                GeometryMesh resting;
                static Real32 bindPalette[10 * 16];

                /* Pose times inverse bind, which at rest is the identity for
                   every bone however far from the origin that bone sits. Any
                   movement here is the caller having passed world transforms
                   instead — the mistake that drew a face with a limb stretched
                   out of it, and the reason this check exists rather than a
                   comment saying be careful. */
                buildSkinnedContainer(&builder, 4U, BOOLEAN_TRUE);
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

                buildSkinnedContainer(&builder, 4U, BOOLEAN_TRUE);
                (void)geometryReaderOpen(&again, builder.bytes, builder.length, &arena);
                /* A palette too short for the bones named. Moving nothing is
                   the right answer: a vertex blended from bones that are not
                   there would collapse onto the origin, and a model in a heap
                   at the origin reads as broken maths rather than as a palette
                   that was never built. */
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

        /* Sixty elements. A retail body mesh carries one per morph target and
           per bone array, and a ceiling of thirty-two refused 238 of the 239
           readable containers on a real disc — every one of them for this. */
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

        /* A count is only believable if the resource is big enough to hold that
           many elements. Refusing on the arithmetic means a corrupt count never
           becomes an allocation. */
        {
            static const Unsigned8 wildCount[] = {
                0x01U, 0x00U, 0xFFU, 0xFFU, /* mark */
                0x00U, 0x00U, 0x00U, 0x00U, /* no links */
                0x01U, 0x00U, 0x00U, 0x00U, /* one block */
                0x87U, 0x86U, 0x4FU, 0xACU, /* its type */
                0x01U, 'x',  0x87U, 0x86U, 0x4FU, 0xACU, 0x04U, 0x00U, 0x00U, 0x00U,
                0x01U, 'y',  0x00U, 0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U,
                0x01U, 'z',  /* the resource name */
                0xFFU, 0xFFU, 0xFFU, 0x0FU  /* a quarter of a billion elements */
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
            /* An older collection mark. Not readable here, but saying so as a
               version rather than as rubbish is the difference between knowing
               what a disc holds and guessing. */
            static const Unsigned8 olderMark[16] = { 0x01U, 0x00U, 0xFEU, 0xFFU, 0U };

            checkThat(&failureCount, "calls an older collection mark a version, not rubbish",
                      geometryReaderOpen(&other, olderMark, sizeof(olderMark), &arena) ==
                          GEOMETRY_READ_OLDER_COLLECTION);
            checkThat(&failureCount, "and reports which mark it was",
                      other.versionMark == 0xFFFE0001UL);
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

    printf("\n-- moving a mesh by a transform --\n");
    {
        /* A quarter turn about z, then two along x, written column major the
           way the node reader hands it over. A reader that treated it as row
           major would answer (2, -1, 0) for the first vertex rather than
           (2, 1, 0), which is a mistake that looks like a model facing the
           wrong way rather than one being read wrong. */
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
        /* Normals take the rotation and not the translation. A direction that
           picked up the shift would stop being a direction — and would light
           the model as though every face pointed away from the origin. */
        checkThat(&failureCount, "a normal turns without moving",
                  nearly(normals[0], 0.0F) && nearly(normals[1], 1.0F) &&
                      nearly(normals[2], 0.0F));
        checkThat(&failureCount, "and one along the axis of the turn is unchanged",
                  nearly(normals[3], 0.0F) && nearly(normals[4], 0.0F) &&
                      nearly(normals[5], 1.0F));
    }

    return checkSummarize(failureCount, "geometry reader");
}
