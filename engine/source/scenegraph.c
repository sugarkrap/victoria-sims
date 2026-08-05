#include "victoria/scenegraph.h"

#include "utils/strings.h"
#include "utils/resourceHash.h"
#include "victoria/compression.h"

const char *scenegraphReadResultGetName(ScenegraphReadResult result)
{
    switch (result)
    {
    case SCENEGRAPH_READ_OK:
        return "ok";
    case SCENEGRAPH_READ_NOT_A_RESOURCE:
        return "not a scenegraph resource";
    case SCENEGRAPH_READ_OLDER_COLLECTION:
        return "an older collection, laid out differently";
    case SCENEGRAPH_READ_WRONG_TYPE:
        return "a scenegraph resource of the wrong kind";
    case SCENEGRAPH_READ_TRUNCATED:
        return "the resource ends part way through";
    case SCENEGRAPH_READ_NO_REFERENCE:
        return "nothing to follow from here";
    case SCENEGRAPH_READ_UNSUPPORTED_VERSION:
        return "a block version this reader does not decode";
    default:
        return "unknown";
    }
}

static MemorySize remainingBytes(const ResourceCursor *cursor)
{
    if (cursor->overran)
    {
        return 0UL;
    }
    return cursor->sizeInBytes - cursor->position;
}

/* A count is believable only if the bytes to satisfy it exist. Every list in
 * these blocks costs at least one byte an entry, so a count larger than what is
 * left is not a count — and looping on it would spin four billion times before
 * noticing. */
static Boolean countFits(ResourceCursor *cursor, Unsigned32 count)
{
    if (cursor->overran || (MemorySize)count > remainingBytes(cursor))
    {
        cursor->overran = BOOLEAN_TRUE;
        return BOOLEAN_FALSE;
    }
    return BOOLEAN_TRUE;
}

/* cObjectGraphNode, which prefixes most blocks. Carries the list of resources
 * the block depends on and, from version 4, the tag that names it. */
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

/* cSGResource: the name a scenegraph resource answers to. */
static void readResourceName(ResourceCursor *cursor, char *name, MemorySize capacity)
{
    resourceCursorReadTypeInformation(cursor, NULL_POINTER);
    resourceCursorReadString(cursor, name, capacity);
}

static ScenegraphReadResult translateCollectionResult(ResourceCollectionResult result)
{
    switch (result)
    {
    case RESOURCE_COLLECTION_OK:
        return SCENEGRAPH_READ_OK;
    case RESOURCE_COLLECTION_OLDER:
        return SCENEGRAPH_READ_OLDER_COLLECTION;
    case RESOURCE_COLLECTION_TRUNCATED:
        return SCENEGRAPH_READ_TRUNCATED;
    default:
        return SCENEGRAPH_READ_NOT_A_RESOURCE;
    }
}

