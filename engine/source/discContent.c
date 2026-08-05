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
    search->materialsInPackage = 0U;
    search->texturesInPackage = 0U;
    search->textureName[0] = '\0';
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
    return stringEndsWithIgnoringCase(path, ".package");
}

/* Reads the model's transform tree, and says whether there is one.
 *
 * This is the top of the chain, and starting here rather than at the first
 * shape in the package is the difference between drawing a model and drawing
 * something that happened to be filed beside one. It also yields the transforms
 * that say where each part belongs, which nothing below the resource node
 * knows. */
static Boolean readModelTree(DiscContentSearch *search, const Package *package)
{
    const PackageResource *nodeResource =
        packageReaderFindFirstOfType(package, (Unsigned32)PACKAGE_TYPE_CRES);
    MemorySize marker;
    const Unsigned8 *nodeBytes;
    MemorySize nodeSize;
    Boolean compressed;

    search->modelHasTree = BOOLEAN_FALSE;
    if (nodeResource == NULL_POINTER)
    {
        return BOOLEAN_FALSE;
    }

    marker = memoryArenaGetMarker(search->arena);
    nodeBytes = scenegraphReadResourceBytes(search->arena, package, nodeResource, &nodeSize, &compressed);
    if (nodeBytes == NULL_POINTER ||
        resourceNodeRead(&search->modelTree, nodeBytes, nodeSize) != RESOURCE_NODE_OK)
    {
        memoryArenaRewindToMarker(search->arena, marker);
        return BOOLEAN_FALSE;
    }
    search->packagesWithTrees++;
    search->modelHasTree = BOOLEAN_TRUE;
    /* The tree is a struct of its own, so the bytes it was read from are done
       with the moment it is filled. */
    memoryArenaRewindToMarker(search->arena, marker);
    return BOOLEAN_TRUE;
}

/* Remembers one part of the model, if there is room to.
 *
 * The material a mesh wears is found by matching the shape's own bindings: a
 * shape lists meshes and it lists materials, and the two are joined by the
 * primitive's name rather than by position. Taking the first material for every
 * mesh would dress a Sim's hands in its face. */
static void rememberPart(DiscContentSearch *search, const ShapeDescription *shape,
                         Unsigned32 meshIndex, Unsigned32 nodeIndex)
{
    DiscModelPart *part;
    Unsigned32 which;

    if (search->partCount >= (Unsigned32)DISC_CONTENT_PART_LIMIT)
    {
        search->partsBeyondRoom++;
        return;
    }
    part = &search->parts[search->partCount];
    part->meshName[0] = '\0';
    part->materialName[0] = '\0';
    part->shapeName[0] = '\0';
    part->nodeIndex = nodeIndex;
    part->levelOfDetail = shape->meshLevelsOfDetail[meshIndex];
    stringAppend(part->meshName, RESOURCE_NAME_LIMIT, shape->meshNames[meshIndex]);
    stringAppend(part->shapeName, RESOURCE_NAME_LIMIT, shape->resourceName);

    for (which = 0U; which < shape->storedMaterialCount; which++)
    {
        if (stringEquals(shape->materials[which].primitiveName, shape->meshNames[meshIndex]))
        {
            stringAppend(part->materialName, RESOURCE_NAME_LIMIT,
                         shape->materials[which].materialName);
            break;
        }
    }
    /* No binding names this mesh. Its first material is a guess, but a guess
       that is right whenever a shape wears one material — which is most of
       them — and an empty name would find nothing at all. */
    if (part->materialName[0] == '\0' && shape->storedMaterialCount > 0U)
    {
        stringAppend(part->materialName, RESOURCE_NAME_LIMIT, shape->materials[0].materialName);
    }
    search->partCount++;
}

/* Reads one shape and remembers every mesh it names. Returns the first mesh
   that resolved to a container, which is the one drawn while there is still
   only one being drawn. */
