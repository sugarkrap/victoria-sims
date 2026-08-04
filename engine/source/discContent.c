#include "victoria/discContent.h"

#include "utils/strings.h"
#include "victoria/compression.h"
#include "victoria/packageReader.h"

/* Files larger than this are not opened looking for geometry. A retail
 * objects.package is forty megabytes and holds no mesh worth drawing on its
 * own; the ceiling keeps a search from spending the whole budget on one file it
 * was never going to use. */
#define LARGEST_PACKAGE_BYTES (24UL * 1024UL * 1024UL)

const char *discContentStatusGetName(DiscContentStatus status)
{
    switch (status)
    {
    case DISC_CONTENT_FOUND:
        return "found geometry";
    case DISC_CONTENT_PENDING:
        return "still looking";
    case DISC_CONTENT_NONE_FOUND:
        return "no readable geometry on this disc";
    case DISC_CONTENT_OUT_OF_ARENA:
        return "not enough arena space to open a package";
    default:
        return "unknown";
    }
}

void discContentBegin(DiscContentSearch *search, VirtualFileSystem *fileSystem, MemoryArena *arena)
{
    Unsigned32 index;

    search->fileSystem = fileSystem;
    search->arena = arena;
    search->arenaMarker = memoryArenaGetMarker(arena);
    search->nextIndex = 0U;
    search->packagePath[0] = '\0';
    search->packagesOpened = 0U;
    search->packagesCompressed = 0U;
    search->packagesWithGeometry = 0U;
    search->packagesWithShapes = 0U;
    search->packagesWithTrees = 0U;
    search->modelHasTree = BOOLEAN_FALSE;
    search->modelNodeIndex = 0U;
    search->materialName[0] = '\0';
    search->materialFound = BOOLEAN_FALSE;
    search->textureFound = BOOLEAN_FALSE;
    search->modelsResolved = 0U;
    search->modelName[0] = '\0';
    search->foundThroughScenegraph = BOOLEAN_FALSE;
    search->geometryRefused = 0U;
    search->decompressionRefused = 0U;
    search->sawUnknownMark = BOOLEAN_FALSE;
    search->firstUnknownMark = 0U;
    search->largestElementCount = 0U;
    for (index = 0U; index < GEOMETRY_READ_RESULT_COUNT; index++)
    {
        search->refusalsByReason[index] = 0U;
    }
    for (index = 0U; index < DISC_CONTENT_VERSION_BUCKETS; index++)
    {
        search->versionsSeen[index] = 0U;
    }
}

static Boolean endsWithPackage(const char *path)
{
    MemorySize length = stringLength(path);
    MemorySize index;
    const char *suffix = ".package";
    MemorySize suffixLength = 8UL;

    if (length < suffixLength)
    {
        return BOOLEAN_FALSE;
    }
    for (index = 0UL; index < suffixLength; index++)
    {
        if (characterToLowerCase(path[length - suffixLength + index]) != suffix[index])
        {
            return BOOLEAN_FALSE;
        }
    }
    return BOOLEAN_TRUE;
}

/* The shape a model's resource node points at, or null.
 *
 * This is the top of the chain, and starting here rather than at the first
 * shape in the package is the difference between drawing a model and drawing
 * something that happened to be filed beside one. It also yields the transform
 * that says where the part belongs, which nothing below the resource node
 * knows. */
static const PackageResource *findShapeThroughResourceNode(DiscContentSearch *search,
                                                           const Package *package)
{
    const PackageResource *nodeResource =
        packageReaderFindFirstOfType(package, (Unsigned32)PACKAGE_TYPE_CRES);
    MemorySize marker;
    const Unsigned8 *nodeBytes;
    MemorySize nodeSize;
    Boolean compressed;
    const PackageResource *shape = NULL_POINTER;
    Unsigned32 index;

    search->modelHasTree = BOOLEAN_FALSE;
    if (nodeResource == NULL_POINTER)
    {
        return NULL_POINTER;
    }

    marker = memoryArenaGetMarker(search->arena);
    nodeBytes = scenegraphReadResourceBytes(search->arena, package, nodeResource, &nodeSize, &compressed);
    if (nodeBytes == NULL_POINTER ||
        resourceNodeRead(&search->modelTree, nodeBytes, nodeSize) != RESOURCE_NODE_OK)
    {
        memoryArenaRewindToMarker(search->arena, marker);
        return NULL_POINTER;
    }
    search->packagesWithTrees++;
    search->modelHasTree = BOOLEAN_TRUE;

    for (index = 0U; index < search->modelTree.storedNodeCount && shape == NULL_POINTER; index++)
    {
        if (!search->modelTree.nodes[index].hasShape)
        {
            continue;
        }
        shape = scenegraphFindResource(package, &search->modelTree.nodes[index].shapeKey);
        if (shape != NULL_POINTER)
        {
            search->modelNodeIndex = index;
        }
    }
    memoryArenaRewindToMarker(search->arena, marker);
    return shape;
}

