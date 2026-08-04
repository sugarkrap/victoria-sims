#ifndef VICTORIA_GEOMETRY_READER_HEADER
#define VICTORIA_GEOMETRY_READER_HEADER

#include "victoria/coreTypes.h"
#include "victoria/memoryArena.h"

/* Reads a GMDC — geometric data container — into something that can be drawn.
 *
 * This is the end of the chain the scenegraph walks: CRES names a resource,
 * SHPE binds a material to geometry, GMND holds the practical settings, and
 * GMDC holds the buffers. Everything above it is indirection; this is where the
 * vertices are.
 *
 * A GMDC is wrapped in the scenegraph's resource collection header, so this
 * reads that too rather than making every caller skip it.
 *
 * What is deliberately not read yet:
 *
 *   - Bone assignments and weights. A mesh comes back in its bind pose, which
 *     is the right thing to look at first and the wrong thing to animate.
 *   - Morph targets. Face shapes and body sliders live here.
 *   - Every primitive after the first. A model with separate parts — a bed's
 *     frame and its bedding — will draw only one of them, and says how many it
 *     found so a caller can tell that is what happened.
 *
 * The mesh's arrays are copied into the arena rather than pointed at the source
 * buffer. The buffer's floats are not aligned, and reading an unaligned float
 * is a fault on ARMv5 rather than a slow path. */

#define GEOMETRY_ELEMENT_POSITION 0x5B830781UL
#define GEOMETRY_ELEMENT_NORMAL 0x3B83078BUL
#define GEOMETRY_ELEMENT_TEXTURE_COORDINATE 0xBB8307ABUL
#define GEOMETRY_ELEMENT_TANGENT 0x89D92BA0UL
#define GEOMETRY_ELEMENT_BONE_ASSIGNMENT 0xFBD70111UL
#define GEOMETRY_ELEMENT_BONE_WEIGHT 0x3BD70105UL

/* Longest primitive name kept. Retail names are short — "teapot", "body",
 * "hair" — and one that overruns is truncated rather than rejected: a name is
 * for a human reading a log, and losing the mesh over it would be absurd. */
#define GEOMETRY_NAME_LIMIT 64UL

/* One reason per cause, and never one code for several.
 *
 * These are counted and reported, and a bucket that three different causes
 * share cannot be acted on: a run that refused 238 meshes for "version" looked
 * exactly the same whether the block version was too old, the element list too
 * long, or the vertex count past what a half word index can address. Splitting
 * them costs nothing and is the difference between a number and a diagnosis. */
typedef enum GeometryReadResult
{
    GEOMETRY_READ_OK = 0,
    GEOMETRY_READ_NOT_A_RESOURCE,
    GEOMETRY_READ_WRONG_TYPE,
    GEOMETRY_READ_UNSUPPORTED_VERSION,
    GEOMETRY_READ_TRUNCATED,
    GEOMETRY_READ_NO_GEOMETRY,
    GEOMETRY_READ_OUT_OF_ARENA,
    /* A collection marked 0xFFFE0001 or 0xFFFD0001. Older than what is read
       here, and laid out differently, but not rubbish. */
    GEOMETRY_READ_OLDER_COLLECTION,
    GEOMETRY_READ_TOO_MANY_ELEMENTS,
    /* More vertices than a half word index can address, which is this reader's
       limit rather than the format's. */
    GEOMETRY_READ_TOO_MANY_VERTICES
} GeometryReadResult;

#define GEOMETRY_READ_RESULT_COUNT 10U

const char *geometryReadResultGetName(GeometryReadResult result);

typedef struct GeometryMesh
{
    char name[GEOMETRY_NAME_LIMIT];
    /* The name the resource carries, which is what a CRES refers to. */
    char resourceName[GEOMETRY_NAME_LIMIT];

    /* Three floats per vertex. Never null when the read succeeded. */
    const Real32 *positions;
    /* Three per vertex, or null when the mesh carries none. */
    const Real32 *normals;
    /* Two per vertex, or null. */
    const Real32 *textureCoordinates;
    Unsigned32 vertexCount;

    const Unsigned16 *indices;
    Unsigned32 indexCount;

    /* How many the file held, so a caller can say "showing 1 of 3" rather than
     * quietly drawing part of a model. */
    Unsigned32 primitiveCount;

    /* What the resource said about itself, filled in as soon as it is known and
       left set when the read then fails. A refusal that cannot say which
       version it refused sends the next reader guessing. Zero when the read did
       not get that far. */
    Unsigned32 versionMark;
    Unsigned32 containerVersion;
} GeometryMesh;

GeometryReadResult geometryReaderOpen(GeometryMesh *mesh, const Unsigned8 *bytes, MemorySize sizeInBytes,
                                      MemoryArena *arena);

/* The axis-aligned bounds, for framing a camera on a model whose scale is not
 * known in advance. Writes three floats to each. */
void geometryMeshGetBounds(const GeometryMesh *mesh, Real32 *minimum, Real32 *maximum);

#endif
