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
 *
 * The mesh's arrays are copied into the arena rather than pointed at the source
 * buffer. The buffer's floats are not aligned, and reading an unaligned float
 * is a fault on ARMv5 rather than a slow path. */

/* Distinct unused element kinds remembered per mesh. */
#define GEOMETRY_UNUSED_ELEMENT_LIMIT 12U

#define GEOMETRY_ELEMENT_POSITION 0x5B830781UL
#define GEOMETRY_ELEMENT_NORMAL 0x3B83078BUL
#define GEOMETRY_ELEMENT_TEXTURE_COORDINATE 0xBB8307ABUL
#define GEOMETRY_ELEMENT_TANGENT 0x89D92BA0UL
#define GEOMETRY_ELEMENT_BONE_ASSIGNMENT 0xFBD70111UL
#define GEOMETRY_ELEMENT_BONE_WEIGHT 0x3BD70105UL

/* The slot value meaning "no bone here". The file's own sentinel, not one
 * chosen here: an assignment word packs four byte indices and spells an empty
 * slot 255. */
#define GEOMETRY_BONE_NONE 255U

/* What an element identifier is called, for a log. Every identifier the format
 * uses is known and written down — the table in
 * legacy/scripts/openTS2/Files/Formats/DBPF/Scenegraph/Block/GeometryData/GeometryElement.cs
 * lists all nineteen with both their wiki and in-game names — so an element
 * this reader passes over can say what it was rather than only what number it
 * had. Returns null for an identifier that is in no table. */
const char *geometryElementGetName(Unsigned32 identifier);

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
    /* An element count larger than the resource has bytes to describe, which
       means the count is not a count. Not a ceiling of this reader's choosing:
       one of those refused 238 of 239 readable containers on a retail disc. */
    GEOMETRY_READ_TOO_MANY_ELEMENTS,
    /* More vertices than a half word index can address, which is this reader's
       limit rather than the format's. */
    GEOMETRY_READ_TOO_MANY_VERTICES
} GeometryReadResult;

#define GEOMETRY_READ_RESULT_COUNT 10U

const char *geometryReadResultGetName(GeometryReadResult result);

/* One named part of a model: a bed's frame as distinct from its bedding, or a
 * Sim's body as distinct from the shadow under it.
 *
 * The file gives each primitive its own faces and points it at a component,
 * which is the set of vertices it draws from. Different primitives may use
 * different components, so the vertices are not one array in the file. They are
 * merged into one here and the indices adjusted to match, which means a caller
 * can draw the whole model in one call and still address a single part when it
 * needs to — for a material, or to leave one out. */
typedef struct GeometryPrimitive
{
    char name[GEOMETRY_NAME_LIMIT];
    /* Which component the file said it draws from, kept for reporting: the
       indices below already point into the merged arrays. */
    Unsigned32 componentIndex;
    /* Its range in the mesh's index array. */
    Unsigned32 firstIndex;
    Unsigned32 indexCount;

    /* What this primitive's bone slots mean.
     *
     * A vertex's assignment slots hold small numbers — nought to three on a
     * face — and those are not bones. They are indices into this array, which
     * is per primitive, and this array holds the bones. Without it the slots
     * are meaningless: slot 1 means one thing on the head and another on the
     * hands, and a mesh skinned as though it meant the same would fold itself
     * inside out in a way that looks like broken maths rather than a missing
     * table.
     *
     * Null with a zero count when the primitive named no bones, which is what a
     * rigid part looks like. */
    const Unsigned32 *boneRemap;
    Unsigned32 boneRemapCount;
    /* Which vertices of the merged arrays are this primitive's component's.
       Its faces only reach these, and skinning walks them directly rather than
       through the indices — a vertex reached twice through two faces must not
       be transformed twice. */
    Unsigned32 firstVertex;
    Unsigned32 vertexCount;
} GeometryPrimitive;

