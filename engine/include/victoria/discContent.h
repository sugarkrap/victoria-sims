#ifndef VICTORIA_DISC_CONTENT_HEADER
#define VICTORIA_DISC_CONTENT_HEADER

#include "victoria/coreTypes.h"
#include "victoria/geometryReader.h"
#include "victoria/memoryArena.h"
#include "victoria/material.h"
#include "victoria/resourceNode.h"
#include "victoria/textureReader.h"
#include "victoria/scenegraph.h"
#include "victoria/virtualFileSystem.h"

/* Finds something to draw on a disc.
 *
 * Given a catalogue, this opens each package in turn and stops at the first one
 * carrying geometry it can read.
 *
 * How it picks that geometry depends on what the package holds. The chain is
 * entered as high up as it can be: a resource node names the model and the
 * shapes it is assembled from, a shape names the geometry nodes it is built
 * from, and each of those names the container holding its vertices. A mesh
 * reached that way was chosen — it is part of a named model, not whichever
 * container the index happened to list first.
 *
 * Each hop falls back to the one below it. A package with no resource node is
 * entered at its shape; one with neither takes the first container outright.
 * That last rule is blunt, and it is kept because it still answers the question
 * this was written to answer — is the path from a disc to a triangle connected
 * — for the many packages that carry a container and nothing else.
 *
 * It counts what it passed over on the way. A retail disc will have every
 * package refused for the same reason, and a caller that can only say "nothing
 * found" cannot tell that from an empty disc. */

#define DISC_CONTENT_PATH_LIMIT 320UL

/* Container versions 0 to 14, then everything above in the last. */
#define DISC_CONTENT_VERSION_BUCKETS 16U

typedef enum DiscContentStatus
{
    DISC_CONTENT_FOUND = 0,
    /* More packages to try; step again. Also covers a store that has not
     * answered yet, which is what a browser's does. */
    DISC_CONTENT_PENDING,
    DISC_CONTENT_NONE_FOUND,
    DISC_CONTENT_OUT_OF_ARENA
} DiscContentStatus;

const char *discContentStatusGetName(DiscContentStatus status);

typedef struct DiscContentSearch
{
    VirtualFileSystem *fileSystem;
    MemoryArena *arena;
    MemorySize arenaMarker;
    Unsigned32 nextIndex;

    /* Filled in when the status is FOUND. */
    char packagePath[DISC_CONTENT_PATH_LIMIT];
    GeometryMesh mesh;
    /* The model this mesh belongs to, when a shape named it. Empty when the
       container was taken directly, which is not the same thing and should not
       be reported as though it were. */
    char modelName[RESOURCE_NAME_LIMIT];
    Boolean foundThroughScenegraph;
    /* The model's transform tree, when it had one. Held here rather than on a
       stack: it is a few kilobytes, and the web build's stack is not. */
    ResourceNodeDescription modelTree;
    Boolean modelHasTree;
    Unsigned32 modelNodeIndex;

    /* The material the model's first part wears, and the texture that material
       paints with. Both are found by name — nothing in this chain is numbered —
       so a missing one means a name that did not match, not a broken file.

       The texture's bytes point into the arena, so they are only valid while
       the found package's allocation stands. */
    char materialName[RESOURCE_NAME_LIMIT];
    Boolean materialFound;
    /* How many materials and textures the package held at all. "Not found" and
       "none here to find" are different answers: the first is a name that did
       not match, the second means the resource lives in another package and the
       fix is a wider search rather than a better comparison. */
    Unsigned32 materialsInPackage;
    Unsigned32 texturesInPackage;
    /* The image the material asked for, by name. Kept even when it was not
       found here, because that name is what a search of the rest of the disc
       has to go on. */
    char textureName[RESOURCE_NAME_LIMIT];
    Boolean textureFound;
    TextureDescription texture;

    /* What the search met on the way, so a report can be specific. */
    Unsigned32 packagesOpened;
    Unsigned32 packagesCompressed;
    Unsigned32 packagesWithGeometry;
    /* How many packages carried a readable shape, and how many of those led all
       the way to a container. The gap between them is the interesting number:
       it is shapes whose meshes this engine could not follow. */
    Unsigned32 packagesWithShapes;
    /* Packages whose model could be entered from the top, at its resource node,
       rather than from whichever shape was filed first. */
    Unsigned32 packagesWithTrees;
    Unsigned32 modelsResolved;
    Unsigned32 geometryRefused;
    /* Why the refusals happened, one bucket per GeometryReadResult. A disc that
       refuses hundreds of meshes for one reason and a disc that refuses them for
       six are different problems, and a bare count cannot tell them apart. */
    Unsigned32 refusalsByReason[GEOMETRY_READ_RESULT_COUNT];
    /* Refusals that never reached the geometry reader because the stream would
       not decompress. */
    Unsigned32 decompressionRefused;

    /* Which container versions the disc actually holds, read or not. A reason
       says what this engine did; this says what the game shipped, which is the
       part no amount of reasoning about the reader can supply. Anything past
       the last bucket lands in it. */
    Unsigned32 versionsSeen[DISC_CONTENT_VERSION_BUCKETS];
    /* The first collection mark that was not 0xFFFF0001. Kept whole rather than
       bucketed: there are only a few, and knowing it was 0xFFFE0001 rather than
       "some other mark" is the whole question.

       The flag is not redundant with a zero mark. A resource beginning with
       four zero bytes has a mark of zero, and the first version of this used
       zero to mean "nothing recorded" — which swallowed exactly the case it was
       added to report. */
    Boolean sawUnknownMark;
    Unsigned32 firstUnknownMark;

    /* The largest element count any container claimed, so a refusal for having
       too many can be checked against what the disc actually holds instead of
       argued about. */
    Unsigned32 largestElementCount;
} DiscContentSearch;

void discContentBegin(DiscContentSearch *search, VirtualFileSystem *fileSystem, MemoryArena *arena);

/* Tries one package. */
DiscContentStatus discContentStep(DiscContentSearch *search);

/* Steps until it finds something or runs out. Only for a store that never
 * answers PENDING — a file descriptor, or bytes already in memory. */
DiscContentStatus discContentRunToCompletion(DiscContentSearch *search);

#endif
