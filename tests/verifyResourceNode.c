/* Walks the resource node collection in the fixture package.

   A CRES is the one resource in the chain that is not a single block. It is a
   root plus a node per part, and reaching any of them means consuming every
   block before it — including types this engine has no interest in, purely to
   find out how long they are. The fixture's CRES holds three: a cResourceNode,
   a cDataListExtension and a cShapeRefNode. If the middle one is measured
   wrongly by so much as a byte, the third is read from the middle of the second
   and produces a transform that is pure noise.

   That is what makes the numbers below worth checking rather than eyeballing.
   The expected values were read out of the file by hand first. */

#include <stdio.h>

#include "utils/assert.h"
#include "utils/strings.h"
#include "victoria/memoryArena.h"
#include "victoria/packageReader.h"
#include "victoria/resourceNode.h"

#define FILE_BUFFER_CAPACITY (2UL * 1024UL * 1024UL)
#define ARENA_CAPACITY (2UL * 1024UL * 1024UL)

static Unsigned8 fileBuffer[FILE_BUFFER_CAPACITY];
static Unsigned8 arenaStorage[ARENA_CAPACITY];

static Integer32 failureCount = 0;

static Boolean nearly(Real32 value, Real32 expected)
{
    Real32 difference = value - expected;

    if (difference < 0.0f)
    {
        difference = -difference;
    }
    return difference < 0.001f ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

int main(void)
{
    MemoryArena arena;
    Package package;
    ResourceNodeDescription description;
    const PackageResource *resource;
    ResourceNodeResult result;
    MemorySize sizeInBytes;
    FILE *inputFile;

    memoryArenaInitialize(&arena, arenaStorage, ARENA_CAPACITY);

    inputFile = fopen("testAssets/scenegraph/teapot_model.package", "rb");
    if (inputFile == NULL)
    {
        printf("FAIL  cannot open the fixture package\n");
        return 1;
    }
    sizeInBytes = (MemorySize)fread(fileBuffer, 1, FILE_BUFFER_CAPACITY, inputFile);
    fclose(inputFile);

    if (packageReaderOpen(&package, fileBuffer, sizeInBytes, &arena) != PACKAGE_READ_OK)
    {
        printf("FAIL  the fixture package would not open\n");
        return 1;
    }

    printf("-- walking the blocks --\n");
    resource = packageReaderFindFirstOfType(&package, (Unsigned32)PACKAGE_TYPE_CRES);
    checkThat(&failureCount, "the package holds a resource node", resource != NULL_POINTER);
    if (resource == NULL_POINTER)
    {
        return checkSummarize(failureCount, "resource node");
    }

    result = resourceNodeRead(&description, packageReaderGetResourceBytes(&package, resource),
                              (MemorySize)resource->sizeInBytes);
    checkThat(&failureCount, "the reader accepts it", result == RESOURCE_NODE_OK);
    if (result != RESOURCE_NODE_OK)
    {
        printf("  result: %s (read %u of %u blocks)\n", resourceNodeResultGetName(result),
               (unsigned)description.blocksRead, (unsigned)description.blockCount);
        return checkSummarize(failureCount, "resource node");
    }

    checkThat(&failureCount, "the collection holds three blocks", description.blockCount == 3U);
    /* The measure that matters. Anything less than every block means a type was
       met that could not be sized, and the parts after it are unreachable. */
    checkThat(&failureCount, "and every one of them was walked",
              description.blocksRead == description.blockCount);

    printf("\n-- what carried a transform --\n");
    /* Two of the three: the shape reference node, and nothing else. The root
       resource node holds the tree but no transform of its own, and the data
       list extension is data. */
    checkThat(&failureCount, "one block carried a transform", description.nodeCount == 1U);
    checkThat(&failureCount, "and it was kept", description.storedNodeCount == 1U);
    if (description.storedNodeCount == 0U)
    {
        return checkSummarize(failureCount, "resource node");
    }

    printf("\n-- does it reference a shape --\n");
    {
        const TransformNode *node = &description.nodes[0];

        checkThat(&failureCount, "the node references a shape", node->hasShape);
        /* The CRES links out to exactly one file, and it is the shape sitting
           in this same package. Decoded by hand before this reader existed. */
        checkThat(&failureCount, "which is a shape resource",
                  node->shapeKey.typeIdentifier == 0xFC6EB1F7UL);
        checkThat(&failureCount, "in group 1C0532FA",
                  node->shapeKey.groupIdentifier == 0x1C0532FAUL);
        checkThat(&failureCount, "at instance FFA11ECB",
                  node->shapeKey.instanceIdentifier == 0xFFA11ECBUL);
        checkThat(&failureCount, "high word 78BFA44E",
                  node->shapeKey.instanceIdentifierHigh == 0x78BFA44EUL);
    }

    printf("\n-- and where does it put it --\n");
    {
        const TransformNode *node = &description.nodes[0];
        Real32 matrix[16];

        printf("  translation %.3f %.3f %.3f  rotation %.3f %.3f %.3f %.3f\n",
               (double)node->translation[0], (double)node->translation[1],
               (double)node->translation[2], (double)node->rotation[0], (double)node->rotation[1],
               (double)node->rotation[2], (double)node->rotation[3]);

        /* A quaternion is a unit quaternion or it is not a rotation. This is the
           cheapest proof that the floats landed on the right bytes: a transform
           read from the wrong offset is overwhelmingly unlikely to normalise. */
        {
            Real32 lengthSquared = node->rotation[0] * node->rotation[0] +
                                   node->rotation[1] * node->rotation[1] +
                                   node->rotation[2] * node->rotation[2] +
                                   node->rotation[3] * node->rotation[3];

            checkThat(&failureCount, "the rotation is a unit quaternion",
                      lengthSquared > 0.98f && lengthSquared < 1.02f);
        }

        /* Nothing claimed it as a child, so it hangs off the root. */
        checkThat(&failureCount, "it is a root", node->parentIndex == -1);

        resourceNodeGetWorldTransform(&description, 0U, matrix);
        checkThat(&failureCount, "its world transform has a sane bottom row",
                  nearly(matrix[3], 0.0f) && nearly(matrix[7], 0.0f) && nearly(matrix[11], 0.0f) &&
                      nearly(matrix[15], 1.0f));
        /* A rotation matrix keeps lengths, so its columns are unit vectors. */
        {
            Real32 columnLength = matrix[0] * matrix[0] + matrix[1] * matrix[1] +
                                  matrix[2] * matrix[2];

            checkThat(&failureCount, "and a rotation part that preserves length",
                      columnLength > 0.98f && columnLength < 1.02f);
        }
        checkThat(&failureCount, "with the translation in the last column",
                  nearly(matrix[12], node->translation[0]) &&
                      nearly(matrix[13], node->translation[1]) &&
                      nearly(matrix[14], node->translation[2]));
    }

    printf("\n-- composing a transform through a parent --\n");
    {
        /* Authored rather than found: the fixture is one node deep, and a
           composition that is never asked to compose anything proves nothing.
           A quarter turn about z, then a child pushed one unit along x, should
           put the child one unit along y. */
        ResourceNodeDescription made;
        Real32 matrix[16];
        const Real32 halfRootTwo = 0.70710678f;

        made.storedNodeCount = 2U;
        made.nodes[0].parentIndex = -1;
        made.nodes[0].translation[0] = 0.0f;
        made.nodes[0].translation[1] = 0.0f;
        made.nodes[0].translation[2] = 0.0f;
        made.nodes[0].rotation[0] = 0.0f;
        made.nodes[0].rotation[1] = 0.0f;
        made.nodes[0].rotation[2] = halfRootTwo;
        made.nodes[0].rotation[3] = halfRootTwo;

        made.nodes[1].parentIndex = 0;
        made.nodes[1].translation[0] = 1.0f;
        made.nodes[1].translation[1] = 0.0f;
        made.nodes[1].translation[2] = 0.0f;
        made.nodes[1].rotation[0] = 0.0f;
        made.nodes[1].rotation[1] = 0.0f;
        made.nodes[1].rotation[2] = 0.0f;
        made.nodes[1].rotation[3] = 1.0f;

        resourceNodeGetWorldTransform(&made, 1U, matrix);
        checkThat(&failureCount, "the parent's rotation moves the child",
                  nearly(matrix[12], 0.0f) && nearly(matrix[13], 1.0f) && nearly(matrix[14], 0.0f));

        resourceNodeGetWorldTransform(&made, 0U, matrix);
        checkThat(&failureCount, "and the parent itself stays at the origin",
                  nearly(matrix[12], 0.0f) && nearly(matrix[13], 0.0f) && nearly(matrix[14], 0.0f));

        printf("\n-- finding a bone by the identifier it carries --\n");
        /* A primitive's bone list holds identifiers, not positions in this
           list. The two are given opposite orders here on purpose: bone 9 is
           node 0 and bone 4 is node 1, so a lookup that indexed instead of
           searching would answer node 4 — off the end — or node 9, and either
           way would pose the model by the wrong joint.

           The identifiers are small, which is why this cannot be told apart by
           looking at their size. That was the guess that had to be measured. */
        made.nodes[0].boneIdentifier = 9U;
        made.nodes[1].boneIdentifier = 4U;

        checkThat(&failureCount, "the first bone is found where its identifier says",
                  resourceNodeFindByBoneIdentifier(&made, 9U) == 0);
        checkThat(&failureCount, "and the second likewise, not where its number would index",
                  resourceNodeFindByBoneIdentifier(&made, 4U) == 1);
        checkThat(&failureCount, "an identifier no node carries finds nothing",
                  resourceNodeFindByBoneIdentifier(&made, 7U) == -1);
        checkThat(&failureCount, "and the not-a-bone sentinel is not special-cased into a match",
                  resourceNodeFindByBoneIdentifier(&made, 0x7FFFFFFFUL) == -1);
    }

    printf("\n-- refusing what it should --\n");
    {
        ResourceNodeDescription other;
        static const Unsigned8 notAResource[16] = { 0 };
        const PackageResource *shape =
            packageReaderFindFirstOfType(&package, (Unsigned32)PACKAGE_TYPE_SHPE);

        checkThat(&failureCount, "rejects bytes that are not a collection",
                  resourceNodeRead(&other, notAResource, sizeof(notAResource)) ==
                      RESOURCE_NODE_NOT_A_RESOURCE);
        checkThat(&failureCount, "rejects a shape, which is a collection of the wrong kind",
                  resourceNodeRead(&other, packageReaderGetResourceBytes(&package, shape),
                                   (MemorySize)shape->sizeInBytes) == RESOURCE_NODE_WRONG_TYPE);
        checkThat(&failureCount, "rejects a resource that stops part way",
                  resourceNodeRead(&other, packageReaderGetResourceBytes(&package, resource), 40UL) !=
                      RESOURCE_NODE_OK);
    }

    return checkSummarize(failureCount, "resource node");
}