/* Follows the chain down to a container, and says whether it got there.
 *
 * A package with no resource node and no shape is not a failure — plenty hold a
 * container and nothing else, and the caller falls back to taking one directly.
 * What this buys is a mesh that was chosen: part of a named model, rather than
 * whatever the index happened to list first. */
static const PackageResource *findGeometryThroughScenegraph(DiscContentSearch *search,
                                                            const Package *package)
{
    /* Asked for by the model that owns it first; only failing that, whichever
     * shape the package lists. The fallback is not a lesser answer for a
     * package that has no resource node — plenty do not — but for one that
     * does, taking any other shape would be picking a part of the model over
     * the model. */
    const PackageResource *shapeResource = findShapeThroughResourceNode(search, package);
    MemorySize marker;
    const Unsigned8 *shapeBytes;
    MemorySize shapeSize;
    Boolean compressed;
    ShapeDescription shape;
    const PackageResource *geometry = NULL_POINTER;
    Unsigned32 index;

    if (shapeResource == NULL_POINTER)
    {
        shapeResource = packageReaderFindFirstOfType(package, (Unsigned32)PACKAGE_TYPE_SHPE);
    }
    if (shapeResource == NULL_POINTER)
    {
        return NULL_POINTER;
    }

    marker = memoryArenaGetMarker(search->arena);
    shapeBytes = scenegraphReadResourceBytes(search->arena, package, shapeResource, &shapeSize, &compressed);
    if (shapeBytes == NULL_POINTER ||
        scenegraphReadShape(&shape, shapeBytes, shapeSize) != SCENEGRAPH_READ_OK)
    {
        memoryArenaRewindToMarker(search->arena, marker);
        return NULL_POINTER;
    }
    search->packagesWithShapes++;

    for (index = 0U; index < shape.storedMeshCount && geometry == NULL_POINTER; index++)
    {
        if (shape.meshNames[index][0] == '\0')
        {
            continue;
        }
        geometry = scenegraphFindGeometryNamed(search->arena, package, shape.meshNames[index]);
    }

    if (geometry != NULL_POINTER)
    {
        search->modelName[0] = '\0';
        stringAppend(search->modelName, RESOURCE_NAME_LIMIT, shape.resourceName);
        /* Kept before the shape's bytes are given back. The material is looked
           up later, once the geometry has been read and the arena is settled. */
        search->materialName[0] = '\0';
        if (shape.storedMaterialCount > 0U)
        {
            stringAppend(search->materialName, RESOURCE_NAME_LIMIT, shape.materials[0].materialName);
        }
    }
    /* The shape's own bytes are done with either way; the container is found by
     * index entry, which outlives them. */
    memoryArenaRewindToMarker(search->arena, marker);
    return geometry;
}

/* The material a part wears, and the texture it paints with.
 *
 * Every hop here is a string. A part wearing "ufocrash_cabin" wants the
 * resource named "ufocrash_cabin_txmt", which names a texture "ufocrash-cabin",
 * which lives in "ufocrash-cabin_txtr". Nothing is numbered, so a miss means a
 * name that did not match rather than a file that would not read — worth
 * keeping separate, because they call for opposite fixes.
 *
 * Called only once a package has been settled on, so what it allocates stays:
 * the texture's bytes are pointed at, not copied. */
