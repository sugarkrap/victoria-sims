#include "victoria/resourceNode.h"

#include "utils/strings.h"

#define BLOCK_RESOURCE_NODE 0xE519C933UL
#define BLOCK_TRANSFORM_NODE 0x65246462UL
#define BLOCK_SHAPE_REFERENCE_NODE 0x65245517UL
#define BLOCK_LIGHT_REFERENCE_NODE 0x253D2018UL
#define BLOCK_DATA_LIST_EXTENSION 0x6A836D56UL
#define BLOCK_TAG_EXTENSION 0x9A809646UL
#define BLOCK_BONE_DATA_EXTENSION 0xE9075BC5UL

#define LARGEST_EXTENSION_DEPTH 16U

typedef struct NodeEdge
{
    Unsigned32 parentBlock;
    Unsigned32 childBlock;
} NodeEdge;

const char *resourceNodeResultGetName(ResourceNodeResult result)
{
    switch (result)
    {
    case RESOURCE_NODE_OK:
        return "ok";
    case RESOURCE_NODE_NOT_A_RESOURCE:
        return "not a scenegraph resource";
    case RESOURCE_NODE_OLDER_COLLECTION:
        return "an older collection, laid out differently";
    case RESOURCE_NODE_WRONG_TYPE:
        return "not a resource node";
    case RESOURCE_NODE_TRUNCATED:
        return "the resource ends part way through";
    case RESOURCE_NODE_UNKNOWN_BLOCK:
        return "a block type this reader cannot measure";
    default:
        return "unknown";
    }
}

static MemorySize remainingBytes(const ResourceCursor *cursor)
{
    return cursor->overran ? 0UL : cursor->sizeInBytes - cursor->position;
}

static Boolean countFits(ResourceCursor *cursor, Unsigned32 count)
{
    if (cursor->overran || (MemorySize)count > remainingBytes(cursor))
    {
        cursor->overran = BOOLEAN_TRUE;
        return BOOLEAN_FALSE;
    }
    return BOOLEAN_TRUE;
}

static void readShortString(ResourceCursor *cursor, char *destination, MemorySize capacity)
{
    Unsigned32 length = (Unsigned32)resourceCursorReadUnsigned8(cursor);
    Unsigned32 index;

    if (destination != NULL_POINTER && capacity > 0UL)
    {
        destination[0] = '\0';
    }
    for (index = 0U; index < length; index++)
    {
        Unsigned8 character = resourceCursorReadUnsigned8(cursor);

        if (destination != NULL_POINTER && (MemorySize)index + 1UL < capacity)
        {
            destination[index] = (char)character;
        }
        if (cursor->overran)
        {
            return;
        }
    }
    if (destination != NULL_POINTER && capacity > 0UL)
    {
        destination[((MemorySize)length + 1UL < capacity) ? length : capacity - 1UL] = '\0';
    }
}

static void readObjectGraphNode(ResourceCursor *cursor, char *tag, MemorySize tagCapacity)
{
    PersistTypeInfo typeInformation;
    Unsigned32 count;
    Unsigned32 index;

    if (tag != NULL_POINTER && tagCapacity > 0UL)
    {
        tag[0] = '\0';
    }
    resourceCursorReadTypeInformation(cursor, &typeInformation);
    count = resourceCursorReadUnsigned32(cursor);
    if (!countFits(cursor, count))
    {
        return;
    }
    for (index = 0U; index < count; index++)
    {
        (void)resourceCursorReadObjectReference(cursor);
        if (cursor->overran)
        {
            return;
        }
    }
    if (typeInformation.version >= 4U)
    {
        resourceCursorReadString(cursor, tag, tagCapacity);
    }
}

static void readCompositionTree(ResourceCursor *cursor, char *tag, MemorySize tagCapacity,
                                Unsigned32 ownBlockIndex, NodeEdge *edges, Unsigned32 *edgeCount)
{
    Unsigned32 count;
    Unsigned32 index;

    resourceCursorReadTypeInformation(cursor, NULL_POINTER);
    readObjectGraphNode(cursor, tag, tagCapacity);

    count = resourceCursorReadUnsigned32(cursor);
    if (!countFits(cursor, count))
    {
        return;
    }
    for (index = 0U; index < count; index++)
    {
        ObjectReference reference = resourceCursorReadObjectReference(cursor);

        if (cursor->overran)
        {
            return;
        }
        if (reference.kind == OBJECT_REFERENCE_INTERNAL && reference.index >= 0 &&
            *edgeCount < RESOURCE_NODE_EDGE_LIMIT)
        {
            edges[*edgeCount].parentBlock = ownBlockIndex;
            edges[*edgeCount].childBlock = (Unsigned32)reference.index;
            (*edgeCount)++;
        }
    }
}

