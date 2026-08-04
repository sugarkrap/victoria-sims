#include "victoria/geometryReader.h"

#include "utils/strings.h"
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

/* One component's floats, written where they belong in the merged array.
 *
 * Returns false when the element does not hold as many values as the component
 * claims vertices, which is a file disagreeing with itself rather than a short
 * read, and is not something to paper over with whatever follows the payload. */
static Boolean copyRealValues(Real32 *destination, ResourceCursor *cursor, const ElementSpan *span,
                              Unsigned32 valuesPerVertex, Unsigned32 vertexCount)
{
    Unsigned32 index;
    MemorySize total = (MemorySize)vertexCount * (MemorySize)valuesPerVertex;

    if (span == NULL_POINTER || (MemorySize)span->payloadBytes < total * 4UL)
    {
        return BOOLEAN_FALSE;
    }
    cursor->position = span->payloadPosition;
    cursor->overran = BOOLEAN_FALSE;
    for (index = 0U; index < total; index++)
    {
        destination[index] = resourceCursorReadReal32(cursor);
    }
    return cursor->overran ? BOOLEAN_FALSE : BOOLEAN_TRUE;
}

/* What one component contributes to the merged mesh. */
typedef struct ComponentSpan
{
    const ElementSpan *position;
    const ElementSpan *normal;
    const ElementSpan *texture;
    Unsigned32 vertexCount;
    /* Where this component's vertices start once they are all in one array. A
       primitive's faces are relative to its own component, so this is what
       turns them into indices anyone can draw. */
    Unsigned32 baseVertex;
} ComponentSpan;

/* Where a primitive's faces are in the file, alongside what it will become.
 * Recorded on a first pass because the index array cannot be sized until every
 * primitive has been counted. */