static void findTextureForMaterial(DiscContentSearch *search, const Package *package)
{
    char wanted[RESOURCE_NAME_LIMIT];
    MaterialDescription material;
    Unsigned32 index;

    search->materialFound = BOOLEAN_FALSE;
    search->textureFound = BOOLEAN_FALSE;
    if (search->materialName[0] == '\0')
    {
        return;
    }

    materialBuildResourceName(wanted, sizeof(wanted), search->materialName, "_txmt");
    for (index = 0U; index < package->resourceCount && !search->materialFound; index++)
    {
        const PackageResource *candidate = &package->resources[index];
        MemorySize marker;
        const Unsigned8 *materialBytes;
        MemorySize materialSize;
        Boolean compressed;

        if (candidate->key.typeIdentifier != (Unsigned32)PACKAGE_TYPE_TXMT)
        {
            continue;
        }
        marker = memoryArenaGetMarker(search->arena);
        materialBytes =
            scenegraphReadResourceBytes(search->arena, package, candidate, &materialSize, &compressed);
        if (materialBytes != NULL_POINTER &&
            materialRead(&material, materialBytes, materialSize) == MATERIAL_READ_OK &&
            stringEquals(material.resourceName, wanted))
        {
            search->materialFound = BOOLEAN_TRUE;
        }
        /* The description is copied out by value, so its bytes are done with
         * either way and a material that did not match costs nothing. */
        memoryArenaRewindToMarker(search->arena, marker);
    }
    if (!search->materialFound || material.baseTextureName[0] == '\0')
    {
        return;
    }

    materialBuildResourceName(wanted, sizeof(wanted), material.baseTextureName, "_txtr");
    for (index = 0U; index < package->resourceCount && !search->textureFound; index++)
    {
        const PackageResource *candidate = &package->resources[index];
        MemorySize marker;
        const Unsigned8 *textureBytes;
        MemorySize textureSize;
        Boolean compressed;
        TextureDescription texture;

        if (candidate->key.typeIdentifier != (Unsigned32)PACKAGE_TYPE_TXTR)
        {
            continue;
        }
        marker = memoryArenaGetMarker(search->arena);
        textureBytes =
            scenegraphReadResourceBytes(search->arena, package, candidate, &textureSize, &compressed);
        if (textureBytes != NULL_POINTER &&
            textureReaderOpen(&texture, textureBytes, textureSize) == TEXTURE_READ_OK &&
            stringEquals(texture.resourceName, wanted))
        {
            /* Kept, so its bytes must be kept too: the description points into
             * them rather than owning them. This is the one place here that
             * deliberately does not rewind. */
            search->texture = texture;
            search->textureFound = BOOLEAN_TRUE;
        }
        else
        {
            memoryArenaRewindToMarker(search->arena, marker);
        }
    }
}

