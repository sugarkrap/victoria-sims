#include "victoria/geometryReader.h"

#include "victoria/resourceCollection.h"

#define GEOMETRY_TYPE_IDENTIFIER 0xAC4F8687UL

/* Element payload layouts. The file records a format per element; only the two
 * that carry the geometry we draw are handled, and anything else is skipped
 * whole rather than guessed at. */
#define ELEMENT_FORMAT_TWO_FLOATS 1UL
#define ELEMENT_FORMAT_THREE_FLOATS 2UL

/* Indices narrowed from words to half words at block version 3. Below that they
 * are full words, everywhere they appear — the component's element list and the
 * primitive's faces alike.
 *
 * This used to refuse anything under 3 rather than read it on a guess. A retail
 * disc then refused 238 of 282 meshes for exactly that reason, which turned a
 * cautious gate into the single largest thing standing between the engine and
 * the game. The widths are now handled instead of avoided. */
#define MINIMUM_BLOCK_VERSION 1UL

/* Half word indices cannot address more vertices than this, so a container
 * claiming more is either not what it says or beyond what we can carry. */
#define LARGEST_ADDRESSABLE_VERTEX_COUNT 65536U

/* How wide one index is at a given block version. */
static MemorySize indexWidth(Unsigned32 version)
{
    return (version < 3UL) ? 4UL : 2UL;
}

/* An index array: a count, then that many words or half words. Returns where
 * the array starts so a caller can come back for it, having skipped it here. */
static Unsigned32 skipIndexArray(ResourceCursor *cursor, Unsigned32 version, MemorySize *startPosition)
{
    Unsigned32 count = resourceCursorReadUnsigned32(cursor);

    if (startPosition != NULL_POINTER)
    {
        *startPosition = cursor->position;
    }
    resourceCursorSkip(cursor, (MemorySize)count * indexWidth(version));
    return count;
}

const char *geometryReadResultGetName(GeometryReadResult result)
{
    switch (result)
    {
    case GEOMETRY_READ_OK:
        return "ok";
    case GEOMETRY_READ_NOT_A_RESOURCE:
        return "not a scenegraph resource";
    case GEOMETRY_READ_WRONG_TYPE:
        return "not a geometry data container";
    case GEOMETRY_READ_UNSUPPORTED_VERSION:
        return "a container version this reader does not decode";
    case GEOMETRY_READ_TRUNCATED:
        return "the resource ends part way through";
    case GEOMETRY_READ_NO_GEOMETRY:
        return "the container holds no drawable geometry";
    case GEOMETRY_READ_OUT_OF_ARENA:
        return "not enough arena space for the mesh";
    case GEOMETRY_READ_OLDER_COLLECTION:
        return "an older collection, laid out differently";
    case GEOMETRY_READ_TOO_MANY_ELEMENTS:
        return "more geometry elements than the resource has room for";
    case GEOMETRY_READ_TOO_MANY_VERTICES:
        return "more vertices than a half word index can address";
    default:
        return "unknown";
    }
}

/* Where one element's payload begins, and how many values it holds. */
typedef struct ElementSpan
{
    Unsigned32 identifier;
    Unsigned32 format;
    MemorySize payloadPosition;
    Unsigned32 payloadBytes;
} ElementSpan;

/* The smallest an element can be on disc: six words of header and the word
 * counting its index array. A count claiming more elements than that many bytes
 * allows is describing a resource larger than the one it arrived in, so it can
 * be refused before anything is allocated on the strength of it.
 *
 * This replaces a flat ceiling of 32, which refused 238 of the 239 readable
 * containers on a retail disc. There was never a reason for a fixed limit —
 * a body mesh carries an element per morph target and per bone array, and
 * thirty-two is simply fewer than the game ships. */
#define SMALLEST_ELEMENT_BYTES 28UL

