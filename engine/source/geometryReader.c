#include "victoria/geometryReader.h"

#include "utils/strings.h"
#include "victoria/resourceCollection.h"

#define GEOMETRY_TYPE_IDENTIFIER 0xAC4F8687UL

/* Element payload layouts. The file records a format per element; anything not
 * handled is skipped whole rather than guessed at. */
#define ELEMENT_FORMAT_ONE_FLOAT 0UL
#define ELEMENT_FORMAT_TWO_FLOATS 1UL
#define ELEMENT_FORMAT_THREE_FLOATS 2UL

const char *geometryElementGetName(Unsigned32 identifier)
{
    /* The names the format's own table gives, in-game spelling where there is
       one, since that is what the exporter wrote and what a search will find. */
    switch (identifier)
    {
    case (Unsigned32)GEOMETRY_ELEMENT_POSITION:
        return "positions";
    case (Unsigned32)GEOMETRY_ELEMENT_NORMAL:
        return "normals";
    case (Unsigned32)GEOMETRY_ELEMENT_TEXTURE_COORDINATE:
        return "texture coordinates";
    case (Unsigned32)GEOMETRY_ELEMENT_TANGENT:
        return "tangents";
    case (Unsigned32)GEOMETRY_ELEMENT_BONE_ASSIGNMENT:
        return "bone assignments";
    case (Unsigned32)GEOMETRY_ELEMENT_BONE_WEIGHT:
        return "bone weights";
    case 0x69D92B93UL:
        return "binormals";
    case 0x9BB38AFBUL:
        return "binormals, the other kind";
    case 0xDB830795UL:
        return "colours";
    case 0xEB720693UL:
        return "colour deltas";
    case 0xCB7206A1UL:
        return "texture coordinate deltas";
    case 0x5CF2CFE1UL:
        return "morph vertex deltas";
    case 0xCB6F3A6AUL:
        return "morph normal deltas";
    case 0x1C4AFC56UL:
        return "morph vertex indices";
    case 0x7C4DEE82UL:
        return "morph normal indices";
    case 0xDCF2CFDCUL:
        return "morph vertex map";
    case 0x114113C3UL:
        return "vertex identifiers";
    case 0x114113CDUL:
        return "region mask";
    default:
        return NULL_POINTER;
    }
}

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

/* One packed word per vertex, unpacked into four slots.
 *
 * The word's low byte is the first slot, which is the order the weights come
 * in. Read a byte at a time rather than cast, because the payload is at
 * whatever offset the file put it at and an unaligned word read is a fault on
 * ARMv5 rather than a slow path. */
static Boolean copyBoneAssignments(Unsigned8 *destination, ResourceCursor *cursor,
                                   const ElementSpan *span, Unsigned32 vertexCount)
{
    Unsigned32 vertex;

    if (span == NULL_POINTER || (MemorySize)span->payloadBytes < (MemorySize)vertexCount * 4UL)
    {
        return BOOLEAN_FALSE;
    }
    cursor->position = span->payloadPosition;
    cursor->overran = BOOLEAN_FALSE;
    for (vertex = 0U; vertex < vertexCount; vertex++)
    {
        Unsigned32 packed = resourceCursorReadUnsigned32(cursor);
        Unsigned32 slot;

        for (slot = 0U; slot < 4U; slot++)
        {
            destination[vertex * 4U + slot] = (Unsigned8)((packed >> (slot * 8U)) & 0xFFU);
        }
    }
    return cursor->overran ? BOOLEAN_FALSE : BOOLEAN_TRUE;
}

/* One, two or three weights per vertex into three slots.
 *
 * The file leaves the last weight out and expects it worked back from the
 * others, since they sum to one. Doing that here rather than in each caller is
 * the difference between one place that knows the rule and several that half
 * remember it. A remainder below zero means the stored weights already sum past
 * one, which is a file this cannot fix — clamped, not rejected, because the
 * vertex still has to go somewhere. */
