#ifndef VICTORIA_RESOURCE_NODE_HEADER
#define VICTORIA_RESOURCE_NODE_HEADER

#include "victoria/coreTypes.h"
#include "victoria/packageReader.h"
#include "victoria/resourceCollection.h"

/* The tree that says where each part of a model goes.
 *
 * A CRES is the top of the chain. Unlike the resources below it, it is not one
 * block with a header — it is a collection of them: a root node listing its
 * children, and a node per part carrying that part's position and rotation.
 * A shape reference is one of those nodes, so following the tree is what turns
 * "these are the meshes" into "and this one belongs there".
 *
 * Reaching any block means consuming every block before it, and a block's
 * length is only knowable by parsing it. So this reads block types it has no
 * interest in purely to get past them, and stops at the first type it does not
 * know rather than guessing a length — a wrong guess would not fail, it would
 * quietly read the next block from the middle of this one.
 *
 * The nodes are read here; nothing is drawn and nothing is allocated.
 *
 * What this does not do is skinning. A Sim's mesh is posed by bones, and the
 * bone identifiers are read but not yet applied, so a body still arrives in its
 * bind pose. Furniture, which is placed by these transforms rather than by
 * bones, is what this makes correct today. */

/* Nodes kept per model, and edges between them.

   Thirty-two was the first guess and it was wrong: a retail Sim's head model
   walks 177 blocks and carries more transform nodes than that, because a
   character's tree is its skeleton rather than a handful of parts. A model that
   still overruns keeps what fits and reports the true count beside it, so a cap
   being hit is visible rather than silently rounding a skeleton down. */
#define RESOURCE_NODE_LIMIT 128U
#define RESOURCE_NODE_EDGE_LIMIT 256U

typedef enum ResourceNodeResult
{
    RESOURCE_NODE_OK = 0,
    RESOURCE_NODE_NOT_A_RESOURCE,
    RESOURCE_NODE_OLDER_COLLECTION,
    RESOURCE_NODE_WRONG_TYPE,
    RESOURCE_NODE_TRUNCATED,
    /* The walk met a block type it cannot measure and stopped. Whatever was
       read before it is good; everything after it is unreachable. */
    RESOURCE_NODE_UNKNOWN_BLOCK
} ResourceNodeResult;

const char *resourceNodeResultGetName(ResourceNodeResult result);

typedef struct TransformNode
{
    char name[RESOURCE_NAME_LIMIT];
    /* Which block this came from, which is how the tree's references name it. */
    Unsigned32 blockIndex;

    Real32 translation[3];
    /* x, y, z, w — the order the file writes them, not the order every maths
       library expects. */
    Real32 rotation[4];
    Unsigned32 boneIdentifier;

    /* -1 when nothing claimed this node as a child, which makes it a root. */
    Integer32 parentIndex;

    /* Set when this node references a shape, which is what makes it a part of
       the model that gets drawn rather than a joint on the way there. */
    Boolean hasShape;
    PackageResourceKey shapeKey;
} TransformNode;

typedef struct ResourceNodeDescription
{
    char resourceName[RESOURCE_NAME_LIMIT];
    Unsigned32 blockVersion;

    /* How many blocks the collection held and how many the walk got through.
       They differ when an unknown block type stopped it, and the gap is the
       only honest measure of how much of the model is missing. */
    Unsigned32 blockCount;
    Unsigned32 blocksRead;

    Unsigned32 nodeCount;
    Unsigned32 storedNodeCount;
    TransformNode nodes[RESOURCE_NODE_LIMIT];
} ResourceNodeDescription;

ResourceNodeResult resourceNodeRead(ResourceNodeDescription *description, const Unsigned8 *bytes,
                                    MemorySize sizeInBytes);

/* The node's transform with every parent's applied, as a column major four by
   four. A part's own transform is relative to whatever it hangs from, so this
   is what a caller actually needs to place it. */
void resourceNodeGetWorldTransform(const ResourceNodeDescription *description, Unsigned32 nodeIndex,
                                   Real32 *matrix);

#endif