ScenegraphReadResult scenegraphReadShape(ShapeDescription *shape, const Unsigned8 *bytes,
                                         MemorySize sizeInBytes)
{
    ResourceCollection collection;
    ResourceCursor cursor;
    ResourceCollectionResult collectionResult;
    PersistTypeInfo blockType;
    Unsigned32 count;
    Unsigned32 index;

    shape->resourceName[0] = '\0';
    shape->blockVersion = 0U;
    shape->meshCount = 0U;
    shape->storedMeshCount = 0U;
    shape->materialCount = 0U;
    shape->storedMaterialCount = 0U;

    collectionResult = resourceCollectionOpen(&collection, &cursor, bytes, sizeInBytes);
    if (collectionResult != RESOURCE_COLLECTION_OK)
    {
        return translateCollectionResult(collectionResult);
    }
    if (collection.firstBlockType != (Unsigned32)PACKAGE_TYPE_SHPE)
    {
        return SCENEGRAPH_READ_WRONG_TYPE;
    }

    resourceCursorReadTypeInformation(&cursor, &blockType);
    if (cursor.overran)
    {
        return SCENEGRAPH_READ_TRUNCATED;
    }
    if (blockType.typeIdentifier != (Unsigned32)PACKAGE_TYPE_SHPE)
    {
        return SCENEGRAPH_READ_WRONG_TYPE;
    }
    shape->blockVersion = blockType.version;
    /* Below this the layout differs and there is nothing on hand to check a
     * guess against. Refused by version rather than misread. */
    if (blockType.version <= 5U)
    {
        return SCENEGRAPH_READ_UNSUPPORTED_VERSION;
    }

    readResourceName(&cursor, shape->resourceName, RESOURCE_NAME_LIMIT);
    /* A cReferentNode, which is a cObjectGraphNode wearing another name. */
    resourceCursorReadTypeInformation(&cursor, NULL_POINTER);
    readObjectGraphNode(&cursor, NULL_POINTER, 0UL);

    if (blockType.version > 6U)
    {
        count = resourceCursorReadUnsigned32(&cursor);
        if (!countFits(&cursor, count))
        {
            return SCENEGRAPH_READ_TRUNCATED;
        }
        resourceCursorSkip(&cursor, (MemorySize)count * 4UL);
    }

    shape->meshCount = resourceCursorReadUnsigned32(&cursor);
    if (!countFits(&cursor, shape->meshCount))
    {
        return SCENEGRAPH_READ_TRUNCATED;
    }
    for (index = 0U; index < shape->meshCount; index++)
    {
        Unsigned32 levelOfDetail = resourceCursorReadUnsigned32(&cursor);
        Boolean namedByReference = BOOLEAN_TRUE;
        char *destination = NULL_POINTER;
        MemorySize capacity = 0UL;

        if (blockType.version >= 8U)
        {
            namedByReference = (resourceCursorReadUnsigned8(&cursor) == 0U) ? BOOLEAN_TRUE
                                                                           : BOOLEAN_FALSE;
        }
        if (shape->storedMeshCount < SCENEGRAPH_MESH_LIMIT)
        {
            destination = shape->meshNames[shape->storedMeshCount];
            capacity = RESOURCE_NAME_LIMIT;
        }

        if (namedByReference)
        {
            /* Named through a file link rather than by string. Skipped rather
             * than resolved: no shape met so far does this, so resolving it
             * would be code written against nothing. */
            (void)resourceCursorReadObjectReference(&cursor);
            if (destination != NULL_POINTER)
            {
                destination[0] = '\0';
            }
        }
        else
        {
            resourceCursorReadString(&cursor, destination, capacity);
        }
        if (destination != NULL_POINTER)
        {
            shape->meshLevelsOfDetail[shape->storedMeshCount] = levelOfDetail;
            shape->storedMeshCount++;
        }
        if (cursor.overran)
        {
            return SCENEGRAPH_READ_TRUNCATED;
        }
    }

    shape->materialCount = resourceCursorReadUnsigned32(&cursor);
    if (!countFits(&cursor, shape->materialCount))
    {
        return SCENEGRAPH_READ_TRUNCATED;
    }
    for (index = 0U; index < shape->materialCount; index++)
    {
        ShapeMaterialBinding *binding = NULL_POINTER;
        Unsigned32 extraGroups;
        Unsigned32 extra;

        if (shape->storedMaterialCount < SCENEGRAPH_MATERIAL_LIMIT)
        {
            binding = &shape->materials[shape->storedMaterialCount];
        }
        if (binding != NULL_POINTER)
        {
            resourceCursorReadString(&cursor, binding->primitiveName, RESOURCE_NAME_LIMIT);
            resourceCursorReadString(&cursor, binding->materialName, RESOURCE_NAME_LIMIT);
            shape->storedMaterialCount++;
        }
        else
        {
            resourceCursorReadString(&cursor, NULL_POINTER, 0UL);
            resourceCursorReadString(&cursor, NULL_POINTER, 0UL);
        }

        (void)resourceCursorReadUnsigned8(&cursor);
        extraGroups = resourceCursorReadUnsigned32(&cursor);
        if (!countFits(&cursor, extraGroups))
        {
            return SCENEGRAPH_READ_TRUNCATED;
        }
        for (extra = 0U; extra < extraGroups; extra++)
        {
            resourceCursorReadString(&cursor, NULL_POINTER, 0UL);
        }
        (void)resourceCursorReadUnsigned32(&cursor);
        if (cursor.overran)
        {
            return SCENEGRAPH_READ_TRUNCATED;
        }
    }

    return SCENEGRAPH_READ_OK;
}