static Boolean copyBoneWeights(Real32 *destination, ResourceCursor *cursor, const ElementSpan *span,
                               Unsigned32 storedPerVertex, Unsigned32 vertexCount)
{
    Unsigned32 vertex;

    if (span == NULL_POINTER || storedPerVertex == 0U || storedPerVertex > 3U ||
        (MemorySize)span->payloadBytes < (MemorySize)vertexCount * (MemorySize)storedPerVertex * 4UL)
    {
        return BOOLEAN_FALSE;
    }
    cursor->position = span->payloadPosition;
    cursor->overran = BOOLEAN_FALSE;
    for (vertex = 0U; vertex < vertexCount; vertex++)
    {
        Real32 remainder = 1.0f;
        Unsigned32 slot;

        /* Four slots for three stored weights: the fourth bone's weight is the
           one the file left out. */
        for (slot = 0U; slot < 4U; slot++)
        {
            Real32 weight = 0.0f;

            if (slot < storedPerVertex)
            {
                weight = resourceCursorReadReal32(cursor);
                remainder -= weight;
            }
            else if (slot == storedPerVertex)
            {
                weight = (remainder > 0.0f) ? remainder : 0.0f;
            }
            destination[vertex * 4U + slot] = weight;
        }
    }
    return cursor->overran ? BOOLEAN_FALSE : BOOLEAN_TRUE;
}

/* Notes an element kind the mesh did not end up using.
 *
 * Once per kind rather than once per component, so a model with several
 * components does not report the same element a dozen times. */
static void rememberUnusedElement(GeometryMesh *mesh, Unsigned32 identifier, Unsigned32 format)
{
    Unsigned32 seen;

    for (seen = 0U; seen < mesh->unusedElementCount; seen++)
    {
        if (mesh->unusedElements[seen] == identifier && mesh->unusedElementFormats[seen] == format)
        {
            return;
        }
    }
    if (mesh->unusedElementCount < (Unsigned32)GEOMETRY_UNUSED_ELEMENT_LIMIT)
    {
        mesh->unusedElements[mesh->unusedElementCount] = identifier;
        mesh->unusedElementFormats[mesh->unusedElementCount] = format;
        mesh->unusedElementCount++;
    }
}

