#include "victoria/geometryReader.h"

#include "utils/strings.h"

#define SCENEGRAPH_VERSION_MARK 0xFFFF0001UL
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

/* Reads a buffer without ever indexing past its end. Every read checks, and a
 * cursor that has overrun stays overrun, so a caller can do a run of reads and
 * test once at the end rather than after each one. */
typedef struct Cursor
{
    const Unsigned8 *bytes;
    MemorySize sizeInBytes;
    MemorySize position;
    Boolean overran;
} Cursor;

static void cursorInitialize(Cursor *cursor, const Unsigned8 *bytes, MemorySize sizeInBytes)
{
    cursor->bytes = bytes;
    cursor->sizeInBytes = sizeInBytes;
    cursor->position = 0UL;
    cursor->overran = BOOLEAN_FALSE;
}

static Boolean cursorTake(Cursor *cursor, MemorySize count)
{
    if (cursor->overran || count > cursor->sizeInBytes - cursor->position)
    {
        cursor->overran = BOOLEAN_TRUE;
        return BOOLEAN_FALSE;
    }
    cursor->position += count;
    return BOOLEAN_TRUE;
}

static Unsigned8 readUnsigned8(Cursor *cursor)
{
    MemorySize at = cursor->position;

    if (!cursorTake(cursor, 1UL))
    {
        return 0U;
    }
    return cursor->bytes[at];
}

static Unsigned16 readUnsigned16(Cursor *cursor)
{
    MemorySize at = cursor->position;

    if (!cursorTake(cursor, 2UL))
    {
        return 0U;
    }
    return (Unsigned16)((Unsigned16)cursor->bytes[at] | ((Unsigned16)cursor->bytes[at + 1UL] << 8));
}

static Unsigned32 readUnsigned32(Cursor *cursor)
{
    MemorySize at = cursor->position;

    if (!cursorTake(cursor, 4UL))
    {
        return 0U;
    }
    return (Unsigned32)cursor->bytes[at] | ((Unsigned32)cursor->bytes[at + 1UL] << 8) |
           ((Unsigned32)cursor->bytes[at + 2UL] << 16) | ((Unsigned32)cursor->bytes[at + 3UL] << 24);
}

/* Assembled from bytes rather than cast, because the payload is not aligned and
 * an unaligned load is a fault on ARMv5 rather than a slow path. */
static Real32 readReal32(Cursor *cursor)
{
    union
    {
        Unsigned32 word;
        Real32 value;
    } converter;

    converter.word = readUnsigned32(cursor);
    return converter.value;
}

/* Strings carry a length prefixed seven bits at a time, high bit set while more
 * follows. Copies what fits and skips the rest, so a long name costs a truncated
 * label rather than the whole mesh. */
static void readString(Cursor *cursor, char *destination, MemorySize capacity)
{
    MemorySize length = 0UL;
    MemorySize shift = 0UL;
    MemorySize index;

    for (;;)
    {
        Unsigned8 byte = readUnsigned8(cursor);

        length |= (MemorySize)(byte & 0x7FU) << shift;
        if ((byte & 0x80U) == 0U || shift >= 28UL || cursor->overran)
        {
            break;
        }
        shift += 7UL;
    }

    for (index = 0UL; index < length; index++)
    {
        Unsigned8 character = readUnsigned8(cursor);

        if (destination != NULL_POINTER && index + 1UL < capacity)
        {
            destination[index] = (char)character;
        }
    }
    if (destination != NULL_POINTER && capacity > 0UL)
    {
        destination[(length + 1UL < capacity) ? length : capacity - 1UL] = '\0';
    }
}

/* How wide one index is at a given block version. */
static MemorySize indexWidth(Unsigned32 version)
{
    return (version < 3UL) ? 4UL : 2UL;
}

/* An index array: a count, then that many words or half words. Returns where
 * the array starts so a caller can come back for it, having skipped it here. */
static Unsigned32 skipIndexArray(Cursor *cursor, Unsigned32 version, MemorySize *startPosition)
{
    Unsigned32 count = readUnsigned32(cursor);

    if (startPosition != NULL_POINTER)
    {
        *startPosition = cursor->position;
    }
    cursorTake(cursor, (MemorySize)count * indexWidth(version));
    return count;
}

/* The type name, identifier and version that prefix every scenegraph block. */
static Unsigned32 readTypeInformation(Cursor *cursor, Unsigned32 *typeIdentifier, char *name,
                                      MemorySize nameCapacity)
{
    Unsigned32 identifier;
    Unsigned32 version;

    readString(cursor, name, nameCapacity);
    identifier = readUnsigned32(cursor);
    version = readUnsigned32(cursor);
    if (typeIdentifier != NULL_POINTER)
    {
        *typeIdentifier = identifier;
    }
    return version;
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
        return "more geometry elements than this reader holds";
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

#define MAXIMUM_ELEMENTS 32U

static Real32 *copyRealArray(MemoryArena *arena, Cursor *cursor, const ElementSpan *span,
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
        values[index] = readReal32(cursor);
    }
    return cursor->overran ? NULL_POINTER : values;
}

