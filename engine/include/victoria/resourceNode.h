#ifndef VICTORIA_RESOURCE_NODE_HEADER
#define VICTORIA_RESOURCE_NODE_HEADER

#include "victoria/coreTypes.h"
#include "victoria/packageReader.h"
#include "victoria/resourceCollection.h"

#define RESOURCE_NODE_LIMIT 128U
#define RESOURCE_NODE_EDGE_LIMIT 256U

typedef enum ResourceNodeResult
{
    RESOURCE_NODE_OK = 0,
    RESOURCE_NODE_NOT_A_RESOURCE,
    RESOURCE_NODE_OLDER_COLLECTION,
    RESOURCE_NODE_WRONG_TYPE,
    RESOURCE_NODE_TRUNCATED,
    RESOURCE_NODE_UNKNOWN_BLOCK
} ResourceNodeResult;

const char *resourceNodeResultGetName(ResourceNodeResult result);

typedef struct TransformNode
{
    char name[RESOURCE_NAME_LIMIT];
    Unsigned32 blockIndex;

    Real32 translation[3];
    Real32 rotation[4];
    Unsigned32 boneIdentifier;

    Integer32 parentIndex;

    Boolean hasShape;
    PackageResourceKey shapeKey;
} TransformNode;

typedef struct ResourceNodeDescription
{
    char resourceName[RESOURCE_NAME_LIMIT];
    Unsigned32 blockVersion;

    Unsigned32 blockCount;
    Unsigned32 blocksRead;

    Unsigned32 nodeCount;
    Unsigned32 storedNodeCount;
    TransformNode nodes[RESOURCE_NODE_LIMIT];
} ResourceNodeDescription;

ResourceNodeResult resourceNodeRead(ResourceNodeDescription *description, const Unsigned8 *bytes,
                                    MemorySize sizeInBytes);

void resourceNodeGetWorldTransform(const ResourceNodeDescription *description, Unsigned32 nodeIndex,
                                   Real32 *matrix);

void resourceNodeBuildTransform(const Real32 *rotation, const Real32 *translation, Real32 *matrix);

void resourceNodeMultiplyTransforms(const Real32 *left, const Real32 *right, Real32 *result);

Integer32 resourceNodeFindByBoneIdentifier(const ResourceNodeDescription *description,
                                           Unsigned32 boneIdentifier);

#endif