static Real32 *copyRealArray(MemoryArena *arena, ResourceCursor *cursor, const ElementSpan *span,
                             Unsigned32 componentCount, Unsigned32 vertexCount)
{
    Real32 *values;
    Unsigned32 index;
    MemorySize total = (MemorySize)vertexCount * (MemorySize)componentCount;

    if ((MemorySize)span->payloadBytes < total * 4UL)
    {
        return NULL_POINTER;
    }
    values = (Real32 *)memoryArenaAllocate(arena, total * sizeof(Real32), sizeof(Real32));
    if (values == NULL_POINTER)
    {
        return NULL_POINTER;
    }

    cursor->position = span->payloadPosition;
    cursor->overran = BOOLEAN_FALSE;
    for (index = 0U; index < total; index++)
    {
        values[index] = resourceCursorReadReal32(cursor);
    }
    return cursor->overran ? NULL_POINTER : values;
}

/* The collection result, said in the geometry reader's own words. Callers count
 * these, so a cause that arrives here must not be folded into a neighbouring
 * one on the way. */
static GeometryReadResult translateCollectionResult(ResourceCollectionResult result)
{
    switch (result)
    {
    case RESOURCE_COLLECTION_NOT_A_RESOURCE:
        return GEOMETRY_READ_NOT_A_RESOURCE;
    case RESOURCE_COLLECTION_OLDER:
        return GEOMETRY_READ_OLDER_COLLECTION;
    case RESOURCE_COLLECTION_NO_BLOCKS:
        return GEOMETRY_READ_NO_GEOMETRY;
    default:
        return GEOMETRY_READ_TRUNCATED;
    }
}

