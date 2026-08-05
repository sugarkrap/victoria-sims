#include "victoria/discContent.h"

#include "utils/resourceHash.h"
#include "utils/strings.h"
#include "victoria/compression.h"
#include "victoria/freestandingRuntime.h"
#include "victoria/packageReader.h"

/* Files larger than this are not opened looking for geometry. A retail
 * objects.package is forty megabytes and holds no mesh worth drawing on its
 * own; the ceiling keeps a search from spending the whole budget on one file it
 * was never going to use. */
#define LARGEST_PACKAGE_BYTES (24UL * 1024UL * 1024UL)

/* The engine links no maths library — see the allocation and dependency rules —
   so the one thing a matrix comparison needs is spelled out here. */
static Real32 absoluteValue(Real32 value)
{
    return (value < 0.0f) ? -value : value;
}

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
    search->limitedToOneFile = BOOLEAN_FALSE;
    search->onlyFileIndex = 0U;
    search->verticesPosed = 0U;
    search->bonesInPalette = 0U;
    search->firstBoneNameCount = 0U;
    search->bonesMatchedToANode = 0U;
    search->bonesWithoutANode = 0U;
    search->bindPoseFromIdentity = 0.0f;
    search->bindPoseFromWorld = 0.0f;
    search->bonesMeasured = 0U;
    search->channelsApplied = 0U;
    search->bonesPosed = 0U;
    search->poseShift = 0.0f;
    search->poseSpan = 0.0f;
    search->boneReportCount = 0U;
    search->bindPositions = NULL_POINTER;
    search->bindNormals = NULL_POINTER;
    search->bindVertexCount = 0U;
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
    search->morphWeights = NULL_POINTER;
    search->morphWeightCount = 0U;
    search->verticesDeformed = 0U;
    search->geometryRefused = 0U;
    search->largestArenaWant = 0UL;
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