GeometryReadResult geometryReaderOpen(GeometryMesh *mesh, const Unsigned8 *bytes, MemorySize sizeInBytes,
                                      MemoryArena *arena)
{
    Cursor cursor;
    ElementSpan spans[MAXIMUM_ELEMENTS];
    const ElementSpan *positionSpan = NULL_POINTER;
    const ElementSpan *normalSpan = NULL_POINTER;
    const ElementSpan *textureSpan = NULL_POINTER;
    Unsigned32 versionMark;
    Unsigned32 fileLinkCount;
    Unsigned32 blockCount;
    Unsigned32 blockType = 0U;
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

    cursorInitialize(&cursor, bytes, sizeInBytes);

    versionMark = readUnsigned32(&cursor);
    if (cursor.overran)
    {
        return GEOMETRY_READ_TRUNCATED;
    }
    mesh->versionMark = versionMark;
    if (versionMark != (Unsigned32)SCENEGRAPH_VERSION_MARK)
    {
        /* Older collections mark themselves 0xFFFE0001 or 0xFFFD0001 and differ
         * in how the file links are laid out. Saying so separately keeps them
         * from being counted as rubbish. */
        if ((versionMark & 0x0000FFFFUL) == 0x00000001UL &&
            (versionMark >> 16) >= 0xFFF0UL)
        {
            return GEOMETRY_READ_OLDER_COLLECTION;
        }
        return GEOMETRY_READ_NOT_A_RESOURCE;
    }

    /* Links to resources in other packages. Following them is the scenegraph's
     * job, not this reader's. */
    fileLinkCount = readUnsigned32(&cursor);
    cursorTake(&cursor, (MemorySize)fileLinkCount * 16UL);

    blockCount = readUnsigned32(&cursor);
    if (blockCount == 0U)
    {
        return GEOMETRY_READ_NO_GEOMETRY;
    }
    /* The type list, then the blocks themselves. Only the first is read: a GMDC
     * resource carries exactly one. */
    cursorTake(&cursor, (MemorySize)blockCount * 4UL);

    blockVersion = readTypeInformation(&cursor, &blockType, NULL_POINTER, 0UL);
    if (cursor.overran)
    {
        return GEOMETRY_READ_TRUNCATED;
    }
    if (blockType != (Unsigned32)GEOMETRY_TYPE_IDENTIFIER)
    {
        return GEOMETRY_READ_WRONG_TYPE;
    }
    mesh->containerVersion = blockVersion;
    if (blockVersion < MINIMUM_BLOCK_VERSION)
    {
        return GEOMETRY_READ_UNSUPPORTED_VERSION;
    }

    /* The resource's own name, behind another type prefix. */
    readTypeInformation(&cursor, NULL_POINTER, NULL_POINTER, 0UL);
    readString(&cursor, mesh->resourceName, GEOMETRY_NAME_LIMIT);

    elementCount = readUnsigned32(&cursor);
    /* Checked before the counts are believed. A resource that stops here has a
     * zero element count only because the read failed, and reporting that as
     * "holds no geometry" would blame the file for our own short buffer. */
    if (cursor.overran)
    {
        return GEOMETRY_READ_TRUNCATED;
    }
    if (elementCount > MAXIMUM_ELEMENTS)
    {
        return GEOMETRY_READ_TOO_MANY_ELEMENTS;
    }
    for (index = 0U; index < elementCount; index++)
    {
        Unsigned32 payloadBytes;

        (void)readUnsigned32(&cursor);
        spans[index].identifier = readUnsigned32(&cursor);
        (void)readUnsigned32(&cursor);
        spans[index].format = readUnsigned32(&cursor);
        (void)readUnsigned32(&cursor);
        payloadBytes = readUnsigned32(&cursor);

        spans[index].payloadBytes = payloadBytes;
        spans[index].payloadPosition = cursor.position;
        cursorTake(&cursor, (MemorySize)payloadBytes);
        (void)skipIndexArray(&cursor, blockVersion, NULL_POINTER);

        if (cursor.overran)
        {
            return GEOMETRY_READ_TRUNCATED;
        }
    }

    /* Components tie a set of elements together and say how many vertices they
     * describe. The first is the one the first primitive draws from. */
    componentCount = readUnsigned32(&cursor);
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
        componentVertexCount = readUnsigned32(&cursor);
        (void)readUnsigned32(&cursor);
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
            Cursor elementCursor = cursor;
            Unsigned32 which;

            elementCursor.position = elementIndexStart + ((MemorySize)inner * indexWidth(blockVersion));
            elementCursor.overran = BOOLEAN_FALSE;
            which = (blockVersion < 3UL) ? readUnsigned32(&elementCursor)
                                         : (Unsigned32)readUnsigned16(&elementCursor);
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

    mesh->primitiveCount = readUnsigned32(&cursor);
    if (cursor.overran)
    {
        return GEOMETRY_READ_TRUNCATED;
    }
    if (mesh->primitiveCount == 0U)
    {
        return GEOMETRY_READ_NO_GEOMETRY;
    }

    /* Only the first primitive. Its faces index into the component's vertices. */
    (void)readUnsigned32(&cursor);
    (void)readUnsigned32(&cursor);
    readString(&cursor, mesh->name, GEOMETRY_NAME_LIMIT);
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
        Unsigned32 value = (blockVersion < 3UL) ? readUnsigned32(&cursor)
                                                : (Unsigned32)readUnsigned16(&cursor);

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