ScenegraphReadResult scenegraphReadGeometryNode(GeometryNodeDescription *node, const Unsigned8 *bytes,
                                                MemorySize sizeInBytes)
{
    ResourceCollection collection;
    ResourceCursor cursor;
    ResourceCollectionResult collectionResult;
    PersistTypeInfo blockType;
    ObjectReference reference;
    const PackageResourceKey *link;

    node->resourceName[0] = '\0';
    node->blockVersion = 0U;
    node->hasGeometry = BOOLEAN_FALSE;
    node->geometryKey.typeIdentifier = 0U;
    node->geometryKey.groupIdentifier = 0U;
    node->geometryKey.instanceIdentifier = 0U;
    node->geometryKey.instanceIdentifierHigh = 0U;

    collectionResult = resourceCollectionOpen(&collection, &cursor, bytes, sizeInBytes);
    if (collectionResult != RESOURCE_COLLECTION_OK)
    {
        return translateCollectionResult(collectionResult);
    }
    if (collection.firstBlockType != (Unsigned32)PACKAGE_TYPE_GMND)
    {
        return SCENEGRAPH_READ_WRONG_TYPE;
    }

    resourceCursorReadTypeInformation(&cursor, &blockType);
    if (cursor.overran)
    {
        return SCENEGRAPH_READ_TRUNCATED;
    }
    if (blockType.typeIdentifier != (Unsigned32)PACKAGE_TYPE_GMND)
    {
        return SCENEGRAPH_READ_WRONG_TYPE;
    }
    node->blockVersion = blockType.version;

    readObjectGraphNode(&cursor, NULL_POINTER, 0UL);
    readResourceName(&cursor, node->resourceName, RESOURCE_NAME_LIMIT);

    /* Fields the game reads and does nothing with. Skipped by version because
     * that is how they were written, and getting the count wrong moves
     * everything after them. */
    if (blockType.version > 6U)
    {
        (void)resourceCursorReadUnsigned8(&cursor);
    }
    if (blockType.version == 11U)
    {
        resourceCursorSkip(&cursor, 2UL);
    }
    if (blockType.version == 6U)
    {
        resourceCursorSkip(&cursor, 3UL);
    }
    if (cursor.overran)
    {
        return SCENEGRAPH_READ_TRUNCATED;
    }
    if (blockType.version < 8U)
    {
        /* The block ends here. Not a failure — a node this old carries no
         * reference to a container, and the game stops reading at the same
         * point. */
        return SCENEGRAPH_READ_NO_REFERENCE;
    }
    if (blockType.version < 10U)
    {
        resourceCursorSkip(&cursor, 12UL);
    }

    reference = resourceCursorReadObjectReference(&cursor);
    if (cursor.overran)
    {
        return SCENEGRAPH_READ_TRUNCATED;
    }
    link = resourceCollectionGetLink(&collection, reference);
    if (link == NULL_POINTER)
    {
        return SCENEGRAPH_READ_NO_REFERENCE;
    }
    node->geometryKey = *link;
    node->hasGeometry = BOOLEAN_TRUE;
    return SCENEGRAPH_READ_OK;
}

const PackageResource *scenegraphFindResource(const Package *package, const PackageResourceKey *key)
{
    Unsigned32 index;

    for (index = 0U; index < package->resourceCount; index++)
    {
        const PackageResource *resource = &package->resources[index];

        /* All four words. Two resources differing only in the high instance
         * word are different resources, and a lookup that ignores it will
         * cheerfully return the wrong one. */
        if (resource->key.typeIdentifier == key->typeIdentifier &&
            resource->key.groupIdentifier == key->groupIdentifier &&
            resource->key.instanceIdentifier == key->instanceIdentifier &&
            resource->key.instanceIdentifierHigh == key->instanceIdentifierHigh)
        {
            return resource;
        }
    }
    return NULL_POINTER;
}

/* A resource's bytes, unpacked if the stream says it is packed.
 *
 * Almost everything on a retail disc is stored compressed, so what the index
 * points at is usually not the resource — it is a RefPack stream that unpacks
 * into it. Detected from the stream's own header rather than from the package's
 * directory resource: the header is on the thing being read, which is the
 * harder of the two to be wrong about.
 *
 * Anything allocated here comes from the arena, so a caller that rewinds gets
 * it back. */