void discContentBeginInFile(DiscContentSearch *search, VirtualFileSystem *fileSystem,
                            MemoryArena *arena, Unsigned32 fileIndex)
{
    discContentBegin(search, fileSystem, arena);
    search->limitedToOneFile = BOOLEAN_TRUE;
    search->onlyFileIndex = fileIndex;
    search->nextIndex = fileIndex;
    /* Neither round applies: the caller is not asking this to look, it is
       telling it where to look. Asking for a skinned model here as well would
       let the search reject the one package it was pointed at. */
    search->walkingPreferred = BOOLEAN_FALSE;
    search->wantingSkinned = BOOLEAN_FALSE;
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

/* Notes what the mesh is weighted to, and deliberately does not move it.
 *
 * Applying the skeleton to a mesh at rest is a mistake, and it took drawing a
 * Sim's face stretched into a spike to see it. Skinning is
 *
 *     v' = sum over bones of weight * bone's world transform * inverse bind * v
 *
 * and the inverse bind is the bone's world transform at the pose the mesh was
 * authored in. The mesh on the disc IS in that pose, so at rest every bone's
 * pair multiplies out to the identity and correct skinning moves nothing at
 * all. Applying only the world transform — which is what this did — transforms
 * vertices that are already in world space a second time.
 *
 * The proof was already on screen: the face drew correctly for several runs
 * with no skinning applied whatsoever.
 *
 * So nothing is applied here. The weights, the assignments and the bone lists
 * are all read and kept, and geometryMeshApplySkin stands ready — they become
 * useful the moment an animation supplies bone transforms that are not the
 * bind pose, which is the only time skinning has anything to say. */
static void poseByTheSkeleton(DiscContentSearch *search)
{
    Unsigned32 nodeCount;
    Unsigned32 index;

    search->verticesPosed = 0U;
    search->bonesInPalette = 0U;
    /* Cleared here as well as at the start of a search: the redirect that finds
       a skinned model calls this a second time, and counters left standing
       would report the rigid model's bones alongside the skinned one's. */
    search->bonesMatchedToANode = 0U;
    search->bonesWithoutANode = 0U;
    search->bindPoseFromIdentity = 0.0f;
    search->bindPoseFromWorld = 0.0f;
    search->bonesMeasured = 0U;
    search->firstBoneNameCount = 0U;
    if (search->mesh.boneAssignments == NULL_POINTER || !search->modelHasTree)
    {
        return;
    }
    nodeCount = search->modelTree.storedNodeCount;
    if (nodeCount == 0U)
    {
        return;
    }
    /* Filled in by the walk below. It counts the bones the primitives actually
       name, which is not the size of the tree: this face names three of a
       hundred and twenty six nodes, and reporting the tree's size as the number
       of bones it is weighted to overstated it by two orders of magnitude. */
    search->bonesInPalette = 0U;

    /* Every bone the primitives name, resolved and then measured.
     *
     * A bone number is an identifier a node carries, not a position in the node
     * list — openTS2 keys its own lookup by TransformNode.BoneId — so these are
     * searched for rather than indexed by. The two agree often enough that
     * indexing would look right on this disc's face and be wrong on the next
     * tree along.
     *
     * The measurement settles what the container's bind pose actually is. At
     * rest the mesh is already in the pose its bones were measured in, so:
     *
     *   - if the stored transform is the INVERSE bind, world * stored is the
     *     identity, and a palette needs no matrix inverse at all;
     *   - if it is the FORWARD bind, stored is instead equal to world, and the
     *     palette has to invert it.
     *
     * Both are computed, because a single number that is merely large says only
     * that something is wrong and not which of the two it is. */
    for (index = 0U; index < search->mesh.storedPrimitiveCount; index++)
    {
        const GeometryPrimitive *primitive = &search->mesh.primitives[index];
        Unsigned32 inner;

        for (inner = 0U; inner < primitive->boneRemapCount; inner++)
        {
            Unsigned32 bone = primitive->boneRemap[inner];
            Integer32 node = resourceNodeFindByBoneIdentifier(&search->modelTree, bone);

            search->bonesInPalette++;
            if (search->firstBoneNameCount < DISC_CONTENT_BONE_SAMPLE)
            {
                char *slot = search->firstBoneNodeNames[search->firstBoneNameCount];

                search->firstBoneNames[search->firstBoneNameCount] = bone;
                slot[0] = '\0';
                stringAppend(slot, RESOURCE_NAME_LIMIT,
                             (node >= 0) ? search->modelTree.nodes[node].name : "no node");
                search->firstBoneNameCount++;
            }
            if (node < 0)
            {
                search->bonesWithoutANode++;
                continue;
            }
            search->bonesMatchedToANode++;
            if (bone < search->mesh.bindPoseCount)
            {
                Real32 world[16];
                Real32 stored[16];
                Real32 product[16];
                Unsigned32 cell;

                resourceNodeGetWorldTransform(&search->modelTree, (Unsigned32)node, world);
                resourceNodeBuildTransform(search->mesh.bindPoses[bone].rotation,
                                           search->mesh.bindPoses[bone].translation, stored);
                resourceNodeMultiplyTransforms(world, stored, product);
                for (cell = 0U; cell < 16U; cell++)
                {
                    Real32 identityCell = ((cell % 5U) == 0U) ? 1.0f : 0.0f;
                    Real32 fromIdentity = absoluteValue(product[cell] - identityCell);
                    Real32 fromWorld = absoluteValue(stored[cell] - world[cell]);

                    if (fromIdentity > search->bindPoseFromIdentity)
                    {
                        search->bindPoseFromIdentity = fromIdentity;
                    }
                    if (fromWorld > search->bindPoseFromWorld)
                    {
                        search->bindPoseFromWorld = fromWorld;
                    }
                }
                search->bonesMeasured++;
            }
        }
    }
}

/* The rotation an Euler channel names, in the order the game composes it.
 *
 * Ported from the format's own conversion rather than derived: the three angles
 * are in degrees, and which axis is applied first is not something that can be
 * inferred from a resting pose, where every angle is nought and every order
 * agrees. */
static void eulerDegreesToQuaternion(Real32 x, Real32 y, Real32 z, Real32 *rotation)
{
    const Real32 halfDegreesToRadians = 0.00872664625f;
    Real32 sinX = mathSine(x * halfDegreesToRadians);
    Real32 cosX = mathCosine(x * halfDegreesToRadians);
    Real32 sinY = mathSine(y * halfDegreesToRadians);
    Real32 cosY = mathCosine(y * halfDegreesToRadians);
    Real32 sinZ = mathSine(z * halfDegreesToRadians);
    Real32 cosZ = mathCosine(z * halfDegreesToRadians);

    rotation[0] = (cosZ * (cosY * sinX)) - (sinZ * (cosX * sinY));
    rotation[1] = (cosZ * (cosX * sinY)) + (sinZ * (cosY * sinX));
    rotation[2] = (sinZ * (cosX * cosY)) - (cosZ * (sinX * sinY));
    rotation[3] = (cosZ * (cosX * cosY)) + (sinZ * (sinX * sinY));
}

/* Whether buildAnimatedLocal would actually act on this channel. Shared with
   the report below rather than restated there: a report that guessed at the
   condition would drift the moment either changed, and the whole point of it is
   to say what the poser really did. */
static Boolean channelIsApplied(const AnimationChannel *channel)
{
    if (channel->componentCount < 3U)
    {
        return BOOLEAN_FALSE;
    }
    return (channel->type == ANIMATION_CHANNEL_EULER_ROTATION ||
            channel->type == ANIMATION_CHANNEL_TRANSFORM_XYZ)
               ? BOOLEAN_TRUE
               : BOOLEAN_FALSE;
}

/* One node's local transform with whatever the animation says about it applied.
 *
 * A bone can be named by more than one channel — a rotation and a translation
 * are separate channels sharing a name — so every channel is offered the node
 * rather than the first one found being taken. What no channel mentions keeps
 * what the tree gave it, which is what leaves an unanimated bone where it
 * belongs instead of collapsing it onto the origin. */
static Unsigned32 buildAnimatedLocal(const ResourceNodeDescription *tree, const Animation *animation,
                                     Unsigned32 nodeIndex, Real32 tick, Real32 *matrix)
{
    const TransformNode *node = &tree->nodes[nodeIndex];
    Real32 rotation[4];
    Real32 translation[3];
    Unsigned32 applied = 0U;
    Unsigned32 index;
    Unsigned32 axis;

    for (axis = 0U; axis < 4U; axis++)
    {
        rotation[axis] = node->rotation[axis];
    }
    for (axis = 0U; axis < 3U; axis++)
    {
        translation[axis] = node->translation[axis];
    }

    for (index = 0U; index < animation->channelCount; index++)
    {
        const AnimationChannel *channel = &animation->channels[index];

        if (!stringEqualsIgnoringCase(channel->name, node->name))
        {
            continue;
        }
        if (!channelIsApplied(channel))
        {
            continue;
        }
        if (channel->type == ANIMATION_CHANNEL_EULER_ROTATION)
        {
            eulerDegreesToQuaternion(animationComponentSample(&channel->components[0], tick),
                                     animationComponentSample(&channel->components[1], tick),
                                     animationComponentSample(&channel->components[2], tick),
                                     rotation);
            applied++;
        }
        else
        {
            for (axis = 0U; axis < 3U; axis++)
            {
                translation[axis] = animationComponentSample(&channel->components[axis], tick);
            }
            applied++;
        }
    }

    resourceNodeBuildTransform(rotation, translation, matrix);
    return applied;
}

/* The same walk up the parents as resourceNodeGetWorldTransform, but composing
   animated locals rather than the ones the tree stored. It cannot call that
   function for the parents: a parent that the animation moves has to move its
   children too, and taking the stored transform for it would leave a posed arm
   hanging off a shoulder still in its bind pose. */
static void buildAnimatedWorld(const ResourceNodeDescription *tree, const Animation *animation,
                               Unsigned32 nodeIndex, Real32 tick, Real32 *matrix)
{
    Real32 accumulated[16];
    Real32 scratch[16];
    Integer32 current;
    Unsigned32 guard = 0U;
    Unsigned32 index;

    (void)buildAnimatedLocal(tree, animation, nodeIndex, tick, accumulated);
    current = tree->nodes[nodeIndex].parentIndex;
    while (current >= 0 && (Unsigned32)current < tree->storedNodeCount && guard < tree->storedNodeCount)
    {
        Real32 parentMatrix[16];

        (void)buildAnimatedLocal(tree, animation, (Unsigned32)current, tick, parentMatrix);
        resourceNodeMultiplyTransforms(parentMatrix, accumulated, scratch);
        for (index = 0U; index < 16U; index++)
        {
            accumulated[index] = scratch[index];
        }
        current = tree->nodes[current].parentIndex;
        guard++;
    }
    for (index = 0U; index < 16U; index++)
    {
        matrix[index] = accumulated[index];
    }
}

/* How many of the animation's channels name a node of this tree.
 *
 * Counted over the animation once, not accumulated as the palette is built.
 * The version that added up what each bone's walk to the root applied counted
 * a channel again for every bone whose chain passed through it, and reported
 * two channels as a hundred and twenty three. */
static Unsigned32 countChannelsReachingTree(const ResourceNodeDescription *tree,
                                            const Animation *animation)
{
    Unsigned32 reaching = 0U;
    Unsigned32 index;

    for (index = 0U; index < animation->channelCount; index++)
    {
        Unsigned32 node;

        for (node = 0U; node < tree->storedNodeCount; node++)
        {
            if (stringEqualsIgnoringCase(animation->channels[index].name, tree->nodes[node].name))
            {
                reaching++;
                break;
            }
        }
    }
    return reaching;
}

/* Whether the animation was authored against this model's skeleton.
 *
 * The tag names a node the skeleton is expected to have — "auskel" for a Sim —
 * so this asks the tree for it rather than comparing against a list of names
 * written down here. Without the check any animation sharing a single node name
 * is accepted: a birthday cake box's carry animation drove a Sim's face,
 * because both trees happen to contain a node the other also has. */
static Boolean animationTargetsTree(const ResourceNodeDescription *tree, const Animation *animation)
{
    Unsigned32 node;

    if (animation->skeletonTag[0] == '\0')
    {
        return BOOLEAN_FALSE;
    }
    for (node = 0U; node < tree->storedNodeCount; node++)
    {
        if (stringEqualsIgnoringCase(animation->skeletonTag, tree->nodes[node].name))
        {
            return BOOLEAN_TRUE;
        }
    }
    return BOOLEAN_FALSE;
}

/* Walks one bone's chain to the root, counting what the animation named against
   what was applied. */
static void describeBoneChain(const ResourceNodeDescription *tree, const Animation *animation,
                              Unsigned32 nodeIndex, DiscContentBoneReport *report)
{
    Integer32 current = (Integer32)nodeIndex;
    Unsigned32 guard = 0U;

    report->nodeName[0] = '\0';
    stringAppend(report->nodeName, RESOURCE_NAME_LIMIT, tree->nodes[nodeIndex].name);
    report->chainLength = 0U;
    report->chainNamed = 0U;
    report->chainApplied = 0U;
    report->anySkipped = BOOLEAN_FALSE;
    report->skippedNode[0] = '\0';
    report->skippedType = 0U;
    report->skippedAttribute = 0U;
    report->skippedComponents = 0U;

    while (current >= 0 && (Unsigned32)current < tree->storedNodeCount &&
           guard < tree->storedNodeCount)
    {
        const TransformNode *node = &tree->nodes[current];
        Boolean named = BOOLEAN_FALSE;
        Boolean applied = BOOLEAN_FALSE;
        Unsigned32 index;

        for (index = 0U; index < animation->channelCount; index++)
        {
            const AnimationChannel *channel = &animation->channels[index];

            if (!stringEqualsIgnoringCase(channel->name, node->name))
            {
                continue;
            }
            named = BOOLEAN_TRUE;
            if (channelIsApplied(channel))
            {
                applied = BOOLEAN_TRUE;
            }
            else if (!report->anySkipped)
            {
                report->anySkipped = BOOLEAN_TRUE;
                report->skippedNode[0] = '\0';
                stringAppend(report->skippedNode, RESOURCE_NAME_LIMIT, node->name);
                report->skippedType = (Unsigned32)channel->type;
                report->skippedAttribute = (Unsigned32)channel->attribute;
                report->skippedComponents = channel->componentCount;
            }
        }
        report->chainLength++;
        if (named)
        {
            report->chainNamed++;
        }
        if (applied)
        {
            report->chainApplied++;
        }
        current = node->parentIndex;
        guard++;
    }
}

const char *discModelResultGetName(DiscModelResult result)
{
    switch (result)
    {
    case DISC_MODEL_OK:
        return "read";
    case DISC_MODEL_NO_TREE:
        return "no tree of that name in this package";
    case DISC_MODEL_TREE_UNREADABLE:
        return "its tree would not read";
    case DISC_MODEL_NO_SHAPE_NODE:
        return "its tree names no shape";
    case DISC_MODEL_SHAPE_NOT_IN_PACKAGE:
        return "the shape it names is not in this package";
    case DISC_MODEL_SHAPE_UNREADABLE:
        return "its shape would not read";
    case DISC_MODEL_NO_GEOMETRY_NAMED:
        return "no container matched the mesh its shape names";
    case DISC_MODEL_GEOMETRY_UNREADABLE:
        return "its container would not read";
    default:
        return "an unnamed result";
    }
}

DiscModelResult discContentReadNamedModel(MemoryArena *arena, const Package *package,
                                          const char *resourceName, GeometryMesh *mesh,
                                          char *materialName, MemorySize materialCapacity)
{
    const PackageResource *nodeResource;
    const PackageResource *shapeResource = NULL_POINTER;
    const PackageResource *geometry;
    const Unsigned8 *bytes;
    MemorySize size;
    Boolean compressed;
    /* On the stack rather than in the search: this is a tree read to find one
       shape reference in it, and nothing after wants it. A Sim's parts each
       carry a hundred and eighteen nodes, so it is not small — but the web
       build's stack is the reason the search keeps its own, and this runs on
       the load path where that stack is not the constraint. */
    static ResourceNodeDescription tree;
    ShapeDescription shape;
    Unsigned32 index;

    if (materialName != NULL_POINTER && materialCapacity > 0UL)
    {
        materialName[0] = '\0';
    }

    nodeResource = scenegraphFindResourceByInstance(package, (Unsigned32)PACKAGE_TYPE_CRES,
                                                    resourceHashInstance(resourceName),
                                                    resourceHashInstanceHigh(resourceName));
    if (nodeResource == NULL_POINTER)
    {
        return DISC_MODEL_NO_TREE;
    }

    bytes = scenegraphReadResourceBytes(arena, package, nodeResource, &size, &compressed);
    if (bytes == NULL_POINTER || resourceNodeRead(&tree, bytes, size) != RESOURCE_NODE_OK)
    {
        return DISC_MODEL_TREE_UNREADABLE;
    }

    /* The first node that references a shape. A Sim's part names exactly one,
       which the probe established before any of this was written. */
    {
        Unsigned32 shapeNodes = 0U;

        for (index = 0U; index < tree.storedNodeCount; index++)
        {
            if (!tree.nodes[index].hasShape)
            {
                continue;
            }
            shapeNodes++;
            shapeResource = scenegraphFindResource(package, &tree.nodes[index].shapeKey);
            if (shapeResource == NULL_POINTER)
            {
                /* The key carries a group, and a shape filed under another one
                   is the same shape. Tried by instance alone before giving up,
                   because a name identifies a resource and a group does not. */
                shapeResource = scenegraphFindResourceByInstance(
                    package, (Unsigned32)PACKAGE_TYPE_SHPE,
                    tree.nodes[index].shapeKey.instanceIdentifier,
                    tree.nodes[index].shapeKey.instanceIdentifierHigh);
            }
            if (shapeResource != NULL_POINTER)
            {
                break;
            }
        }
        if (shapeResource == NULL_POINTER)
        {
            return (shapeNodes == 0U) ? DISC_MODEL_NO_SHAPE_NODE : DISC_MODEL_SHAPE_NOT_IN_PACKAGE;
        }
    }

    bytes = scenegraphReadResourceBytes(arena, package, shapeResource, &size, &compressed);
    if (bytes == NULL_POINTER || scenegraphReadShape(&shape, bytes, size) != SCENEGRAPH_READ_OK)
    {
        return DISC_MODEL_SHAPE_UNREADABLE;
    }
    if (materialName != NULL_POINTER && shape.storedMaterialCount > 0U)
    {
        stringAppend(materialName, materialCapacity, shape.materials[0].materialName);
    }

    /* The first mesh the shape names that resolves to a container. */
    geometry = NULL_POINTER;
    for (index = 0U; index < shape.storedMeshCount && geometry == NULL_POINTER; index++)
    {
        if (shape.meshNames[index][0] == '\0')
        {
            continue;
        }
        geometry = scenegraphFindGeometryNamed(arena, package, shape.meshNames[index]);
    }
    if (geometry == NULL_POINTER)
    {
        return DISC_MODEL_NO_GEOMETRY_NAMED;
    }

    bytes = scenegraphReadResourceBytes(arena, package, geometry, &size, &compressed);
    if (bytes == NULL_POINTER)
    {
        return DISC_MODEL_GEOMETRY_UNREADABLE;
    }
    return (geometryReaderOpen(mesh, bytes, size, arena) == GEOMETRY_READ_OK)
               ? DISC_MODEL_OK
               : DISC_MODEL_GEOMETRY_UNREADABLE;
}

Boolean discContentKeepBindPose(DiscContentSearch *search, MemoryArena *arena)
{
    MemorySize count = (MemorySize)search->mesh.vertexCount * 3UL;
    Unsigned32 index;

    search->bindPositions = NULL_POINTER;
    search->bindNormals = NULL_POINTER;
    search->bindVertexCount = 0U;
    if (search->mesh.positions == NULL_POINTER || count == 0UL)
    {
        return BOOLEAN_FALSE;
    }

    search->bindPositions =
        (Real32 *)memoryArenaAllocate(arena, count * sizeof(Real32), sizeof(Real32));
    if (search->bindPositions == NULL_POINTER)
    {
        return BOOLEAN_FALSE;
    }
    if (search->mesh.normals != NULL_POINTER)
    {
        search->bindNormals =
            (Real32 *)memoryArenaAllocate(arena, count * sizeof(Real32), sizeof(Real32));
        if (search->bindNormals == NULL_POINTER)
        {
            search->bindPositions = NULL_POINTER;
            return BOOLEAN_FALSE;
        }
    }
    for (index = 0U; index < (Unsigned32)count; index++)
    {
        search->bindPositions[index] = search->mesh.positions[index];
        if (search->bindNormals != NULL_POINTER)
        {
            search->bindNormals[index] = search->mesh.normals[index];
        }
    }
    search->bindVertexCount = search->mesh.vertexCount;
    return BOOLEAN_TRUE;
}

/* Puts the mesh back in the pose the container gave it, so the pose about to be
   built starts where the last one did rather than on top of it. */
static Boolean restoreBindPose(DiscContentSearch *search)
{
    Unsigned32 index;

    if (search->bindPositions == NULL_POINTER ||
        search->bindVertexCount != search->mesh.vertexCount)
    {
        return BOOLEAN_FALSE;
    }
    for (index = 0U; index < search->bindVertexCount * 3U; index++)
    {
        search->mesh.positions[index] = search->bindPositions[index];
        if (search->bindNormals != NULL_POINTER && search->mesh.normals != NULL_POINTER)
        {
            search->mesh.normals[index] = search->bindNormals[index];
        }
    }
    return BOOLEAN_TRUE;
}

Boolean discContentPoseFromAnimation(DiscContentSearch *search, const Animation *animation,
                                     Real32 tick, MemoryArena *arena)
{
    Real32 *palette;
    Unsigned32 boneCount;
    Unsigned32 index;

    search->channelsApplied = 0U;
    search->bonesPosed = 0U;
    search->verticesPosed = 0U;
    search->poseShift = 0.0f;
    search->poseSpan = 0.0f;
    search->boneReportCount = 0U;
    if (search->mesh.boneAssignments == NULL_POINTER || !search->modelHasTree ||
        search->mesh.bindPoses == NULL_POINTER || animation->channelCount == 0U)
    {
        return BOOLEAN_FALSE;
    }

    /* Refused before any matrix is built. An animation authored against another
       skeleton can still share a node name or two with this one, and posing by
       it moves those bones and leaves every other in its bind pose — which is
       not a pose of anything, only a model with one joint pulled out of it. */
    if (!animationTargetsTree(&search->modelTree, animation))
    {
        return BOOLEAN_FALSE;
    }
    search->channelsApplied = countChannelsReachingTree(&search->modelTree, animation);
    if (search->channelsApplied == 0U)
    {
        return BOOLEAN_FALSE;
    }

    /* Back to the pose the container gave, so this starts where the last one
       did rather than on top of it. */
    if (!restoreBindPose(search))
    {
        return BOOLEAN_FALSE;
    }

    /* Deformation goes here — after the mesh is back at rest and before a bone
     * touches it — and this is the only place it may go.
     *
     * A morph changes the shape the model was authored in. A fatter body is a
     * different resting mesh, not a differently posed one, so its deltas are in
     * rest space and belong on a resting mesh. Applied after the skin they
     * would be added to vertices that have already been carried off to wherever
     * their bones went, and the further a limb had swung the further out the
     * displacement would land. Between the restore and the palette is the one
     * window where the mesh is at rest and about to be posed. */
    search->verticesDeformed = 0U;
    if (search->morphWeights != NULL_POINTER && search->morphWeightCount > 0U)
    {
        search->verticesDeformed = geometryMeshApplyMorph(&search->mesh, search->morphWeights,
                                                          search->morphWeightCount);
    }

    /* One matrix per entry of the container's bind pose, because that array and
       the primitives' bone numbers share a numbering — so a bone number indexes
       both, and the palette can be indexed by it directly. */
    boneCount = search->mesh.bindPoseCount;
    palette = (Real32 *)memoryArenaAllocate(arena, (MemorySize)boneCount * 16UL * sizeof(Real32),
                                            sizeof(Real32));
    if (palette == NULL_POINTER)
    {
        return BOOLEAN_FALSE;
    }

    /* Every bone starts as the identity, which leaves any the animation does not
       reach exactly where the mesh already has them. A palette left uninitialised
       would send the vertices weighted to those bones somewhere arbitrary. */
    for (index = 0U; index < boneCount * 16U; index++)
    {
        palette[index] = ((index % 17U) == 0U) ? 1.0f : 0.0f;
    }

    for (index = 0U; index < boneCount; index++)
    {
        Integer32 node = resourceNodeFindByBoneIdentifier(&search->modelTree, index);
        Real32 world[16];
        Real32 bind[16];

        if (node < 0)
        {
            continue;
        }
        buildAnimatedWorld(&search->modelTree, animation, (Unsigned32)node, tick, world);
        resourceNodeBuildTransform(search->mesh.bindPoses[index].rotation,
                                   search->mesh.bindPoses[index].translation, bind);
        /* The pose times the inverse bind, which is what geometryMeshApplySkin
           requires and what the stored transform already is — measured, not
           assumed, on a resting mesh where the product came out the identity. */
        resourceNodeMultiplyTransforms(world, bind, &palette[index * 16U]);
        search->bonesPosed++;
    }

    /* The report, over the bones the primitives actually draw with rather than
       over every bone in the palette. Those are the only ones that can move
       this mesh, so they are the only ones whose coverage explains what is on
       screen. */
    {
        Unsigned32 which;

        for (which = 0U; which < search->firstBoneNameCount &&
                         search->boneReportCount < DISC_CONTENT_BONE_SAMPLE;
             which++)
        {
            Integer32 node =
                resourceNodeFindByBoneIdentifier(&search->modelTree, search->firstBoneNames[which]);

            if (node < 0)
            {
                continue;
            }
            describeBoneChain(&search->modelTree, animation, (Unsigned32)node,
                              &search->boneReports[search->boneReportCount]);
            search->boneReportCount++;
        }
    }

    /* How far the pose actually moved the model, measured as the largest change
     * in any of its six bounds.
     *
     * A count of vertices posed cannot tell a pose from a catastrophe: the
     * failure this project has already met once — a Sim's face with a limb
     * stretched out of it — moved every vertex too, and would report the same
     * number. A face lifted by a shoulder should shift by something on the
     * order of the model's own size; a number far larger than that is the
     * spike, and one of nought is a pose that did nothing. */
    {
        Real32 minimumBefore[3];
        Real32 maximumBefore[3];
        Real32 minimumAfter[3];
        Real32 maximumAfter[3];
        Unsigned32 axis;

        geometryMeshGetBounds(&search->mesh, minimumBefore, maximumBefore);
        search->verticesPosed = geometryMeshApplySkin(&search->mesh, palette, boneCount);
        geometryMeshGetBounds(&search->mesh, minimumAfter, maximumAfter);

        search->poseShift = 0.0f;
        search->poseSpan = 0.0f;
        for (axis = 0U; axis < 3U; axis++)
        {
            Real32 movedLow = absoluteValue(minimumAfter[axis] - minimumBefore[axis]);
            Real32 movedHigh = absoluteValue(maximumAfter[axis] - maximumBefore[axis]);
            Real32 span = maximumBefore[axis] - minimumBefore[axis];

            if (movedLow > search->poseShift)
            {
                search->poseShift = movedLow;
            }
            if (movedHigh > search->poseShift)
            {
                search->poseShift = movedHigh;
            }
            if (span > search->poseSpan)
            {
                search->poseSpan = span;
            }
        }
        return (search->verticesPosed > 0U) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
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

    /* Pointed at one package: it is either that one or nothing, and walking on
       past it would quietly answer a different question than the one asked. */
    if (search->limitedToOneFile && search->nextIndex != search->onlyFileIndex)
    {
        return DISC_CONTENT_NONE_FOUND;
    }

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
            if (search->mesh.arenaWantedBytes > search->largestArenaWant)
            {
                search->largestArenaWant = search->mesh.arenaWantedBytes;
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
    poseByTheSkeleton(search);
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