GeometryReadResult geometryReaderOpen(GeometryMesh *mesh, const Unsigned8 *bytes, MemorySize sizeInBytes,
                                      MemoryArena *arena)
{
    ResourceCursor cursor;
    ResourceCollection collection;
    ResourceCollectionResult collectionResult;
    PersistTypeInfo blockType;
    ElementSpan *spans;
    const ElementSpan *positionSpan = NULL_POINTER;
    const ElementSpan *normalSpan = NULL_POINTER;
    const ElementSpan *textureSpan = NULL_POINTER;
    Unsigned32 blockVersion;
    Unsigned32 elementCount;
    Unsigned32 componentCount;
    Unsigned32 vertexCount = 0U;
    Unsigned32 index;
    MemorySize faceStart = 0UL;
    Unsigned32 faceCount = 0U;
    Unsigned16 *indices;

    mesh->name[0] = '\0';
    mesh->resourceName[0] = '\0';
    mesh->positions = NULL_POINTER;
    mesh->normals = NULL_POINTER;
    mesh->textureCoordinates = NULL_POINTER;
    mesh->vertexCount = 0U;
    mesh->indices = NULL_POINTER;
    mesh->indexCount = 0U;
    mesh->primitiveCount = 0U;
    mesh->versionMark = 0U;
    mesh->containerVersion = 0U;
    mesh->elementCount = 0U;

    /* The wrapper — version mark, links to resources elsewhere, the block type
     * list — is the same for every scenegraph resource and is read in one
     * place. Following the links is the scenegraph's job, not this reader's. */
    collectionResult = resourceCollectionOpen(&collection, &cursor, bytes, sizeInBytes);
    mesh->versionMark = collection.versionMark;
    if (collectionResult != RESOURCE_COLLECTION_OK)
    {
        return translateCollectionResult(collectionResult);
    }

    resourceCursorReadTypeInformation(&cursor, &blockType);
    if (cursor.overran)
    {
        return GEOMETRY_READ_TRUNCATED;
    }
    if (blockType.typeIdentifier != (Unsigned32)GEOMETRY_TYPE_IDENTIFIER)
    {
        return GEOMETRY_READ_WRONG_TYPE;
    }
    blockVersion = blockType.version;
    mesh->containerVersion = blockVersion;
    if (blockVersion < MINIMUM_BLOCK_VERSION)
    {
        return GEOMETRY_READ_UNSUPPORTED_VERSION;
    }

    /* The resource's own name, behind another type prefix. */
    resourceCursorReadTypeInformation(&cursor, NULL_POINTER);
    resourceCursorReadString(&cursor, mesh->resourceName, GEOMETRY_NAME_LIMIT);

    elementCount = resourceCursorReadUnsigned32(&cursor);
    /* Checked before the counts are believed. A resource that stops here has a
     * zero element count only because the read failed, and reporting that as
     * "holds no geometry" would blame the file for our own short buffer. */
    if (cursor.overran)
    {
        return GEOMETRY_READ_TRUNCATED;
    }
    mesh->elementCount = elementCount;
    if ((MemorySize)elementCount > sizeInBytes / SMALLEST_ELEMENT_BYTES)
    {
        return GEOMETRY_READ_TOO_MANY_ELEMENTS;
    }
    if (elementCount == 0U)
    {
        return GEOMETRY_READ_NO_GEOMETRY;
    }
    /* From the arena rather than the stack, so the count comes from the file
     * instead of from a number picked here. It is given back with everything
     * else when a caller rewinds after a refusal. */
    spans = (ElementSpan *)memoryArenaAllocate(arena, (MemorySize)elementCount * sizeof(ElementSpan),
                                               sizeof(MemorySize));
    if (spans == NULL_POINTER)
    {
        return GEOMETRY_READ_OUT_OF_ARENA;
    }
    for (index = 0U; index < elementCount; index++)
    {
        Unsigned32 payloadBytes;

        (void)resourceCursorReadUnsigned32(&cursor);
        spans[index].identifier = resourceCursorReadUnsigned32(&cursor);
        (void)resourceCursorReadUnsigned32(&cursor);
        spans[index].format = resourceCursorReadUnsigned32(&cursor);
        (void)resourceCursorReadUnsigned32(&cursor);
        payloadBytes = resourceCursorReadUnsigned32(&cursor);

        spans[index].payloadBytes = payloadBytes;
        spans[index].payloadPosition = cursor.position;
        resourceCursorSkip(&cursor, (MemorySize)payloadBytes);
        (void)skipIndexArray(&cursor, blockVersion, NULL_POINTER);

        if (cursor.overran)
        {
            return GEOMETRY_READ_TRUNCATED;
        }
    }

    /* Components tie a set of elements together and say how many vertices they
     * describe. The first is the one the first primitive draws from. */
    componentCount = resourceCursorReadUnsigned32(&cursor);
    if (cursor.overran)
    {
        return GEOMETRY_READ_TRUNCATED;
    }
    if (componentCount == 0U)
    {
        return GEOMETRY_READ_NO_GEOMETRY;
    }
    for (index = 0U; index < componentCount; index++)
    {
        Unsigned32 elementIndexCount;
        MemorySize elementIndexStart;
        Unsigned32 componentVertexCount;
        Unsigned32 inner;

        elementIndexCount = skipIndexArray(&cursor, blockVersion, &elementIndexStart);
        componentVertexCount = resourceCursorReadUnsigned32(&cursor);
        (void)resourceCursorReadUnsigned32(&cursor);
        (void)skipIndexArray(&cursor, blockVersion, NULL_POINTER);
        (void)skipIndexArray(&cursor, blockVersion, NULL_POINTER);
        (void)skipIndexArray(&cursor, blockVersion, NULL_POINTER);

        if (cursor.overran)
        {
            return GEOMETRY_READ_TRUNCATED;
        }
        if (index != 0U)
        {
            continue;
        }

        vertexCount = componentVertexCount;
        for (inner = 0U; inner < elementIndexCount; inner++)
        {
            ResourceCursor elementCursor = cursor;
            Unsigned32 which;

            elementCursor.position = elementIndexStart + ((MemorySize)inner * indexWidth(blockVersion));
            elementCursor.overran = BOOLEAN_FALSE;
            which = (blockVersion < 3UL)
                        ? resourceCursorReadUnsigned32(&elementCursor)
                        : (Unsigned32)resourceCursorReadUnsigned16(&elementCursor);
            if (which >= elementCount)
            {
                continue;
            }

            if (spans[which].identifier == (Unsigned32)GEOMETRY_ELEMENT_POSITION &&
                spans[which].format == ELEMENT_FORMAT_THREE_FLOATS)
            {
                positionSpan = &spans[which];
            }
            else if (spans[which].identifier == (Unsigned32)GEOMETRY_ELEMENT_NORMAL &&
                     spans[which].format == ELEMENT_FORMAT_THREE_FLOATS)
            {
                normalSpan = &spans[which];
            }
            else if (spans[which].identifier == (Unsigned32)GEOMETRY_ELEMENT_TEXTURE_COORDINATE &&
                     spans[which].format == ELEMENT_FORMAT_TWO_FLOATS)
            {
                textureSpan = &spans[which];
            }
        }
    }

    mesh->primitiveCount = resourceCursorReadUnsigned32(&cursor);
    if (cursor.overran)
    {
        return GEOMETRY_READ_TRUNCATED;
    }
    if (mesh->primitiveCount == 0U)
    {
        return GEOMETRY_READ_NO_GEOMETRY;
    }

    /* Only the first primitive. Its faces index into the component's vertices. */
    (void)resourceCursorReadUnsigned32(&cursor);
    (void)resourceCursorReadUnsigned32(&cursor);
    resourceCursorReadString(&cursor, mesh->name, GEOMETRY_NAME_LIMIT);
    faceCount = skipIndexArray(&cursor, blockVersion, &faceStart);
    if (cursor.overran)
    {
        return GEOMETRY_READ_TRUNCATED;
    }
    if (faceCount == 0U || vertexCount == 0U || positionSpan == NULL_POINTER)
    {
        return GEOMETRY_READ_NO_GEOMETRY;
    }
    if (vertexCount > LARGEST_ADDRESSABLE_VERTEX_COUNT)
    {
        return GEOMETRY_READ_TOO_MANY_VERTICES;
    }

    mesh->positions = copyRealArray(arena, &cursor, positionSpan, 3U, vertexCount);
    if (mesh->positions == NULL_POINTER)
    {
        return GEOMETRY_READ_OUT_OF_ARENA;
    }
    if (normalSpan != NULL_POINTER)
    {
        mesh->normals = copyRealArray(arena, &cursor, normalSpan, 3U, vertexCount);
    }
    if (textureSpan != NULL_POINTER)
    {
        mesh->textureCoordinates = copyRealArray(arena, &cursor, textureSpan, 2U, vertexCount);
    }

    indices = (Unsigned16 *)memoryArenaAllocate(arena, (MemorySize)faceCount * sizeof(Unsigned16),
                                                sizeof(Unsigned16));
    if (indices == NULL_POINTER)
    {
        return GEOMETRY_READ_OUT_OF_ARENA;
    }

    cursor.position = faceStart;
    cursor.overran = BOOLEAN_FALSE;
    for (index = 0U; index < faceCount; index++)
    {
        Unsigned32 value = (blockVersion < 3UL) ? resourceCursorReadUnsigned32(&cursor)
                                                : (Unsigned32)resourceCursorReadUnsigned16(&cursor);

        /* An index outside the component would read a vertex that is not there.
         * Clamping keeps a malformed model from reading past the arrays. */
        indices[index] = (value < vertexCount) ? (Unsigned16)value : (Unsigned16)(vertexCount - 1U);
    }
    if (cursor.overran)
    {
        return GEOMETRY_READ_TRUNCATED;
    }

    mesh->vertexCount = vertexCount;
    mesh->indices = indices;
    mesh->indexCount = faceCount;
    return GEOMETRY_READ_OK;
}

void geometryMeshGetBounds(const GeometryMesh *mesh, Real32 *minimum, Real32 *maximum)
{
    Unsigned32 index;
    Unsigned32 axis;

    for (axis = 0U; axis < 3U; axis++)
    {
        minimum[axis] = 0.0f;
        maximum[axis] = 0.0f;
    }
    if (mesh->positions == NULL_POINTER || mesh->vertexCount == 0U)
    {
        return;
    }

    for (axis = 0U; axis < 3U; axis++)
    {
        minimum[axis] = mesh->positions[axis];
        maximum[axis] = mesh->positions[axis];
    }
    for (index = 1U; index < mesh->vertexCount; index++)
    {
        for (axis = 0U; axis < 3U; axis++)
        {
            Real32 value = mesh->positions[(MemorySize)index * 3UL + axis];

            if (value < minimum[axis])
            {
                minimum[axis] = value;
            }
            if (value > maximum[axis])
            {
                maximum[axis] = value;
            }
        }
    }
}
