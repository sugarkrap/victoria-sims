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
    search->walkingPreferred = BOOLEAN_TRUE;
    search->foundInPreferred = BOOLEAN_FALSE;
    search->wantingSkinned = BOOLEAN_TRUE;
    search->rigidModelFound = BOOLEAN_FALSE;
    search->rigidModelIndex = 0U;
    search->rigidModelsPassed = 0U;
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

/* Where the game keeps the meshes a Sim is built from. Not a guess about this
   disc: it is the directory the archive's own entries named, and the packages
   mounted out of it are the ones holding whole models rather than one face. */
#define PREFERRED_DIRECTORY "Sims3D"

/* Skipped on the first round. A locale's Sims3D holds that language's objects —
   the archway this drew is the Japanese one — and objects are static: their
   containers carry positions, normals and texture coordinates and nothing else.
   The meshes a Sim is built from are under the plain Sims3D, and they are the
   only ones that can say what a bone element looks like. */
#define PREFERRED_EXCLUDES "Locale"

/* How many rigid models the first round will walk past before settling for one.
 *
 * Each is a package read, and on the web every package read is a round trip, so
 * this is the difference between a search that looks and one that stalls the
 * load. Forty-eight because the preferred directory holds hundreds of packages
 * and a disc that has a body mesh at all will not hide it behind fifty faces. */