/* What one component contributes to the merged mesh. */
typedef struct ComponentSpan
{
    const ElementSpan *position;
    const ElementSpan *normal;
    const ElementSpan *texture;
    const ElementSpan *boneAssignment;
    const ElementSpan *boneWeight;
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
    /* Where the primitive's bone list is, and how long. */
    MemorySize boneStart;
    Unsigned32 boneCount;
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

static void clearMesh(GeometryMesh *mesh);

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
    Boolean anyBoneAssignments = BOOLEAN_FALSE;
    Boolean anyBoneWeights = BOOLEAN_FALSE;
    Unsigned32 weightsStoredPerVertex = 0U;
    Real32 *positions;
    Real32 *normals = NULL_POINTER;
    Real32 *textures = NULL_POINTER;
    Unsigned8 *boneAssignments = NULL_POINTER;
    Real32 *boneWeights = NULL_POINTER;
    Unsigned16 *indices;

    clearMesh(mesh);

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
        components[index].boneAssignment = NULL_POINTER;
        components[index].boneWeight = NULL_POINTER;
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
            /* The format of an assignment word is not checked. It carries four
               byte indices whatever the file calls that layout, and the length
               of the payload is the check that matters — one word per vertex,
               which copyBoneAssignments verifies before it reads a byte. */
            else if (spans[which].identifier == (Unsigned32)GEOMETRY_ELEMENT_BONE_ASSIGNMENT)
            {
                components[index].boneAssignment = &spans[which];
                anyBoneAssignments = BOOLEAN_TRUE;
            }
            else if (spans[which].identifier == (Unsigned32)GEOMETRY_ELEMENT_BONE_WEIGHT &&
                     spans[which].format <= ELEMENT_FORMAT_THREE_FLOATS)
            {
                components[index].boneWeight = &spans[which];
                /* One float at format zero, two at one, three at two. Kept from
                   the first component that has any: a model whose parts disagree
                   would need an array per part, and none has been met that
                   does. */
                if (!anyBoneWeights)
                {
                    weightsStoredPerVertex = (Unsigned32)spans[which].format + 1U;
                }
                anyBoneWeights = BOOLEAN_TRUE;
            }
            else
            {
                rememberUnusedElement(mesh, spans[which].identifier,
                                      (Unsigned32)spans[which].format);
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
        primitiveSpans[storedPrimitives].boneCount = 0U;
        primitiveSpans[storedPrimitives].boneStart = 0UL;
        if (blockVersion > 1UL)
        {
            /* The bones this primitive's vertex slots stand for. Noted rather
               than skipped now: the slots in a vertex assignment are indices
               into this, so without it they name nothing. */
            primitiveSpans[storedPrimitives].boneCount =
                skipIndexArray(&cursor, blockVersion, &primitiveSpans[storedPrimitives].boneStart);
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
        primitives[storedPrimitives].boneRemap = NULL_POINTER;
        primitives[storedPrimitives].boneRemapCount = 0U;
        primitives[storedPrimitives].firstVertex = components[componentIndex].baseVertex;
        primitives[storedPrimitives].vertexCount = components[componentIndex].vertexCount;
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

    /* The bind pose, which begins where the last primitive record ended. The
     * first pass consumed every record whole — including the two fields after
     * the faces that an earlier version of it stopped short of — so the cursor
     * is already here and this needs no seek.
     *
     * Nothing here refuses the mesh. A bind pose that will not read costs the
     * ability to pose the model and nothing else, and a model that draws is
     * worth more than one that is thrown away for a section no one has needed
     * until now. */
    if (anyBoneAssignments)
    {
        Unsigned32 poseCount = resourceCursorReadUnsigned32(&cursor);

        /* A quaternion and a translation each: twenty-eight bytes at the very
         * least, so a count past that is not a count. The same shape of guard
         * as the element and primitive counts above, and for the same reason —
         * one of those refused 238 of 239 readable containers on a real disc. */
        if (!cursor.overran && poseCount > 0U && (MemorySize)poseCount <= sizeInBytes / 28UL)
        {
            GeometryBindPose *poses = (GeometryBindPose *)memoryArenaAllocate(
                arena, (MemorySize)poseCount * sizeof(GeometryBindPose), sizeof(Real32));

            if (poses == NULL_POINTER)
            {
                return GEOMETRY_READ_OUT_OF_ARENA;
            }
            for (index = 0U; index < poseCount; index++)
            {
                Unsigned32 axis;

                for (axis = 0U; axis < 4U; axis++)
                {
                    poses[index].rotation[axis] = resourceCursorReadReal32(&cursor);
                }
                for (axis = 0U; axis < 3U; axis++)
                {
                    poses[index].translation[axis] = resourceCursorReadReal32(&cursor);
                }
            }
            if (!cursor.overran)
            {
                mesh->bindPoses = poses;
                mesh->bindPoseCount = poseCount;

                /* The deformation channels, which begin the moment the bind
                 * pose ends. Only attempted when that read came back clean: the
                 * array has no header to seek to and no marker to recognise, so
                 * a cursor that has lost its place would read whatever happened
                 * to be there and report it as a name.
                 *
                 * Names only. What actually moves a vertex is the morph
                 * elements, which are still passed over and reported as unused
                 * — a Sim's body carries two of them. Reading what the channels
                 * are called is the half that says which sliders the disc has,
                 * and it costs a walk of a few dozen short strings. */
                {
                    Unsigned32 targetCount = resourceCursorReadUnsigned32(&cursor);

                    /* Two length-prefixed strings each, so two bytes at the very
                       least even when both are empty. The same shape of guard as
                       the counts above, for the same reason. */
                    if (!cursor.overran && targetCount > 0U &&
                        (MemorySize)targetCount <= sizeInBytes / 2UL)
                    {
                        GeometryMorphTarget *targets = (GeometryMorphTarget *)memoryArenaAllocate(
                            arena, (MemorySize)targetCount * sizeof(GeometryMorphTarget), 1UL);

                        if (targets == NULL_POINTER)
                        {
                            return GEOMETRY_READ_OUT_OF_ARENA;
                        }
                        for (index = 0U; index < targetCount; index++)
                        {
                            resourceCursorReadString(&cursor, targets[index].groupName,
                                                     GEOMETRY_NAME_LIMIT);
                            resourceCursorReadString(&cursor, targets[index].channelName,
                                                     GEOMETRY_NAME_LIMIT);
                        }
                        if (!cursor.overran)
                        {
                            mesh->morphTargets = targets;
                            mesh->morphTargetCount = targetCount;
                        }
                    }
                }
            }
        }
        /* The passes below seek before they read, but each clears this for
           itself only after seeking; leaving it set here would make the first
           of them look as though it had overrun. */
        cursor.overran = BOOLEAN_FALSE;
    }

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
    /* Both arrays or neither. Weights without assignments name no bone and
       assignments without weights weigh nothing, so a mesh carrying one of them
       is treated as carrying no skinning rather than half of it. */
    if (anyBoneAssignments != anyBoneWeights)
    {
        /* One half of the pair and not the other. Nothing is kept, and what was
           there is reported as met and not used — because it was. Letting it go
           by unmentioned would leave a mesh that plainly carries bone data
           looking, in the log, exactly like one that carries none. */
        for (index = 0U; index < elementCount; index++)
        {
            if (spans[index].identifier == (Unsigned32)GEOMETRY_ELEMENT_BONE_ASSIGNMENT ||
                spans[index].identifier == (Unsigned32)GEOMETRY_ELEMENT_BONE_WEIGHT)
            {
                rememberUnusedElement(mesh, spans[index].identifier,
                                      (Unsigned32)spans[index].format);
            }
        }
    }
    if (anyBoneAssignments && anyBoneWeights)
    {
        boneAssignments =
            (Unsigned8 *)memoryArenaAllocate(arena, (MemorySize)vertexCount * 4UL, 4UL);
        boneWeights = (Real32 *)memoryArenaAllocate(
            arena, (MemorySize)vertexCount * 4UL * sizeof(Real32), sizeof(Real32));
        if (boneAssignments == NULL_POINTER || boneWeights == NULL_POINTER)
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

        if (boneAssignments != NULL_POINTER)
        {
            MemorySize slot = (MemorySize)component->baseVertex * 4UL;

            /* A component of a skinned model that carries no bones of its own is
               left unassigned rather than pinned to bone zero, which would drag
               that part along with whatever joint happened to be first. */
            if (!copyBoneAssignments(&boneAssignments[slot], &cursor, component->boneAssignment,
                                     component->vertexCount) ||
                !copyBoneWeights(&boneWeights[slot], &cursor, component->boneWeight,
                                 weightsStoredPerVertex, component->vertexCount))
            {
                for (inner = 0U; inner < component->vertexCount * 4U; inner++)
                {
                    boneAssignments[slot + inner] = (Unsigned8)GEOMETRY_BONE_NONE;
                    boneWeights[slot + inner] = 0.0f;
                }
            }
        }
    }

    indices = (Unsigned16 *)memoryArenaAllocate(arena, (MemorySize)indexTotal * sizeof(Unsigned16),
                                                sizeof(Unsigned16));
    if (indices == NULL_POINTER)
    {
        return GEOMETRY_READ_OUT_OF_ARENA;
    }

    /* The bone lists, one array shared out between the primitives that have
       one. Only when the mesh is skinned: on a rigid model these name bones
       nothing is weighted to, and copying them would cost a walk of the whole
       disc's static objects to hold numbers no one reads. */
    if (boneAssignments != NULL_POINTER)
    {
        Unsigned32 boneTotal = 0U;

        for (index = 0U; index < storedPrimitives; index++)
        {
            boneTotal += primitiveSpans[index].boneCount;
        }
        if (boneTotal > 0U)
        {
            Unsigned32 *bones = (Unsigned32 *)memoryArenaAllocate(
                arena, (MemorySize)boneTotal * sizeof(Unsigned32), sizeof(Unsigned32));
            Unsigned32 written = 0U;

            if (bones == NULL_POINTER)
            {
                return GEOMETRY_READ_OUT_OF_ARENA;
            }
            for (index = 0U; index < storedPrimitives; index++)
            {
                Unsigned32 inner;

                if (primitiveSpans[index].boneCount == 0U)
                {
                    continue;
                }
                cursor.position = primitiveSpans[index].boneStart;
                cursor.overran = BOOLEAN_FALSE;
                primitives[index].boneRemap = &bones[written];
                for (inner = 0U; inner < primitiveSpans[index].boneCount; inner++)
                {
                    bones[written + inner] = (blockVersion < 3UL)
                                                 ? resourceCursorReadUnsigned32(&cursor)
                                                 : (Unsigned32)resourceCursorReadUnsigned16(&cursor);
                }
                if (cursor.overran)
                {
                    /* Kept readable rather than refused: a mesh whose bone list
                       ran off the end still draws, it just cannot be skinned,
                       and losing the model over it would be the wrong trade. */
                    primitives[index].boneRemap = NULL_POINTER;
                    continue;
                }
                primitives[index].boneRemapCount = primitiveSpans[index].boneCount;
                written += primitiveSpans[index].boneCount;
            }
        }
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
    mesh->boneAssignments = boneAssignments;
    mesh->boneWeights = boneWeights;
    mesh->weightsStoredPerVertex = (boneAssignments != NULL_POINTER) ? weightsStoredPerVertex : 0U;
    if (boneAssignments != NULL_POINTER)
    {
        /* Counted rather than assumed from the arrays existing. A component
           whose bones would not read is filled with the unassigned value, and
           a mesh where that happened to every component has the arrays and no
           skinning — which reads identically until this number is looked at. */
        for (index = 0U; index < vertexCount; index++)
        {
            if (boneAssignments[index * 4U] != (Unsigned8)GEOMETRY_BONE_NONE)
            {
                mesh->skinnedVertexCount++;
            }
        }
    }
    mesh->vertexCount = vertexCount;
    mesh->indices = indices;
    mesh->indexCount = indexTotal;
    mesh->primitives = primitives;
    mesh->storedPrimitiveCount = storedPrimitives;
    return GEOMETRY_READ_OK;
}

/* Every field to its empty value, so a mesh that fails to read is not left
   holding whatever the caller's storage had in it. Shared by the reader and the
   merge: a field added to GeometryMesh and cleared in only one of them would be
   uninitialised down whichever path was forgotten. */
static void clearMesh(GeometryMesh *mesh)
{
    mesh->name[0] = '\0';
    mesh->resourceName[0] = '\0';
    mesh->positions = NULL_POINTER;
    mesh->normals = NULL_POINTER;
    mesh->textureCoordinates = NULL_POINTER;
    mesh->boneAssignments = NULL_POINTER;
    mesh->boneWeights = NULL_POINTER;
    mesh->weightsStoredPerVertex = 0U;
    mesh->skinnedVertexCount = 0U;
    mesh->bindPoses = NULL_POINTER;
    mesh->bindPoseCount = 0U;
    mesh->morphTargets = NULL_POINTER;
    mesh->morphTargetCount = 0U;
    mesh->vertexCount = 0U;
    mesh->indices = NULL_POINTER;
    mesh->indexCount = 0U;
    mesh->primitives = NULL_POINTER;
    mesh->primitiveCount = 0U;
    mesh->storedPrimitiveCount = 0U;
    mesh->componentCount = 0U;
    mesh->unusedElementCount = 0U;
    mesh->versionMark = 0U;
    mesh->containerVersion = 0U;
    mesh->elementCount = 0U;
}

GeometryReadResult geometryMeshMerge(GeometryMesh *merged, const GeometryMesh *const *sources,
                                     Unsigned32 sourceCount, MemoryArena *arena)
{
    Unsigned32 vertexTotal = 0U;
    Unsigned32 indexTotal = 0U;
    Unsigned32 primitiveTotal = 0U;
    Boolean anyNormals = BOOLEAN_FALSE;
    Boolean anyTextures = BOOLEAN_FALSE;
    Boolean anySkinning = BOOLEAN_FALSE;
    Unsigned32 weightsStored = 0U;
    Unsigned32 vertexBase = 0U;
    Unsigned32 indexBase = 0U;
    Unsigned32 primitiveBase = 0U;
    Unsigned32 componentBase = 0U;
    Unsigned32 which;
    Real32 *positions;
    Real32 *normals = NULL_POINTER;
    Real32 *textures = NULL_POINTER;
    Unsigned8 *boneAssignments = NULL_POINTER;
    Real32 *boneWeights = NULL_POINTER;
    Unsigned16 *indices;
    GeometryPrimitive *primitives;

    clearMesh(merged);
    if (sources == NULL_POINTER || sourceCount == 0U)
    {
        return GEOMETRY_READ_NO_GEOMETRY;
    }

    for (which = 0U; which < sourceCount; which++)
    {
        const GeometryMesh *source = sources[which];

        if (source == NULL_POINTER || source->positions == NULL_POINTER ||
            source->vertexCount == 0U)
        {
            continue;
        }
        vertexTotal += source->vertexCount;
        indexTotal += source->indexCount;
        primitiveTotal += source->storedPrimitiveCount;
        anyNormals = (source->normals != NULL_POINTER) ? BOOLEAN_TRUE : anyNormals;
        anyTextures = (source->textureCoordinates != NULL_POINTER) ? BOOLEAN_TRUE : anyTextures;
        if (source->boneAssignments != NULL_POINTER)
        {
            anySkinning = BOOLEAN_TRUE;
            if (source->weightsStoredPerVertex > weightsStored)
            {
                weightsStored = source->weightsStoredPerVertex;
            }
        }
        if (source->bindPoseCount > merged->bindPoseCount)
        {
            merged->bindPoses = source->bindPoses;
            merged->bindPoseCount = source->bindPoseCount;
        }
        /* Morph targets are deliberately not carried across. A bind pose is one
         * thing every part shares — they are all weighted to the same skeleton,
         * so the longest one covers the rest. A deformation channel is not:
         * each part declares its own, numbered from its own array, and the
         * morph elements that reference those numbers are per container too.
         * Taking one part's list for the whole would be the component-index
         * mistake again, where an index that meant something inside one
         * container was read as though it meant the same after the join.
         *
         * A caller wanting them asks the parts, which still hold theirs. */
    }

    if (vertexTotal == 0U || indexTotal == 0U || primitiveTotal == 0U)
    {
        return GEOMETRY_READ_NO_GEOMETRY;
    }
    /* The indices are half words, so the join has a ceiling the parts did not
       individually. Refused rather than wrapped: an index that wraps points at
       another part's vertex and draws a triangle across the model. */
    if (vertexTotal > 0xFFFFU)
    {
        return GEOMETRY_READ_TOO_MANY_VERTICES;
    }

    positions = (Real32 *)memoryArenaAllocate(arena, (MemorySize)vertexTotal * 3UL * sizeof(Real32),
                                              sizeof(Real32));
    indices = (Unsigned16 *)memoryArenaAllocate(arena, (MemorySize)indexTotal * sizeof(Unsigned16),
                                                sizeof(Unsigned16));
    primitives = (GeometryPrimitive *)memoryArenaAllocate(
        arena, (MemorySize)primitiveTotal * sizeof(GeometryPrimitive), sizeof(MemorySize));
    if (positions == NULL_POINTER || indices == NULL_POINTER || primitives == NULL_POINTER)
    {
        return GEOMETRY_READ_OUT_OF_ARENA;
    }
    if (anyNormals)
    {
        normals = (Real32 *)memoryArenaAllocate(arena, (MemorySize)vertexTotal * 3UL * sizeof(Real32),
                                                sizeof(Real32));
        if (normals == NULL_POINTER)
        {
            return GEOMETRY_READ_OUT_OF_ARENA;
        }
    }
    if (anyTextures)
    {
        textures = (Real32 *)memoryArenaAllocate(arena, (MemorySize)vertexTotal * 2UL * sizeof(Real32),
                                                 sizeof(Real32));
        if (textures == NULL_POINTER)
        {
            return GEOMETRY_READ_OUT_OF_ARENA;
        }
    }
    if (anySkinning)
    {
        boneAssignments = (Unsigned8 *)memoryArenaAllocate(
            arena, (MemorySize)vertexTotal * 4UL * sizeof(Unsigned8), sizeof(Unsigned8));
        boneWeights = (Real32 *)memoryArenaAllocate(
            arena, (MemorySize)vertexTotal * 4UL * sizeof(Real32), sizeof(Real32));
        if (boneAssignments == NULL_POINTER || boneWeights == NULL_POINTER)
        {
            return GEOMETRY_READ_OUT_OF_ARENA;
        }
    }

    for (which = 0U; which < sourceCount; which++)
    {
        const GeometryMesh *source = sources[which];
        Unsigned32 index;

        if (source == NULL_POINTER || source->positions == NULL_POINTER ||
            source->vertexCount == 0U)
        {
            continue;
        }

        for (index = 0U; index < source->vertexCount; index++)
        {
            Unsigned32 axis;

            for (axis = 0U; axis < 3U; axis++)
            {
                positions[(vertexBase + index) * 3U + axis] = source->positions[index * 3U + axis];
                if (normals != NULL_POINTER)
                {
                    normals[(vertexBase + index) * 3U + axis] =
                        (source->normals != NULL_POINTER) ? source->normals[index * 3U + axis] : 0.0f;
                }
            }
            if (textures != NULL_POINTER)
            {
                textures[(vertexBase + index) * 2U] =
                    (source->textureCoordinates != NULL_POINTER)
                        ? source->textureCoordinates[index * 2U]
                        : 0.0f;
                textures[(vertexBase + index) * 2U + 1U] =
                    (source->textureCoordinates != NULL_POINTER)
                        ? source->textureCoordinates[index * 2U + 1U]
                        : 0.0f;
            }
            if (boneAssignments != NULL_POINTER)
            {
                for (axis = 0U; axis < 4U; axis++)
                {
                    /* A part with no weights of its own is left unassigned
                       rather than weighted to bone nought, which would drag it
                       to wherever that bone went. */
                    boneAssignments[(vertexBase + index) * 4U + axis] =
                        (source->boneAssignments != NULL_POINTER)
                            ? source->boneAssignments[index * 4U + axis]
                            : (Unsigned8)GEOMETRY_BONE_NONE;
                    boneWeights[(vertexBase + index) * 4U + axis] =
                        (source->boneWeights != NULL_POINTER)
                            ? source->boneWeights[index * 4U + axis]
                            : 0.0f;
                }
            }
        }

        for (index = 0U; index < source->indexCount; index++)
        {
            indices[indexBase + index] = (Unsigned16)(source->indices[index] + vertexBase);
        }

        for (index = 0U; index < source->storedPrimitiveCount; index++)
        {
            primitives[primitiveBase + index] = source->primitives[index];
            primitives[primitiveBase + index].firstIndex += indexBase;
            primitives[primitiveBase + index].firstVertex += vertexBase;
            /* Shifted like everything else, and for a sharper reason than
               tidiness. A component index means something only inside its own
               container, so every source's first primitive draws from component
               nought. Left alone, two parts that share nothing look to
               geometryMeshApplySkin like two primitives over one component —
               and it skips the second, because transforming shared vertices
               twice would fold a part in on itself. That is what left a Sim's
               head behind while its body lay down. */
            primitives[primitiveBase + index].componentIndex += componentBase;
        }

        vertexBase += source->vertexCount;
        indexBase += source->indexCount;
        primitiveBase += source->storedPrimitiveCount;
        componentBase += (source->componentCount > 0U) ? source->componentCount : 1U;
    }

    stringAppend(merged->name, GEOMETRY_NAME_LIMIT, primitives[0].name);
    merged->positions = positions;
    merged->normals = normals;
    merged->textureCoordinates = textures;
    merged->vertexCount = vertexTotal;
    merged->indices = indices;
    merged->indexCount = indexTotal;
    merged->primitives = primitives;
    merged->primitiveCount = primitiveBase;
    merged->storedPrimitiveCount = primitiveBase;
    merged->componentCount = componentBase;
    merged->boneAssignments = boneAssignments;
    merged->boneWeights = boneWeights;
    merged->weightsStoredPerVertex = weightsStored;
    if (boneAssignments != NULL_POINTER)
    {
        for (which = 0U; which < vertexTotal; which++)
        {
            if (boneAssignments[which * 4U] != (Unsigned8)GEOMETRY_BONE_NONE)
            {
                merged->skinnedVertexCount++;
            }
        }
    }
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

Unsigned32 geometryMeshApplySkin(GeometryMesh *mesh, const Real32 *boneMatrices,
                                 Unsigned32 boneCount)
{
    Unsigned32 which;
    Unsigned32 moved = 0U;

    if (mesh->positions == NULL_POINTER || mesh->boneAssignments == NULL_POINTER ||
        mesh->boneWeights == NULL_POINTER || boneMatrices == NULL_POINTER || boneCount == 0U)
    {
        return 0U;
    }

    for (which = 0U; which < mesh->storedPrimitiveCount; which++)
    {
        const GeometryPrimitive *primitive = &mesh->primitives[which];
        Boolean alreadyDone = BOOLEAN_FALSE;
        Unsigned32 earlier;
        Unsigned32 vertex;

        if (primitive->boneRemap == NULL_POINTER || primitive->boneRemapCount == 0U)
        {
            continue;
        }
        /* Two primitives can draw from one component, and their vertices are
           the same vertices. Transforming them once per primitive would apply
           the pose twice and fold the part in on itself. */
        for (earlier = 0U; earlier < which; earlier++)
        {
            if (mesh->primitives[earlier].componentIndex == primitive->componentIndex &&
                mesh->primitives[earlier].boneRemap != NULL_POINTER)
            {
                alreadyDone = BOOLEAN_TRUE;
                break;
            }
        }
        if (alreadyDone)
        {
            continue;
        }

        for (vertex = primitive->firstVertex;
             vertex < primitive->firstVertex + primitive->vertexCount &&
             vertex < mesh->vertexCount;
             vertex++)
        {
            const Unsigned8 *slots = &mesh->boneAssignments[vertex * 4U];
            const Real32 *weights = &mesh->boneWeights[vertex * 4U];
            Real32 blended[12];
            Real32 weightTotal = 0.0f;
            Unsigned32 slot;
            Unsigned32 cell;
            Real32 x;
            Real32 y;
            Real32 z;
            Real32 *point;

            for (cell = 0U; cell < 12U; cell++)
            {
                blended[cell] = 0.0f;
            }

            /* Only the rotation and the translation: the fourth column of an
               affine matrix is the same for all of them, so blending it would
               be adding up ones. */
            for (slot = 0U; slot < 4U; slot++)
            {
                const Real32 *matrix;
                Unsigned32 bone;

                if (slots[slot] == (Unsigned8)GEOMETRY_BONE_NONE || weights[slot] == 0.0f)
                {
                    continue;
                }
                if (slots[slot] >= primitive->boneRemapCount)
                {
                    continue;
                }
                bone = primitive->boneRemap[slots[slot]];
                if (bone >= boneCount)
                {
                    continue;
                }
                matrix = &boneMatrices[bone * 16U];
                for (cell = 0U; cell < 3U; cell++)
                {
                    blended[cell] += matrix[cell] * weights[slot];
                    blended[3U + cell] += matrix[4U + cell] * weights[slot];
                    blended[6U + cell] += matrix[8U + cell] * weights[slot];
                    blended[9U + cell] += matrix[12U + cell] * weights[slot];
                }
                weightTotal += weights[slot];
            }

            /* A vertex whose bones were all out of range keeps the position it
               was read with. Left where it is rather than collapsed to the
               origin, which is what multiplying by an all-zero blend would do
               and what makes a half-skinned mesh look like a black hole. */
            if (weightTotal <= 0.0f)
            {
                continue;
            }

            point = &mesh->positions[vertex * 3U];
            x = point[0];
            y = point[1];
            z = point[2];
            point[0] = blended[0] * x + blended[3] * y + blended[6] * z + blended[9];
            point[1] = blended[1] * x + blended[4] * y + blended[7] * z + blended[10];
            point[2] = blended[2] * x + blended[5] * y + blended[8] * z + blended[11];

            if (mesh->normals != NULL_POINTER)
            {
                Real32 *direction = &mesh->normals[vertex * 3U];

                x = direction[0];
                y = direction[1];
                z = direction[2];
                direction[0] = blended[0] * x + blended[3] * y + blended[6] * z;
                direction[1] = blended[1] * x + blended[4] * y + blended[7] * z;
                direction[2] = blended[2] * x + blended[5] * y + blended[8] * z;
            }
            moved++;
        }
    }
    return moved;
}

void geometryMeshApplyTransform(GeometryMesh *mesh, const Real32 *matrix)
{
    Unsigned32 vertex;

    if (mesh->positions == NULL_POINTER)
    {
        return;
    }
    for (vertex = 0U; vertex < mesh->vertexCount; vertex++)
    {
        Real32 *point = &mesh->positions[vertex * 3U];
        Real32 x = point[0];
        Real32 y = point[1];
        Real32 z = point[2];

        point[0] = matrix[0] * x + matrix[4] * y + matrix[8] * z + matrix[12];
        point[1] = matrix[1] * x + matrix[5] * y + matrix[9] * z + matrix[13];
        point[2] = matrix[2] * x + matrix[6] * y + matrix[10] * z + matrix[14];

        if (mesh->normals != NULL_POINTER)
        {
            Real32 *direction = &mesh->normals[vertex * 3U];

            x = direction[0];
            y = direction[1];
            z = direction[2];
            direction[0] = matrix[0] * x + matrix[4] * y + matrix[8] * z;
            direction[1] = matrix[1] * x + matrix[5] * y + matrix[9] * z;
            direction[2] = matrix[2] * x + matrix[6] * y + matrix[10] * z;
        }
    }
}