static void readTransformBody(ResourceCursor *cursor, TransformNode *node, Unsigned32 ownBlockIndex,
                              NodeEdge *edges, Unsigned32 *edgeCount)
{
    Unsigned32 axis;

    readCompositionTree(cursor, node != NULL_POINTER ? node->name : NULL_POINTER,
                        node != NULL_POINTER ? RESOURCE_NAME_LIMIT : 0UL, ownBlockIndex, edges,
                        edgeCount);
    for (axis = 0U; axis < 3U; axis++)
    {
        Real32 value = resourceCursorReadReal32(cursor);

        if (node != NULL_POINTER)
        {
            node->translation[axis] = value;
        }
    }
    for (axis = 0U; axis < 4U; axis++)
    {
        Real32 value = resourceCursorReadReal32(cursor);

        if (node != NULL_POINTER)
        {
            node->rotation[axis] = value;
        }
    }
    {
        Unsigned32 bone = resourceCursorReadUnsigned32(cursor);

        if (node != NULL_POINTER)
        {
            node->boneIdentifier = bone;
        }
    }
}

static void readBoundedNode(ResourceCursor *cursor, TransformNode *node, Unsigned32 ownBlockIndex,
                            NodeEdge *edges, Unsigned32 *edgeCount)
{
    resourceCursorReadTypeInformation(cursor, NULL_POINTER);
    resourceCursorReadTypeInformation(cursor, NULL_POINTER);
    readTransformBody(cursor, node, ownBlockIndex, edges, edgeCount);
    (void)resourceCursorReadUnsigned8(cursor);
}

static void readRenderableNode(ResourceCursor *cursor, TransformNode *node, Unsigned32 ownBlockIndex,
                               NodeEdge *edges, Unsigned32 *edgeCount)
{
    Unsigned32 count;
    Unsigned32 index;

    resourceCursorReadTypeInformation(cursor, NULL_POINTER);
    readBoundedNode(cursor, node, ownBlockIndex, edges, edgeCount);

    (void)resourceCursorReadUnsigned8(cursor);
    count = resourceCursorReadUnsigned32(cursor);
    if (!countFits(cursor, count))
    {
        return;
    }
    for (index = 0U; index < count; index++)
    {
        resourceCursorReadString(cursor, NULL_POINTER, 0UL);
    }
    (void)resourceCursorReadUnsigned32(cursor);
    (void)resourceCursorReadUnsigned8(cursor);
}

static void readExtensionValue(ResourceCursor *cursor, Unsigned32 depth)
{
    Unsigned8 dataType = resourceCursorReadUnsigned8(cursor);

    resourceCursorReadString(cursor, NULL_POINTER, 0UL);
    if (cursor->overran)
    {
        return;
    }

    switch (dataType)
    {
    case 1U:
        (void)resourceCursorReadUnsigned8(cursor);
        break;
    case 2U:
    case 3U:
        resourceCursorSkip(cursor, 4UL);
        break;
    case 4U:
        resourceCursorSkip(cursor, 8UL);
        break;
    case 5U:
        resourceCursorSkip(cursor, 12UL);
        break;
    case 6U:
        resourceCursorReadString(cursor, NULL_POINTER, 0UL);
        break;
    case 7U:
    {
        Unsigned32 count = resourceCursorReadUnsigned32(cursor);
        Unsigned32 index;

        if (depth >= LARGEST_EXTENSION_DEPTH || !countFits(cursor, count))
        {
            cursor->overran = BOOLEAN_TRUE;
            return;
        }
        for (index = 0U; index < count; index++)
        {
            readExtensionValue(cursor, depth + 1U);
            if (cursor->overran)
            {
                return;
            }
        }
        break;
    }
    case 8U:
        resourceCursorSkip(cursor, 16UL);
        break;
    case 9U:
    {
        Unsigned32 length = resourceCursorReadUnsigned32(cursor);

        if (!countFits(cursor, length))
        {
            return;
        }
        resourceCursorSkip(cursor, (MemorySize)length);
        break;
    }
    default:
        cursor->overran = BOOLEAN_TRUE;
        break;
    }
}

