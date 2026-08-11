#include "victoria/discContent.h"

#include "utils/resourceHash.h"
#include "utils/strings.h"
#include "victoria/compression.h"
#include "victoria/freestandingRuntime.h"
#include "victoria/packageReader.h"

#define LARGEST_PACKAGE_BYTES (24UL * 1024UL * 1024UL)

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
    search->walkingPreferred = BOOLEAN_FALSE;
    search->wantingSkinned = BOOLEAN_FALSE;
}

#define PREFERRED_DIRECTORY "Sims3D"

#define PREFERRED_EXCLUDES "Locale"

#define RIGID_MODELS_TO_WALK_PAST 48U

static Boolean endsWithPackage(const char *path)
{
    return stringEndsWithIgnoringCase(path, ".package");
}

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
    memoryArenaRewindToMarker(search->arena, marker);
    return BOOLEAN_TRUE;
}

static void rememberPart(DiscContentSearch *search, const ShapeDescription *shape,
                         Unsigned32 meshIndex, Unsigned32 nodeIndex)
{
    DiscModelPart *part;
    Unsigned32 which;

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
    if (part->materialName[0] == '\0' && shape->storedMaterialCount > 0U)
    {
        stringAppend(part->materialName, RESOURCE_NAME_LIMIT, shape->materials[0].materialName);
    }
    search->partCount++;
}

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
        if (shape.storedMaterialCount > 0U)
        {
            stringAppend(search->materialName, RESOURCE_NAME_LIMIT,
                         shape.materials[0].materialName);
        }
    }
    memoryArenaRewindToMarker(search->arena, marker);
    return geometry;
}

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

static void poseByTheSkeleton(DiscContentSearch *search)
{
    Unsigned32 nodeCount;
    Unsigned32 index;

    search->verticesPosed = 0U;
    search->bonesInPalette = 0U;
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
    search->bonesInPalette = 0U;

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

    if (!animationTargetsTree(&search->modelTree, animation))
    {
        return BOOLEAN_FALSE;
    }
    search->channelsApplied = countChannelsReachingTree(&search->modelTree, animation);
    if (search->channelsApplied == 0U)
    {
        return BOOLEAN_FALSE;
    }

    if (!restoreBindPose(search))
    {
        return BOOLEAN_FALSE;
    }

    search->verticesDeformed = 0U;
    if (search->morphWeights != NULL_POINTER && search->morphWeightCount > 0U)
    {
        search->verticesDeformed = geometryMeshApplyMorph(&search->mesh, search->morphWeights,
                                                          search->morphWeightCount);
    }

    boneCount = search->mesh.bindPoseCount;
    palette = (Real32 *)memoryArenaAllocate(arena, (MemorySize)boneCount * 16UL * sizeof(Real32),
                                            sizeof(Real32));
    if (palette == NULL_POINTER)
    {
        return BOOLEAN_FALSE;
    }

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
        resourceNodeMultiplyTransforms(world, bind, &palette[index * 16U]);
        search->bonesPosed++;
    }

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
            (stringEquals(material.resourceName, wanted) ||
             stringEquals(material.materialName, search->materialName)))
        {
            search->materialFound = BOOLEAN_TRUE;
        }
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

    if (search->limitedToOneFile && search->nextIndex != search->onlyFileIndex)
    {
        return DISC_CONTENT_NONE_FOUND;
    }

    if (search->nextIndex >= search->fileSystem->entryCount)
    {
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
    if (search->walkingPreferred &&
        (!stringContainsIgnoringCase(entry->path, PREFERRED_DIRECTORY) ||
         stringContainsIgnoringCase(entry->path, PREFERRED_EXCLUDES)))
    {
        search->nextIndex++;
        return DISC_CONTENT_PENDING;
    }

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

    if (search->wantingSkinned && search->mesh.boneAssignments == NULL_POINTER)
    {
        if (!search->rigidModelFound)
        {
            search->rigidModelFound = BOOLEAN_TRUE;
            search->rigidModelIndex = search->nextIndex - 1U;
        }
        search->rigidModelsPassed++;
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