static const PackageResource *collectShapeParts(DiscContentSearch *search, const Package *package,
                                                const PackageResource *shapeResource,
                                                Unsigned32 nodeIndex)
{
    MemorySize marker = memoryArenaGetMarker(search->arena);
    const Unsigned8 *shapeBytes;
    MemorySize shapeSize;
    Boolean compressed;
    ShapeDescription shape;
    const PackageResource *geometry = NULL_POINTER;
    Unsigned32 index;

    shapeBytes = scenegraphReadResourceBytes(search->arena, package, shapeResource, &shapeSize,
                                             &compressed);
    if (shapeBytes == NULL_POINTER ||
        scenegraphReadShape(&shape, shapeBytes, shapeSize) != SCENEGRAPH_READ_OK)
    {
        memoryArenaRewindToMarker(search->arena, marker);
        return NULL_POINTER;
    }
    search->packagesWithShapes++;

    /* Every mesh the shape names, not the first that resolves. The one that
       resolves first is still the one drawn today, but the rest are what a Sim
       is made of and nothing was ever written down about them. */
    for (index = 0U; index < shape.storedMeshCount; index++)
    {
        const PackageResource *named;

        if (shape.meshNames[index][0] == '\0')
        {
            continue;
        }
        named = scenegraphFindGeometryNamed(search->arena, package, shape.meshNames[index]);
        if (named == NULL_POINTER)
        {
            continue;
        }
        rememberPart(search, &shape, index, nodeIndex);
        if (geometry == NULL_POINTER)
        {
            geometry = named;
        }
    }

    if (geometry != NULL_POINTER && search->modelName[0] == '\0')
    {
        stringAppend(search->modelName, RESOURCE_NAME_LIMIT, shape.resourceName);
        /* Kept before the shape's bytes are given back. The material is looked
           up later, once the geometry has been read and the arena is settled. */
        if (shape.storedMaterialCount > 0U)
        {
            stringAppend(search->materialName, RESOURCE_NAME_LIMIT,
                         shape.materials[0].materialName);
        }
    }
    /* The shape's own bytes are done with either way; the container is found by
     * index entry, which outlives them. */
    memoryArenaRewindToMarker(search->arena, marker);
    return geometry;
}

/* Follows the chain down to a container, and says whether it got there.
 *
 * A package with no resource node and no shape is not a failure — plenty hold a
 * container and nothing else, and the caller falls back to taking one directly.
 * What this buys is a mesh that was chosen: part of a named model, rather than
 * whatever the index happened to list first.
 *
 * Every node of the tree that names a shape is followed, not the first. A Sim's
 * head, body and hands are separate shapes hanging off separate nodes, and
 * stopping at the first is the whole reason what arrives on screen is a face. */
static const PackageResource *findGeometryThroughScenegraph(DiscContentSearch *search,
                                                            const Package *package)
{
    const PackageResource *geometry = NULL_POINTER;

    search->modelName[0] = '\0';
    search->materialName[0] = '\0';
    search->partCount = 0U;
    search->partsBeyondRoom = 0U;

    if (readModelTree(search, package))
    {
        Unsigned32 index;

        for (index = 0U; index < search->modelTree.storedNodeCount; index++)
        {
            const PackageResource *shapeResource;
            const PackageResource *fromThisShape;

            if (!search->modelTree.nodes[index].hasShape)
            {
                continue;
            }
            shapeResource = scenegraphFindResource(package, &search->modelTree.nodes[index].shapeKey);
            if (shapeResource == NULL_POINTER)
            {
                continue;
            }
            fromThisShape = collectShapeParts(search, package, shapeResource, index);
            if (fromThisShape != NULL_POINTER && geometry == NULL_POINTER)
            {
                geometry = fromThisShape;
                search->modelNodeIndex = index;
            }
        }
    }

    if (geometry != NULL_POINTER)
    {
        return geometry;
    }

    /* Whichever shape the package lists, for one that has no resource node.
     * Not a lesser answer there — plenty do not have one — but for a package
     * that does, taking any other shape would be picking a part of the model
     * over the model. */
    {
        const PackageResource *shapeResource =
            packageReaderFindFirstOfType(package, (Unsigned32)PACKAGE_TYPE_SHPE);

        if (shapeResource == NULL_POINTER)
        {
            return NULL_POINTER;
        }
        return collectShapeParts(search, package, shapeResource, 0U);
    }
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
    search->materialsInPackage = 0U;
    search->texturesInPackage = 0U;
    search->textureName[0] = '\0';
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
        search->materialsInPackage++;
        if (materialBytes != NULL_POINTER &&
            materialRead(&material, materialBytes, materialSize) == MATERIAL_READ_OK &&
            /* Either spelling. The convention is that a binding "x" is the
             * resource "x_txmt", but the material also carries the binding name
             * in a field of its own, and trusting only the convention means a
             * material that names itself correctly is missed for having been
             * filed under something else. */
            (stringEquals(material.resourceName, wanted) ||
             stringEquals(material.materialName, search->materialName)))
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
    stringAppend(search->textureName, RESOURCE_NAME_LIMIT, material.baseTextureName);

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
        search->texturesInPackage++;
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