static Boolean readBlock(ResourceCursor *cursor, Unsigned32 blockType, Unsigned32 blockVersion,
                         Unsigned32 blockIndex, const ResourceCollection *collection,
                         TransformNode *node, Boolean *carriesTransform, NodeEdge *edges,
                         Unsigned32 *edgeCount, char *resourceName, MemorySize resourceNameCapacity)
{
    *carriesTransform = BOOLEAN_FALSE;

    switch (blockType)
    {
    case BLOCK_RESOURCE_NODE:
    {
        Unsigned8 hasTree = resourceCursorReadUnsigned8(cursor);

        if (hasTree != 0U)
        {
            resourceCursorReadTypeInformation(cursor, NULL_POINTER);
            resourceCursorReadString(cursor, resourceName, resourceNameCapacity);
            readCompositionTree(cursor, NULL_POINTER, 0UL, blockIndex, edges, edgeCount);
        }
        else
        {
            readObjectGraphNode(cursor, resourceName, resourceNameCapacity);
        }
        (void)resourceCursorReadObjectReference(cursor);
        (void)resourceCursorReadUnsigned32(cursor);
        return BOOLEAN_TRUE;
    }

    case BLOCK_TRANSFORM_NODE:
        readTransformBody(cursor, node, blockIndex, edges, edgeCount);
        *carriesTransform = BOOLEAN_TRUE;
        return BOOLEAN_TRUE;

    case BLOCK_SHAPE_REFERENCE_NODE:
    {
        Unsigned32 count;
        Unsigned32 index;
        Unsigned32 morphCount;
        Unsigned32 byteCount;

        readRenderableNode(cursor, node, blockIndex, edges, edgeCount);
        *carriesTransform = BOOLEAN_TRUE;

        count = resourceCursorReadUnsigned32(cursor);
        if (!countFits(cursor, count))
        {
            return BOOLEAN_TRUE;
        }
        for (index = 0U; index < count; index++)
        {
            ObjectReference reference = resourceCursorReadObjectReference(cursor);
            const PackageResourceKey *link = resourceCollectionGetLink(collection, reference);

            if (link != NULL_POINTER && node != NULL_POINTER && !node->hasShape)
            {
                node->shapeKey = *link;
                node->hasShape = BOOLEAN_TRUE;
            }
        }
        (void)resourceCursorReadUnsigned32(cursor);

        morphCount = resourceCursorReadUnsigned32(cursor);
        if (!countFits(cursor, morphCount))
        {
            return BOOLEAN_TRUE;
        }
        resourceCursorSkip(cursor, (MemorySize)morphCount * 4UL);
        if (blockVersion >= 21U)
        {
            for (index = 0U; index < morphCount; index++)
            {
                resourceCursorReadString(cursor, NULL_POINTER, 0UL);
            }
        }

        byteCount = resourceCursorReadUnsigned32(cursor);
        if (!countFits(cursor, byteCount))
        {
            return BOOLEAN_TRUE;
        }
        resourceCursorSkip(cursor, (MemorySize)byteCount);
        if (blockVersion > 19U)
        {
            (void)resourceCursorReadUnsigned32(cursor);
        }
        return BOOLEAN_TRUE;
    }

    case BLOCK_LIGHT_REFERENCE_NODE:
    {
        readRenderableNode(cursor, node, blockIndex, edges, edgeCount);
        *carriesTransform = BOOLEAN_TRUE;

        if (blockVersion < 10U)
        {
            if (resourceCursorReadUnsigned32(cursor) == 1U)
            {
                (void)resourceCursorReadObjectReference(cursor);
            }
            {
                Unsigned8 secondCount = resourceCursorReadUnsigned8(cursor);
                Unsigned8 hasSecond = resourceCursorReadUnsigned8(cursor);

                if (secondCount == 1U && hasSecond != 0U)
                {
                    (void)resourceCursorReadObjectReference(cursor);
                }
            }
        }
        else
        {
            (void)resourceCursorReadObjectReference(cursor);
            {
                Unsigned8 secondCount = resourceCursorReadUnsigned8(cursor);
                Unsigned8 missingPrecomputed = resourceCursorReadUnsigned8(cursor);

                if (secondCount == 1U && missingPrecomputed == 0U)
                {
                    (void)resourceCursorReadObjectReference(cursor);
                }
            }
        }
        return BOOLEAN_TRUE;
    }

    case BLOCK_DATA_LIST_EXTENSION:
        resourceCursorReadTypeInformation(cursor, NULL_POINTER);
        readExtensionValue(cursor, 0U);
        return BOOLEAN_TRUE;

    case BLOCK_TAG_EXTENSION:
        resourceCursorReadTypeInformation(cursor, NULL_POINTER);
        readShortString(cursor, NULL_POINTER, 0UL);
        return BOOLEAN_TRUE;

    case BLOCK_BONE_DATA_EXTENSION:
        resourceCursorReadTypeInformation(cursor, NULL_POINTER);
        resourceCursorSkip(cursor, 16UL);
        if (blockVersion > 3U)
        {
            resourceCursorSkip(cursor, 16UL);
        }
        return BOOLEAN_TRUE;

    default:
        return BOOLEAN_FALSE;
    }
}