#define RIGID_MODELS_TO_WALK_PAST 48U

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

    /* A shape names the same mesh several times over, once per level of
       detail: a face and the same face coarsened for distance. Keeping both
       would draw the face twice, the second time through the first. The finest
       is the one with the lowest level number, and a shape that numbers them
       all the same leaves the first met standing. */
    for (which = 0U; which < search->partCount; which++)
    {
        if (!stringEquals(search->parts[which].shapeName, shape->resourceName))
        {
            continue;
        }
        if (shape->meshLevelsOfDetail[meshIndex] < search->parts[which].levelOfDetail)
        {
            search->parts[which].levelOfDetail = shape->meshLevelsOfDetail[meshIndex];
            search->parts[which].meshName[0] = '\0';
            stringAppend(search->parts[which].meshName, RESOURCE_NAME_LIMIT,
                         shape->meshNames[meshIndex]);
        }
        search->coarserPartsDropped++;
        return;
    }

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

    if (geometry != NULL_POINTER && search->bindingCount == 0U)
    {
        /* Copied out whole. Which primitive wears which material cannot be
           settled here — the primitives are inside the container, which has not
           been read yet — and by the time it can be, these bytes are gone. */
        Unsigned32 binding;

        for (binding = 0U; binding < shape.storedMaterialCount &&
                           binding < (Unsigned32)SCENEGRAPH_MATERIAL_LIMIT;
             binding++)
        {
            search->bindings[search->bindingCount] = shape.materials[binding];
            search->bindingCount++;
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
    search->bindingCount = 0U;
    search->shapeReferences = 0U;
    search->shapeReferencesResolved = 0U;
    search->coarserPartsDropped = 0U;

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
            search->shapeReferences++;
            shapeResource = scenegraphFindResource(package, &search->modelTree.nodes[index].shapeKey);
            if (shapeResource == NULL_POINTER)
            {
                /* Named, but not here. Nothing is wrong with the reference —
                   a Sim's body meshes live in the packages the game ships,
                   not in the file that describes one Sim. */
                continue;
            }
            search->shapeReferencesResolved++;
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

/* Which material each primitive wears, and which of them the model is painted
 * with while it is still painted with one.
 *
 * A model's parts are primitives inside one container, not shapes: the torii
 * gate is a two triangle shadow and two hundred and forty four triangles of
 * stone, and the shape names a material for each by the primitive's own name.
 * Taking the first put a sixty-four by thirty-two alpha shadow mask on a stone
 * gate, which is not a texture bug and not a lookup bug — it is the wrong
 * question being asked once instead of the right one being asked per part.
 *
 * Until the renderer can paint each range separately, the one chosen is the
 * material of the primitive covering most of the model. That is a compromise
 * and it is stated as one: the log names every primitive's material, so the
 * difference between what was chosen and what was wanted is visible. */
static void chooseMaterialPerPrimitive(DiscContentSearch *search)
{
    Unsigned32 primitive;
    Unsigned32 widest = 0U;
    Unsigned32 widestIndexCount = 0U;

    search->partCount = 0U;
    search->partsBeyondRoom = 0U;
    search->coarserPartsDropped = 0U;

    for (primitive = 0U; primitive < search->mesh.storedPrimitiveCount; primitive++)
    {
        const GeometryPrimitive *piece = &search->mesh.primitives[primitive];
        DiscModelPart *part;
        Unsigned32 binding;

        if (search->partCount >= (Unsigned32)DISC_CONTENT_PART_LIMIT)
        {
            search->partsBeyondRoom++;
            continue;
        }
        part = &search->parts[search->partCount];
        part->meshName[0] = '\0';
        part->materialName[0] = '\0';
        part->shapeName[0] = '\0';
        part->nodeIndex = search->modelNodeIndex;
        part->levelOfDetail = 0U;
        part->firstIndex = piece->firstIndex;
        part->indexCount = piece->indexCount;
        stringAppend(part->meshName, RESOURCE_NAME_LIMIT, piece->name);

        for (binding = 0U; binding < search->bindingCount; binding++)
        {
            if (stringEquals(search->bindings[binding].primitiveName, piece->name))
            {
                stringAppend(part->materialName, RESOURCE_NAME_LIMIT,
                             search->bindings[binding].materialName);
                break;
            }
        }
        /* No binding names this primitive. The shape's only material is a
           guess, but one that is right whenever a model wears a single
           material, and an empty name would find nothing at all. */
        if (part->materialName[0] == '\0' && search->bindingCount > 0U)
        {
            stringAppend(part->materialName, RESOURCE_NAME_LIMIT,
                         search->bindings[0].materialName);
        }

        if (piece->indexCount > widestIndexCount)
        {
            widestIndexCount = piece->indexCount;
            widest = search->partCount;
        }
        search->partCount++;
    }

    if (search->partCount > 0U && search->parts[widest].materialName[0] != '\0')
    {
        search->materialName[0] = '\0';
        stringAppend(search->materialName, RESOURCE_NAME_LIMIT, search->parts[widest].materialName);
    }
}

/* Moves the mesh to where its node says it belongs.
 *
 * A part's vertices are written relative to whatever it hangs from, and where
 * it hangs from is the transform tree: a head sits at the neck's transform,
 * which sits at the spine's, and so on to the root. A mesh drawn without that
 * is drawn at the origin, which for a single part model is invisibly wrong and
 * for anything assembled is a pile.
 *
 * The composer has existed since the tree reader was written and has never
 * been used for anything. This is its first caller, so the first thing worth
 * knowing is whether it moves anything at all — an identity transform is the
 * right answer for a model whose one node is its root, and the wrong answer for
 * a Sim's head.
 *
 * Positions carry the whole transform; normals carry only its rotation, since
 * translating a direction turns it into a point somewhere else. */
static void placePartByItsNode(DiscContentSearch *search)
{
    static const Real32 identity[16] = { 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                         0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F };
    Real32 matrix[16];
    Unsigned32 index;

    search->partWasMoved = BOOLEAN_FALSE;
    if (!search->modelHasTree)
    {
        return;
    }
    resourceNodeGetWorldTransform(&search->modelTree, search->modelNodeIndex, matrix);

    for (index = 0U; index < 16U; index++)
    {
        if (matrix[index] != identity[index])
        {
            search->partWasMoved = BOOLEAN_TRUE;
            break;
        }
    }
    if (!search->partWasMoved)
    {
        return;
    }
    geometryMeshApplyTransform(&search->mesh, matrix);
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
        /* Nothing skinned among the game's own meshes, but something was there.
           Back to the first model that was passed over for being rigid, which
           is the one this would have taken before it started asking.
         *
           Its file index rather than its bytes: holding the model would mean
           holding a whole package's allocation underneath every later attempt,
           and the arena is a stack. One package is cheaper to read twice than
           to keep. */
        if (search->wantingSkinned && search->rigidModelFound)
        {
            search->wantingSkinned = BOOLEAN_FALSE;
            search->nextIndex = search->rigidModelIndex;
            return DISC_CONTENT_PENDING;
        }
        if (!search->walkingPreferred)
        {
            return DISC_CONTENT_NONE_FOUND;
        }
        /* Nothing among the game's own meshes. Round again over everything,
           which is where this always looked and is still better than nothing. */
        search->walkingPreferred = BOOLEAN_FALSE;
        search->wantingSkinned = BOOLEAN_FALSE;
        search->nextIndex = 0U;
        return DISC_CONTENT_PENDING;
    }

    entry = virtualFileSystemGetEntry(search->fileSystem, search->nextIndex);
    if (entry == NULL_POINTER || !endsWithPackage(entry->path) ||
        entry->sizeInBytes > (Unsigned64)LARGEST_PACKAGE_BYTES || entry->sizeInBytes == 0U)
    {
        search->nextIndex++;
        return DISC_CONTENT_PENDING;
    }
    /* On the first round, only the directory the game keeps its character
       meshes in. On the second, anything — including that directory again,
       which costs one wasted pass over packages that already failed and saves
       a second flag to remember that they did. */
    if (search->walkingPreferred &&
        (!stringContainsIgnoringCase(entry->path, PREFERRED_DIRECTORY) ||
         stringContainsIgnoringCase(entry->path, PREFERRED_EXCLUDES)))
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

    /* A readable model that is welded to one joint rather than weighted across
     * several. Noted and walked past on the first round.
     *
     * The first package on this disc that yields a model is a face, and a face
     * is rigid — so the search kept arriving somewhere reasonable and never
     * anywhere with a skeleton to apply. Which package holds a body is not
     * something to guess from a name; whether a mesh carries bone assignments
     * is something the mesh itself answers. */
    if (search->wantingSkinned && search->mesh.boneAssignments == NULL_POINTER)
    {
        if (!search->rigidModelFound)
        {
            search->rigidModelFound = BOOLEAN_TRUE;
            search->rigidModelIndex = search->nextIndex - 1U;
        }
        search->rigidModelsPassed++;
        /* Bounded, because every one of these is a package read and on the web
           a package read is a round trip. A disc with no skinned mesh in it at
           all would otherwise walk the whole preferred set before admitting it,
           and the walk is the expensive part of loading.
         *
           At the limit it takes the model in hand rather than going back for
           the first one: that one would have to be read again, and this one is
           already here. */
        if (search->rigidModelsPassed < RIGID_MODELS_TO_WALK_PAST)
        {
            memoryArenaRewindToMarker(search->arena, attemptMarker);
            return DISC_CONTENT_PENDING;
        }
        search->wantingSkinned = BOOLEAN_FALSE;
    }

    placePartByItsNode(search);
    chooseMaterialPerPrimitive(search);
    findTextureForMaterial(search, &package);

    search->foundInPreferred = search->walkingPreferred;
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