const Unsigned8 *scenegraphReadResourceBytes(MemoryArena *arena, const Package *package,
                                             const PackageResource *resource, MemorySize *sizeInBytes,
                                             Boolean *wasCompressed)
{
    const Unsigned8 *bytes = packageReaderGetResourceBytes(package, resource);
    MemorySize size = (MemorySize)resource->sizeInBytes;
    MemorySize decompressedSize;
    Unsigned8 *unpacked;

    *wasCompressed = BOOLEAN_FALSE;
    *sizeInBytes = 0UL;
    if (bytes == NULL_POINTER)
    {
        return NULL_POINTER;
    }
    if (!compressionLooksLikeRefPack(bytes, size))
    {
        *sizeInBytes = size;
        return bytes;
    }

    *wasCompressed = BOOLEAN_TRUE;
    decompressedSize = compressionGetDecompressedSize(bytes, size);
    unpacked = (Unsigned8 *)memoryArenaAllocate(arena, decompressedSize, 8UL);
    if (unpacked == NULL_POINTER)
    {
        return NULL_POINTER;
    }
    if (compressionDecompressRefPack(unpacked, decompressedSize, bytes, size, &decompressedSize) !=
        COMPRESSION_OK)
    {
        return NULL_POINTER;
    }
    *sizeInBytes = decompressedSize;
    return unpacked;
}

/* The container a named geometry node points at, or null.
 *
 * Every geometry node in the package is read and matched by the name it carries
 * rather than by its instance identifier. That is a walk over a handful of small
 * resources, and it is the right way round: the shape names its meshes, and a
 * name is what the file gives us to match on. */
const PackageResource *scenegraphFindResourceByInstance(const Package *package,
                                                        Unsigned32 typeIdentifier,
                                                        Unsigned32 instanceIdentifier,
                                                        Unsigned32 instanceIdentifierHigh)
{
    Unsigned32 index;

    for (index = 0U; index < package->resourceCount; index++)
    {
        const PackageResource *resource = &package->resources[index];

        if (resource->key.typeIdentifier == typeIdentifier &&
            resource->key.instanceIdentifier == instanceIdentifier &&
            resource->key.instanceIdentifierHigh == instanceIdentifierHigh)
        {
            return resource;
        }
    }
    return NULL_POINTER;
}

/* Reads one geometry node and returns the container it points at. */
static const PackageResource *followGeometryNode(MemoryArena *arena, const Package *package,
                                                 const PackageResource *nodeResource,
                                                 const char *wantedName)
{
    MemorySize marker = memoryArenaGetMarker(arena);
    MemorySize nodeSize;
    Boolean compressed;
    const Unsigned8 *nodeBytes =
        scenegraphReadResourceBytes(arena, package, nodeResource, &nodeSize, &compressed);
    GeometryNodeDescription node;
    const PackageResource *geometry = NULL_POINTER;

    if (nodeBytes != NULL_POINTER &&
        scenegraphReadGeometryNode(&node, nodeBytes, nodeSize) == SCENEGRAPH_READ_OK &&
        node.hasGeometry &&
        (wantedName == NULL_POINTER || stringEquals(node.resourceName, wantedName)))
    {
        geometry = scenegraphFindResource(package, &node.geometryKey);
    }
    /* The key is a struct copy, so it survives the rewind. */
    memoryArenaRewindToMarker(arena, marker);
    return geometry;
}

const PackageResource *scenegraphFindGeometryNamed(MemoryArena *arena, const Package *package,
                                                   const char *nodeName)
{
    Unsigned32 index;

    /* Asked for by key first. A scenegraph resource's instance words are its
     * name hashed, so the node can be found without opening anything — one
     * read instead of one per node in the package.
     *
     * The walk below is kept rather than deleted. The hash holds for every
     * resource checked so far, but "every resource checked so far" is three
     * fixtures and a retail disc; a resource filed under a key that does not
     * match its name would simply not be found, and silently drawing nothing
     * is a worse failure than reading a few extra resources. */
    {
        const PackageResource *nodeResource = scenegraphFindResourceByInstance(
            package, (Unsigned32)PACKAGE_TYPE_GMND, resourceHashInstance(nodeName),
            resourceHashInstanceHigh(nodeName));

        if (nodeResource != NULL_POINTER)
        {
            const PackageResource *geometry =
                followGeometryNode(arena, package, nodeResource, NULL_POINTER);

            if (geometry != NULL_POINTER)
            {
                return geometry;
            }
        }
    }

    for (index = 0U; index < package->resourceCount; index++)
    {
        const PackageResource *candidate = &package->resources[index];
        const PackageResource *geometry;

        if (candidate->key.typeIdentifier != (Unsigned32)PACKAGE_TYPE_GMND)
        {
            continue;
        }
        /* Each candidate is read and given straight back, so scanning a package
         * full of nodes costs the arena one node at a time rather than all of
         * them. */
        geometry = followGeometryNode(arena, package, candidate, nodeName);
        if (geometry != NULL_POINTER)
        {
            return geometry;
        }
    }
    return NULL_POINTER;
}