DiscContentStatus discContentStep(DiscContentSearch *search)
{
    const VirtualFileEntry *entry;
    Unsigned8 *bytes;
    MemorySize sizeInBytes;
    MemorySize attemptMarker;
    Package package;
    const PackageResource *geometry;
    const Unsigned8 *geometryBytes;
    MemorySize geometrySize;
    VirtualReadResult read;

    if (search->nextIndex >= search->fileSystem->entryCount)
    {
        return DISC_CONTENT_NONE_FOUND;
    }

    entry = virtualFileSystemGetEntry(search->fileSystem, search->nextIndex);
    if (entry == NULL_POINTER || !endsWithPackage(entry->path) ||
        entry->sizeInBytes > (Unsigned64)LARGEST_PACKAGE_BYTES || entry->sizeInBytes == 0U)
    {
        search->nextIndex++;
        return DISC_CONTENT_PENDING;
    }

    /* Everything this attempt allocates is given back unless it succeeds, so a
     * disc full of packages that turn out to hold nothing costs the arena
     * nothing at all. */
    attemptMarker = memoryArenaGetMarker(search->arena);
    sizeInBytes = (MemorySize)entry->sizeInBytes;
    bytes = (Unsigned8 *)memoryArenaAllocate(search->arena, sizeInBytes, 8UL);
    if (bytes == NULL_POINTER)
    {
        memoryArenaRewindToMarker(search->arena, attemptMarker);
        return DISC_CONTENT_OUT_OF_ARENA;
    }

    read = virtualFileSystemReadFile(search->fileSystem, search->nextIndex, 0U, sizeInBytes, bytes);
    if (read == VIRTUAL_READ_PENDING)
    {
        /* The bytes are not here yet. Give the space back and come again; the
         * index is untouched, so the next step retries this same package. */
        memoryArenaRewindToMarker(search->arena, attemptMarker);
        return DISC_CONTENT_PENDING;
    }

    search->nextIndex++;
    if (read != VIRTUAL_READ_OK ||
        packageReaderOpen(&package, bytes, sizeInBytes, search->arena) != PACKAGE_READ_OK)
    {
        memoryArenaRewindToMarker(search->arena, attemptMarker);
        return DISC_CONTENT_PENDING;
    }
    search->packagesOpened++;

    /* Ask the scenegraph first. A shape names the meshes a model is built from,
     * so a container reached that way is one this engine chose rather than the
     * first the index happened to list. Packages that carry no shape fall back
     * to the blunt rule, which is how the engine has worked until now. */
    search->modelName[0] = '\0';
    geometry = findGeometryThroughScenegraph(search, &package);
    search->foundThroughScenegraph = (geometry != NULL_POINTER) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
    if (geometry != NULL_POINTER)
    {
        search->modelsResolved++;
    }
    else
    {
        geometry = packageReaderFindFirstOfType(&package, (Unsigned32)PACKAGE_TYPE_GMDC);
    }
    if (geometry == NULL_POINTER)
    {
        memoryArenaRewindToMarker(search->arena, attemptMarker);
        return DISC_CONTENT_PENDING;
    }
    search->packagesWithGeometry++;

    {
        Boolean compressed = BOOLEAN_FALSE;

        geometryBytes = scenegraphReadResourceBytes(search->arena, &package, geometry, &geometrySize, &compressed);
        if (compressed)
        {
            search->packagesCompressed++;
        }
        if (geometryBytes == NULL_POINTER && compressed)
        {
            search->geometryRefused++;
            search->decompressionRefused++;
            memoryArenaRewindToMarker(search->arena, attemptMarker);
            return DISC_CONTENT_PENDING;
        }
    }

    if (geometryBytes == NULL_POINTER)
    {
        search->geometryRefused++;
        search->refusalsByReason[GEOMETRY_READ_TRUNCATED]++;
        memoryArenaRewindToMarker(search->arena, attemptMarker);
        return DISC_CONTENT_PENDING;
    }

    {
        GeometryReadResult readResult =
            geometryReaderOpen(&search->mesh, geometryBytes, geometrySize, search->arena);

        /* Recorded whether or not the read succeeded. What the disc holds does
           not depend on what this engine could do with it. */
        if (search->mesh.containerVersion != 0U)
        {
            Unsigned32 bucket = search->mesh.containerVersion;

            if (bucket >= DISC_CONTENT_VERSION_BUCKETS)
            {
                bucket = DISC_CONTENT_VERSION_BUCKETS - 1U;
            }
            search->versionsSeen[bucket]++;
        }
        if (search->mesh.versionMark != 0xFFFF0001UL && !search->sawUnknownMark)
        {
            search->sawUnknownMark = BOOLEAN_TRUE;
            search->firstUnknownMark = search->mesh.versionMark;
        }
        if (search->mesh.elementCount > search->largestElementCount)
        {
            search->largestElementCount = search->mesh.elementCount;
        }

        if (readResult != GEOMETRY_READ_OK)
        {
            search->geometryRefused++;
            if ((Unsigned32)readResult < GEOMETRY_READ_RESULT_COUNT)
            {
                search->refusalsByReason[(Unsigned32)readResult]++;
            }
            memoryArenaRewindToMarker(search->arena, attemptMarker);
            return DISC_CONTENT_PENDING;
        }
    }

    findTextureForMaterial(search, &package);

    search->packagePath[0] = '\0';
    stringAppend(search->packagePath, DISC_CONTENT_PATH_LIMIT, entry->path);
    return DISC_CONTENT_FOUND;
}

DiscContentStatus discContentRunToCompletion(DiscContentSearch *search)
{
    /* Bounded so a store that answers PENDING forever cannot hang a caller that
     * should not have used this. */
    Unsigned32 remaining = 1000000U;

    for (;;)
    {
        DiscContentStatus status = discContentStep(search);

        if (status != DISC_CONTENT_PENDING)
        {
            return status;
        }
        if (remaining == 0U)
        {
            return DISC_CONTENT_NONE_FOUND;
        }
        remaining--;
    }
}