ResourceNodeResult resourceNodeRead(ResourceNodeDescription *description, const Unsigned8 *bytes,
                                    MemorySize sizeInBytes)
{
    ResourceCollection collection;
    ResourceCursor cursor;
    ResourceCollectionResult collectionResult;
    NodeEdge edges[RESOURCE_NODE_EDGE_LIMIT];
    Unsigned32 edgeCount = 0U;
    Unsigned32 blockIndex;
    Unsigned32 index;

    description->resourceName[0] = '\0';
    description->blockVersion = 0U;
    description->blockCount = 0U;
    description->blocksRead = 0U;
    description->nodeCount = 0U;
    description->storedNodeCount = 0U;

    collectionResult = resourceCollectionOpen(&collection, &cursor, bytes, sizeInBytes);
    if (collectionResult != RESOURCE_COLLECTION_OK)
    {
        switch (collectionResult)
        {
        case RESOURCE_COLLECTION_OLDER:
            return RESOURCE_NODE_OLDER_COLLECTION;
        case RESOURCE_COLLECTION_TRUNCATED:
            return RESOURCE_NODE_TRUNCATED;
        default:
            return RESOURCE_NODE_NOT_A_RESOURCE;
        }
    }
    if (collection.firstBlockType != (Unsigned32)BLOCK_RESOURCE_NODE)
    {
        return RESOURCE_NODE_WRONG_TYPE;
    }
    description->blockCount = collection.blockCount;

    for (blockIndex = 0U; blockIndex < collection.blockCount; blockIndex++)
    {
        PersistTypeInfo blockType;
        TransformNode *node = NULL_POINTER;
        TransformNode scratch;
        Boolean carriesTransform = BOOLEAN_FALSE;

        resourceCursorReadTypeInformation(&cursor, &blockType);
        if (cursor.overran)
        {
            return RESOURCE_NODE_TRUNCATED;
        }
        if (blockIndex == 0U)
        {
            description->blockVersion = blockType.version;
        }

        node = (description->storedNodeCount < RESOURCE_NODE_LIMIT)
                   ? &description->nodes[description->storedNodeCount]
                   : &scratch;
        node->name[0] = '\0';
        node->blockIndex = blockIndex;
        node->translation[0] = 0.0f;
        node->translation[1] = 0.0f;
        node->translation[2] = 0.0f;
        node->rotation[0] = 0.0f;
        node->rotation[1] = 0.0f;
        node->rotation[2] = 0.0f;
        node->rotation[3] = 1.0f;
        node->boneIdentifier = 0U;
        node->parentIndex = -1;
        node->hasShape = BOOLEAN_FALSE;

        if (!readBlock(&cursor, blockType.typeIdentifier, blockType.version, blockIndex, &collection,
                       node, &carriesTransform, edges, &edgeCount,
                       (blockIndex == 0U) ? description->resourceName : NULL_POINTER,
                       (blockIndex == 0U) ? RESOURCE_NAME_LIMIT : 0UL))
        {
            return RESOURCE_NODE_UNKNOWN_BLOCK;
        }
        if (cursor.overran)
        {
            return RESOURCE_NODE_TRUNCATED;
        }
        description->blocksRead = blockIndex + 1U;

        if (carriesTransform)
        {
            description->nodeCount++;
            if (node != &scratch)
            {
                description->storedNodeCount++;
            }
        }
    }

    for (index = 0U; index < edgeCount; index++)
    {
        Unsigned32 parent;
        Unsigned32 child;

        for (child = 0U; child < description->storedNodeCount; child++)
        {
            if (description->nodes[child].blockIndex != edges[index].childBlock)
            {
                continue;
            }
            for (parent = 0U; parent < description->storedNodeCount; parent++)
            {
                if (description->nodes[parent].blockIndex == edges[index].parentBlock &&
                    parent != child)
                {
                    description->nodes[child].parentIndex = (Integer32)parent;
                }
            }
        }
    }

    return RESOURCE_NODE_OK;
}

