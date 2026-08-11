#ifndef VICTORIA_DISC_CONTENT_HEADER
#define VICTORIA_DISC_CONTENT_HEADER

#include "victoria/animationReader.h"
#include "victoria/coreTypes.h"
#include "victoria/geometryReader.h"
#include "victoria/memoryArena.h"
#include "victoria/material.h"
#include "victoria/resourceNode.h"
#include "victoria/textureReader.h"
#include "victoria/scenegraph.h"
#include "victoria/virtualFileSystem.h"

#define DISC_CONTENT_PATH_LIMIT 320UL

#define DISC_CONTENT_VERSION_BUCKETS 16U

typedef enum DiscContentStatus
{
    DISC_CONTENT_FOUND = 0,
    DISC_CONTENT_PENDING,
    DISC_CONTENT_NONE_FOUND,
    DISC_CONTENT_OUT_OF_ARENA
} DiscContentStatus;

const char *discContentStatusGetName(DiscContentStatus status);

#define DISC_CONTENT_PART_LIMIT 32U

#define DISC_CONTENT_BONE_SAMPLE 6U

typedef struct DiscModelPart
{
    char meshName[RESOURCE_NAME_LIMIT];
    char materialName[RESOURCE_NAME_LIMIT];
    char shapeName[RESOURCE_NAME_LIMIT];
    Unsigned32 nodeIndex;
    Unsigned32 levelOfDetail;
    Unsigned32 firstIndex;
    Unsigned32 indexCount;
} DiscModelPart;

typedef struct DiscContentBoneReport
{
    char nodeName[RESOURCE_NAME_LIMIT];
    Unsigned32 chainLength;
    Unsigned32 chainNamed;
    Unsigned32 chainApplied;
    Boolean anySkipped;
    char skippedNode[RESOURCE_NAME_LIMIT];
    Unsigned32 skippedType;
    Unsigned32 skippedAttribute;
    Unsigned32 skippedComponents;
} DiscContentBoneReport;

typedef struct DiscContentSearch
{
    VirtualFileSystem *fileSystem;
    MemoryArena *arena;
    MemorySize arenaMarker;
    Unsigned32 nextIndex;
    Boolean walkingPreferred;
    Boolean foundInPreferred;

    Boolean wantingSkinned;
    Boolean rigidModelFound;
    Unsigned32 rigidModelIndex;
    Unsigned32 rigidModelsPassed;

    Boolean limitedToOneFile;
    Unsigned32 onlyFileIndex;

    Unsigned32 verticesPosed;
    Unsigned32 bonesInPalette;
    Unsigned32 firstBoneNames[DISC_CONTENT_BONE_SAMPLE];
    char firstBoneNodeNames[DISC_CONTENT_BONE_SAMPLE][RESOURCE_NAME_LIMIT];
    Unsigned32 firstBoneNameCount;

    Unsigned32 bonesMatchedToANode;
    Unsigned32 bonesWithoutANode;

    Real32 bindPoseFromIdentity;
    Real32 bindPoseFromWorld;
    Unsigned32 bonesMeasured;

    Unsigned32 channelsApplied;
    Unsigned32 bonesPosed;
    Real32 poseShift;
    Real32 poseSpan;

    DiscContentBoneReport boneReports[DISC_CONTENT_BONE_SAMPLE];
    Unsigned32 boneReportCount;

    Real32 *bindPositions;
    Real32 *bindNormals;
    Unsigned32 bindVertexCount;

    char packagePath[DISC_CONTENT_PATH_LIMIT];
    GeometryMesh mesh;
    char modelName[RESOURCE_NAME_LIMIT];
    Boolean foundThroughScenegraph;
    ResourceNodeDescription modelTree;
    Boolean modelHasTree;
    Unsigned32 modelNodeIndex;
    Boolean partWasMoved;

    Unsigned32 partCount;
    Unsigned32 partsBeyondRoom;
    Unsigned32 bindingCount;
    ShapeMaterialBinding bindings[SCENEGRAPH_MATERIAL_LIMIT];

    Unsigned32 shapeReferences;
    Unsigned32 shapeReferencesResolved;
    Unsigned32 coarserPartsDropped;
    DiscModelPart parts[DISC_CONTENT_PART_LIMIT];

    char materialName[RESOURCE_NAME_LIMIT];
    Boolean materialFound;
    Unsigned32 materialsInPackage;
    Unsigned32 texturesInPackage;
    char textureName[RESOURCE_NAME_LIMIT];
    Boolean textureFound;
    TextureDescription texture;

    Unsigned32 packagesOpened;
    Unsigned32 packagesCompressed;
    Unsigned32 packagesWithGeometry;
    Unsigned32 packagesWithShapes;
    Unsigned32 packagesWithTrees;
    Unsigned32 modelsResolved;
    const Real32 *morphWeights;
    Unsigned32 morphWeightCount;
    Unsigned32 verticesDeformed;

    Unsigned32 geometryRefused;
    Unsigned32 refusalsByReason[GEOMETRY_READ_RESULT_COUNT];
    MemorySize largestArenaWant;
    Unsigned32 decompressionRefused;

    Unsigned32 versionsSeen[DISC_CONTENT_VERSION_BUCKETS];
    Boolean sawUnknownMark;
    Unsigned32 firstUnknownMark;

    Unsigned32 largestElementCount;
} DiscContentSearch;

void discContentBegin(DiscContentSearch *search, VirtualFileSystem *fileSystem, MemoryArena *arena);

void discContentBeginInFile(DiscContentSearch *search, VirtualFileSystem *fileSystem,
                            MemoryArena *arena, Unsigned32 fileIndex);

DiscContentStatus discContentStep(DiscContentSearch *search);

DiscContentStatus discContentRunToCompletion(DiscContentSearch *search);

Boolean discContentPoseFromAnimation(DiscContentSearch *search, const Animation *animation,
                                     Real32 tick, MemoryArena *arena);

Boolean discContentKeepBindPose(DiscContentSearch *search, MemoryArena *arena);

typedef enum DiscModelResult
{
    DISC_MODEL_OK = 0,
    DISC_MODEL_NO_TREE,
    DISC_MODEL_TREE_UNREADABLE,
    DISC_MODEL_NO_SHAPE_NODE,
    DISC_MODEL_SHAPE_NOT_IN_PACKAGE,
    DISC_MODEL_SHAPE_UNREADABLE,
    DISC_MODEL_NO_GEOMETRY_NAMED,
    DISC_MODEL_GEOMETRY_UNREADABLE
} DiscModelResult;

const char *discModelResultGetName(DiscModelResult result);

DiscModelResult discContentReadNamedModel(MemoryArena *arena, const Package *package,
                                          const char *resourceName, GeometryMesh *mesh,
                                          char *materialName, MemorySize materialCapacity);

#endif