typedef struct GeometryMesh
{
    char name[GEOMETRY_NAME_LIMIT];
    /* The name the resource carries, which is what a CRES refers to. */
    char resourceName[GEOMETRY_NAME_LIMIT];

    /* Three floats per vertex. Never null when the read succeeded.
     *
     * Not const: the arrays are the caller's arena, and placing a part by its
     * node's transform rewrites them in place. Declaring them const and casting
     * it away at the one place that writes would be the same operation with the
     * warning turned off. */
    Real32 *positions;
    /* Three per vertex, or null when the mesh carries none. */
    Real32 *normals;
    /* Two per vertex, or null. */
    Real32 *textureCoordinates;
    Unsigned32 vertexCount;

    const Unsigned16 *indices;
    Unsigned32 indexCount;

    /* Every part of the model, in the order the file listed them. Their index
     * ranges tile the mesh's index array, so drawing all of the indices draws
     * all of the parts. */
    const GeometryPrimitive *primitives;
    /* How many the file held, and how many are in the array above. They differ
     * when a primitive pointed at a component that does not exist, which is a
     * file this reader will not invent vertices for. */
    Unsigned32 primitiveCount;
    Unsigned32 storedPrimitiveCount;
    /* How many components the vertices were merged from, so a caller can tell a
     * one piece model from an assembled one. */
    Unsigned32 componentCount;

    /* Four bone indices per vertex, GEOMETRY_BONE_NONE where a slot is unused,
     * or null when the mesh carries no skinning at all. A face is rigid — it
     * hangs off one joint as a whole — and has none of this; a body does.
     *
     * Unpacked from the word the file stores rather than kept as one, because
     * every reader of it wants a slot and not a bit shift. */
    Unsigned8 *boneAssignments;
    /* Four weights per vertex, in the same slot order. The file stores one, two
     * or three of them and leaves the last implied so they sum to one;
     * weightsStoredPerVertex says which, and the implied one is computed here
     * so callers do not each have to remember the rule. */
    Real32 *boneWeights;
    Unsigned32 weightsStoredPerVertex;
    /* Vertices that ended up with at least one real bone. Zero alongside a
       non-null array means the elements were there and said nothing, which is
       a different problem from their not being there. */
    Unsigned32 skinnedVertexCount;

    /* Element kinds met and not used, with the format each was in. Reported
     * because what a mesh carries decides what the renderer has to be able to
     * do, and a mesh that quietly holds morph targets or a second colour set
     * looks exactly like one that does not. */
    Unsigned32 unusedElements[GEOMETRY_UNUSED_ELEMENT_LIMIT];
    Unsigned32 unusedElementFormats[GEOMETRY_UNUSED_ELEMENT_LIMIT];
    Unsigned32 unusedElementCount;

    /* What the resource said about itself, filled in as soon as it is known and
       left set when the read then fails. A refusal that cannot say which
       version it refused sends the next reader guessing. Zero when the read did
       not get that far. */
    Unsigned32 versionMark;
    Unsigned32 containerVersion;
    /* How many geometry elements the container claimed, set before it is acted
       on so a refusal can quote it. */
    Unsigned32 elementCount;
} GeometryMesh;

GeometryReadResult geometryReaderOpen(GeometryMesh *mesh, const Unsigned8 *bytes, MemorySize sizeInBytes,
                                      MemoryArena *arena);

/* The axis-aligned bounds, for framing a camera on a model whose scale is not
 * known in advance. Writes three floats to each. */
void geometryMeshGetBounds(const GeometryMesh *mesh, Real32 *minimum, Real32 *maximum);

/* Moves every vertex by a column major four by four, and every normal by its
   rotation alone — translating a direction would turn it into a point.
   Rewrites the mesh's arrays, which are the caller's arena. */
void geometryMeshApplyTransform(GeometryMesh *mesh, const Real32 *matrix);

/* Moves every weighted vertex to where its bones put it.
 *
 * boneMatrices is boneCount matrices of sixteen floats, in the same order the
 * primitives' bone lists index. A vertex's assignment slot names a place in its
 * primitive's list; that names a bone; that names a matrix here. Each vertex is
 * the weighted sum of up to four of them.
 *
 * EACH MATRIX MUST ALREADY BE THE POSE TIMES THE INVERSE BIND — the bone's
 * transform in the pose wanted, times the inverse of its transform in the pose
 * the mesh was authored in. Not the bone's world transform on its own.
 *
 * That is not a detail. The mesh on the disc is stored in its bind pose, so
 * every bone's pair multiplies out to the identity there and skinning a resting
 * model correctly moves nothing at all. Passing world transforms instead
 * transforms vertices that are already in world space a second time, and what
 * arrives is a Sim's face with a limb stretched out of it — which is what this
 * did on the first try, and what the rule above is written down to prevent.
 *
 * Positions and normals are rewritten in place, so this is for building one
 * pose of a model, not for animating: an animated model wants the blend on the
 * graphics processor with the mesh left alone.
 *
 * Returns how many vertices it actually moved: a mesh whose bone lists point
 * outside the matrices it was given moves nothing, and a caller that cannot
 * tell that from a mesh with no weights would report a pose it never applied. */
Unsigned32 geometryMeshApplySkin(GeometryMesh *mesh, const Real32 *boneMatrices,
                                 Unsigned32 boneCount);

#endif