typedef struct PrimitiveSpan
{
    MemorySize faceStart;
    Unsigned32 faceCount;
    Unsigned32 componentIndex;
} PrimitiveSpan;

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
    ComponentSpan *components;
    PrimitiveSpan *primitiveSpans;
    GeometryPrimitive *primitives;
    Unsigned32 blockVersion;
    Unsigned32 elementCount;
    Unsigned32 componentCount;
    Unsigned32 primitiveCount;
    Unsigned32 vertexCount = 0U;
    Unsigned32 indexTotal = 0U;
    Unsigned32 storedPrimitives = 0U;
    Unsigned32 index;
    Boolean anyNormals = BOOLEAN_FALSE;
    Boolean anyTextures = BOOLEAN_FALSE;
    Real32 *positions;
    Real32 *normals = NULL_POINTER;
    Real32 *textures = NULL_POINTER;
    Unsigned16 *indices;

    mesh->name[0] = '\0';
    mesh->resourceName[0] = '\0';
    mesh->positions = NULL_POINTER;
    mesh->normals = NULL_POINTER;
    mesh->textureCoordinates = NULL_POINTER;
    mesh->vertexCount = 0U;
    mesh->indices = NULL_POINTER;
    mesh->indexCount = 0U;
    mesh->primitives = NULL_POINTER;
    mesh->primitiveCount = 0U;
    mesh->storedPrimitiveCount = 0U;
    mesh->componentCount = 0U;
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
     * describe. A model with separate parts has one per part, and their vertex
     * arrays are separate in the file — so they are read here and stacked into
     * one, with each component remembering where its own vertices landed. */
    componentCount = resourceCursorReadUnsigned32(&cursor);
    if (cursor.overran)
    {
        return GEOMETRY_READ_TRUNCATED;
    }
    if (componentCount == 0U)
    {
        return GEOMETRY_READ_NO_GEOMETRY;
    }
    mesh->componentCount = componentCount;
    /* A component costs at least six words on disc: its element list, its
     * vertex count, a word beside it and three more index arrays. */
    if ((MemorySize)componentCount > sizeInBytes / 24UL)
    {
        return GEOMETRY_READ_TRUNCATED;
    }
    components = (ComponentSpan *)memoryArenaAllocate(
        arena, (MemorySize)componentCount * sizeof(ComponentSpan), sizeof(MemorySize));
    if (components == NULL_POINTER)
    {
        return GEOMETRY_READ_OUT_OF_ARENA;
    }

    for (index = 0U; index < componentCount; index++)
    {
        Unsigned32 elementIndexCount;
        MemorySize elementIndexStart;
        Unsigned32 inner;

        components[index].position = NULL_POINTER;
        components[index].normal = NULL_POINTER;
        components[index].texture = NULL_POINTER;
        components[index].baseVertex = vertexCount;

        elementIndexCount = skipIndexArray(&cursor, blockVersion, &elementIndexStart);
        components[index].vertexCount = resourceCursorReadUnsigned32(&cursor);
        (void)resourceCursorReadUnsigned32(&cursor);
        (void)skipIndexArray(&cursor, blockVersion, NULL_POINTER);
        (void)skipIndexArray(&cursor, blockVersion, NULL_POINTER);
        (void)skipIndexArray(&cursor, blockVersion, NULL_POINTER);

        if (cursor.overran)
        {
            return GEOMETRY_READ_TRUNCATED;
        }
        /* Summed before it is used, so a total past what a half word index can
         * reach is refused rather than silently wrapped. */
        if ((MemorySize)vertexCount + (MemorySize)components[index].vertexCount >
            (MemorySize)LARGEST_ADDRESSABLE_VERTEX_COUNT)
        {
            return GEOMETRY_READ_TOO_MANY_VERTICES;
        }
        vertexCount += components[index].vertexCount;

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
                components[index].position = &spans[which];
            }
            else if (spans[which].identifier == (Unsigned32)GEOMETRY_ELEMENT_NORMAL &&
                     spans[which].format == ELEMENT_FORMAT_THREE_FLOATS)
            {
                components[index].normal = &spans[which];
                anyNormals = BOOLEAN_TRUE;
            }
            else if (spans[which].identifier == (Unsigned32)GEOMETRY_ELEMENT_TEXTURE_COORDINATE &&
                     spans[which].format == ELEMENT_FORMAT_TWO_FLOATS)
            {
                components[index].texture = &spans[which];
                anyTextures = BOOLEAN_TRUE;
            }
        }
    }

    if (vertexCount == 0U)
    {
        return GEOMETRY_READ_NO_GEOMETRY;
    }

    primitiveCount = resourceCursorReadUnsigned32(&cursor);
    if (cursor.overran)
    {
        return GEOMETRY_READ_TRUNCATED;
    }
    mesh->primitiveCount = primitiveCount;
    if (primitiveCount == 0U)
    {
        return GEOMETRY_READ_NO_GEOMETRY;
    }
    /* Two words, a name of at least one byte and a face count: sixteen bytes at
     * the very least. */
    if ((MemorySize)primitiveCount > sizeInBytes / 16UL)
    {
        return GEOMETRY_READ_TRUNCATED;
    }

    primitiveSpans = (PrimitiveSpan *)memoryArenaAllocate(
        arena, (MemorySize)primitiveCount * sizeof(PrimitiveSpan), sizeof(MemorySize));
    primitives = (GeometryPrimitive *)memoryArenaAllocate(
        arena, (MemorySize)primitiveCount * sizeof(GeometryPrimitive), sizeof(MemorySize));
    if (primitiveSpans == NULL_POINTER || primitives == NULL_POINTER)
    {
        return GEOMETRY_READ_OUT_OF_ARENA;
    }

    /* First pass: find every primitive and total its faces. The index array
     * cannot be sized until they have all been counted, and the file gives no
     * total of its own. */
    for (index = 0U; index < primitiveCount; index++)
    {
        Unsigned32 componentIndex;
        char name[GEOMETRY_NAME_LIMIT];

        /* The whole record, in order, every time round: a type, the component
         * it draws from, its name, its faces, the order it should be drawn in
         * among transparent parts, and the bone indices it remaps.
         *
         * All of it is consumed even when the primitive is then dropped. The
         * version of this that read only the first primitive could stop at the
         * faces and never noticed the two fields after them; iterating cannot,
         * because whatever is left unread is where the next primitive is
         * expected to start. */
        (void)resourceCursorReadUnsigned32(&cursor);
        componentIndex = resourceCursorReadUnsigned32(&cursor);
        resourceCursorReadString(&cursor, name, GEOMETRY_NAME_LIMIT);
        primitiveSpans[storedPrimitives].faceCount =
            skipIndexArray(&cursor, blockVersion, &primitiveSpans[storedPrimitives].faceStart);
        (void)resourceCursorReadUnsigned32(&cursor);
        if (blockVersion > 1UL)
        {
            (void)skipIndexArray(&cursor, blockVersion, NULL_POINTER);
        }
        if (cursor.overran)
        {
            return GEOMETRY_READ_TRUNCATED;
        }

        /* A primitive naming a component that is not there has no vertices to
         * draw from. Dropped rather than pointed at component zero, which would
         * put one part's faces on another part's mesh. */
        if (componentIndex >= componentCount || primitiveSpans[storedPrimitives].faceCount == 0U ||
            components[componentIndex].position == NULL_POINTER)
        {
            continue;
        }

        primitiveSpans[storedPrimitives].componentIndex = componentIndex;
        primitives[storedPrimitives].componentIndex = componentIndex;
        primitives[storedPrimitives].firstIndex = indexTotal;
        primitives[storedPrimitives].indexCount = primitiveSpans[storedPrimitives].faceCount;
        primitives[storedPrimitives].name[0] = '\0';
        stringAppend(primitives[storedPrimitives].name, GEOMETRY_NAME_LIMIT, name);
        indexTotal += primitiveSpans[storedPrimitives].faceCount;
        storedPrimitives++;
    }

    if (storedPrimitives == 0U || indexTotal == 0U)
    {
        return GEOMETRY_READ_NO_GEOMETRY;
    }
    /* The name of the model as a whole, taken from its first part, which is what
     * the file offers and what a log wants. */
    stringAppend(mesh->name, GEOMETRY_NAME_LIMIT, primitives[0].name);

    positions = (Real32 *)memoryArenaAllocate(arena, (MemorySize)vertexCount * 3UL * sizeof(Real32),
                                              sizeof(Real32));
    if (positions == NULL_POINTER)
    {
        return GEOMETRY_READ_OUT_OF_ARENA;
    }
    if (anyNormals)
    {
        normals = (Real32 *)memoryArenaAllocate(arena, (MemorySize)vertexCount * 3UL * sizeof(Real32),
                                                sizeof(Real32));
        if (normals == NULL_POINTER)
        {
            return GEOMETRY_READ_OUT_OF_ARENA;
        }
    }
    if (anyTextures)
    {
        textures = (Real32 *)memoryArenaAllocate(arena, (MemorySize)vertexCount * 2UL * sizeof(Real32),
                                                 sizeof(Real32));
        if (textures == NULL_POINTER)
        {
            return GEOMETRY_READ_OUT_OF_ARENA;
        }
    }

    for (index = 0U; index < componentCount; index++)
    {
        const ComponentSpan *component = &components[index];
        MemorySize positionOffset = (MemorySize)component->baseVertex * 3UL;
        Unsigned32 inner;

        if (component->vertexCount == 0U)
        {
            continue;
        }
        if (component->position == NULL_POINTER)
        {
            /* No positions means nothing to draw from. Zeroed rather than left
             * as whatever the arena held, so a primitive that slipped through
             * draws a degenerate speck instead of reading somebody else's
             * numbers as coordinates. */
            for (inner = 0U; inner < component->vertexCount * 3U; inner++)
            {
                positions[positionOffset + inner] = 0.0f;
            }
        }
        else if (!copyRealValues(&positions[positionOffset], &cursor, component->position, 3U,
                                 component->vertexCount))
        {
            return GEOMETRY_READ_TRUNCATED;
        }

        if (normals != NULL_POINTER &&
            !copyRealValues(&normals[positionOffset], &cursor, component->normal, 3U,
                            component->vertexCount))
        {
            /* Straight up, so a part without normals is lit flatly rather than
             * black. Mixing a part that has them with one that does not is the
             * file's choice, and the vertex layout has to stay uniform either
             * way. */
            for (inner = 0U; inner < component->vertexCount; inner++)
            {
                normals[positionOffset + inner * 3U] = 0.0f;
                normals[positionOffset + inner * 3U + 1U] = 0.0f;
                normals[positionOffset + inner * 3U + 2U] = 1.0f;
            }
        }
        if (textures != NULL_POINTER &&
            !copyRealValues(&textures[(MemorySize)component->baseVertex * 2UL], &cursor,
                            component->texture, 2U, component->vertexCount))
        {
            for (inner = 0U; inner < component->vertexCount * 2U; inner++)
            {
                textures[(MemorySize)component->baseVertex * 2UL + inner] = 0.0f;
            }
        }
    }

    indices = (Unsigned16 *)memoryArenaAllocate(arena, (MemorySize)indexTotal * sizeof(Unsigned16),
                                                sizeof(Unsigned16));
    if (indices == NULL_POINTER)
    {
        return GEOMETRY_READ_OUT_OF_ARENA;
    }

    /* Second pass: the faces, shifted by where their component's vertices ended
     * up. After this the indices are absolute, so drawing all of them draws
     * every part of the model in one call. */
    for (index = 0U; index < storedPrimitives; index++)
    {
        const ComponentSpan *component = &components[primitiveSpans[index].componentIndex];
        Unsigned32 inner;

        cursor.position = primitiveSpans[index].faceStart;
        cursor.overran = BOOLEAN_FALSE;
        for (inner = 0U; inner < primitiveSpans[index].faceCount; inner++)
        {
            Unsigned32 value = (blockVersion < 3UL)
                                   ? resourceCursorReadUnsigned32(&cursor)
                                   : (Unsigned32)resourceCursorReadUnsigned16(&cursor);

            /* An index outside its own component would reach into the part
             * beside it now that they share an array, which is worse than
             * reading nothing. Clamped to the component it belongs to. */
            if (value >= component->vertexCount)
            {
                value = (component->vertexCount > 0U) ? component->vertexCount - 1U : 0U;
            }
            indices[primitives[index].firstIndex + inner] =
                (Unsigned16)(value + component->baseVertex);
        }
        if (cursor.overran)
        {
            return GEOMETRY_READ_TRUNCATED;
        }
    }

    mesh->positions = positions;
    mesh->normals = normals;
    mesh->textureCoordinates = textures;
    mesh->vertexCount = vertexCount;
    mesh->indices = indices;
    mesh->indexCount = indexTotal;
    mesh->primitives = primitives;
    mesh->storedPrimitiveCount = storedPrimitives;
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