static void setIdentity(Real32 *matrix)
{
    Unsigned32 index;

    for (index = 0U; index < 16U; index++)
    {
        matrix[index] = 0.0f;
    }
    matrix[0] = 1.0f;
    matrix[5] = 1.0f;
    matrix[10] = 1.0f;
    matrix[15] = 1.0f;
}

void resourceNodeBuildTransform(const Real32 *rotation, const Real32 *translation, Real32 *matrix)
{
    Real32 x = rotation[0];
    Real32 y = rotation[1];
    Real32 z = rotation[2];
    Real32 w = rotation[3];

    matrix[0] = 1.0f - 2.0f * (y * y + z * z);
    matrix[1] = 2.0f * (x * y + z * w);
    matrix[2] = 2.0f * (x * z - y * w);
    matrix[3] = 0.0f;

    matrix[4] = 2.0f * (x * y - z * w);
    matrix[5] = 1.0f - 2.0f * (x * x + z * z);
    matrix[6] = 2.0f * (y * z + x * w);
    matrix[7] = 0.0f;

    matrix[8] = 2.0f * (x * z + y * w);
    matrix[9] = 2.0f * (y * z - x * w);
    matrix[10] = 1.0f - 2.0f * (x * x + y * y);
    matrix[11] = 0.0f;

    matrix[12] = translation[0];
    matrix[13] = translation[1];
    matrix[14] = translation[2];
    matrix[15] = 1.0f;
}

static void buildLocalMatrix(const TransformNode *node, Real32 *matrix)
{
    resourceNodeBuildTransform(node->rotation, node->translation, matrix);
}

Integer32 resourceNodeFindByBoneIdentifier(const ResourceNodeDescription *description,
                                           Unsigned32 boneIdentifier)
{
    Unsigned32 index;

    for (index = 0U; index < description->storedNodeCount; index++)
    {
        if (description->nodes[index].boneIdentifier == boneIdentifier)
        {
            return (Integer32)index;
        }
    }
    return -1;
}

static void multiply(const Real32 *left, const Real32 *right, Real32 *result)
{
    Unsigned32 column;
    Unsigned32 row;

    for (column = 0U; column < 4U; column++)
    {
        for (row = 0U; row < 4U; row++)
        {
            Real32 sum = 0.0f;
            Unsigned32 inner;

            for (inner = 0U; inner < 4U; inner++)
            {
                sum += left[inner * 4U + row] * right[column * 4U + inner];
            }
            result[column * 4U + row] = sum;
        }
    }
}

void resourceNodeMultiplyTransforms(const Real32 *left, const Real32 *right, Real32 *result)
{
    multiply(left, right, result);
}

void resourceNodeGetWorldTransform(const ResourceNodeDescription *description, Unsigned32 nodeIndex,
                                   Real32 *matrix)
{
    Real32 accumulated[16];
    Real32 scratch[16];
    Integer32 current;
    Unsigned32 guard = 0U;

    setIdentity(matrix);
    if (nodeIndex >= description->storedNodeCount)
    {
        return;
    }

    buildLocalMatrix(&description->nodes[nodeIndex], accumulated);
    current = description->nodes[nodeIndex].parentIndex;
    while (current >= 0 && (Unsigned32)current < description->storedNodeCount &&
           guard < description->storedNodeCount)
    {
        Real32 parentMatrix[16];

        buildLocalMatrix(&description->nodes[current], parentMatrix);
        multiply(parentMatrix, accumulated, scratch);
        {
            Unsigned32 index;

            for (index = 0U; index < 16U; index++)
            {
                accumulated[index] = scratch[index];
            }
        }
        current = description->nodes[current].parentIndex;
        guard++;
    }

    {
        Unsigned32 index;

        for (index = 0U; index < 16U; index++)
        {
            matrix[index] = accumulated[index];
        }
    }
}
