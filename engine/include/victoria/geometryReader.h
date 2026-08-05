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
 * Bone assignments, weights, the bind pose and the deformation channels are all
 * read. What is deliberately not read yet:
 *
 *   - Morph normal deltas, and the two morph index elements beside them. A
 *     deformed vertex therefore keeps its resting normal and is shaded very
 *     slightly wrong, on a shape that has only slightly changed.
 *   - Tangents, binormals, and the second colour set.
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

/* Deformation. One map element per component, and up to four sets of deltas
   alongside it — see GEOMETRY_MORPH_SLOT_LIMIT for what the pair means. */
#define GEOMETRY_ELEMENT_MORPH_VERTEX_MAP 0xDCF2CFDCUL
#define GEOMETRY_ELEMENT_MORPH_VERTEX_DELTA 0x5CF2CFE1UL

/* How many deformation channels one vertex can be in at once.
 *
 * Four, because the map spends one packed word a vertex and gives each slot a
 * byte. That is a limit of the file, not of this reader: a body declares three
 * channels and a face twenty-seven, but no single vertex is ever moved by more
 * than four of them. */
#define GEOMETRY_MORPH_SLOT_LIMIT 4U

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

/* One bone's transform in the pose the mesh was authored in.
 *
 * The container stores these itself, in a section straight after the
 * primitives, rather than leaving them to be derived from the tree. They are
 * numbered the way a primitive's bone list is numbered, so a number out of that
 * list indexes this array directly.
 *
 * These are the INVERSE bind transforms, not the bind transforms. That was
 * measured rather than assumed, and the run that settled it is worth repeating
 * before trusting anything built on top: composing each bone's world transform
 * out of the tree with the stored one landed 0.000 from the identity, while the
 * stored one sat 1.666 away from the world transform itself. Exactly one of
 * those is small, and it is the first.
 *
 * What follows from it is that a pose palette is animatedTransform times the
 * stored transform, with no matrix inverse anywhere — the engine has none, and
 * on this evidence needs none. */
typedef struct GeometryBindPose
{
    /* x, y, z, w — the order the file writes them, matching TransformNode. */
    Real32 rotation[4];
    Real32 translation[3];
} GeometryBindPose;

/* One deformation channel the container declares, named the way the file names
 * it: a group and a channel within that group. A Sim's body carries the sliders
 * here — the fat, fit and pregnant shapes — and a face carries its archetypes.
 *
 * Both names are kept because neither identifies a channel on its own: the file
 * has several groups using the same channel name, and reporting one without the
 * other turns distinct channels into apparent duplicates. */
typedef struct GeometryMorphTarget
{
    char groupName[GEOMETRY_NAME_LIMIT];
    char channelName[GEOMETRY_NAME_LIMIT];
} GeometryMorphTarget;

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

    /* The bind pose the container carries, one entry per bone, or null when it
     * carried none. Read only for a skinned mesh, for the same reason the bone
     * lists are: a rigid model's section is empty, and reading it across the
     * disc's static objects would cost a walk to hold nothing.
     *
     * Null with a zero count also covers a section that would not read. A mesh
     * whose bind pose is unreadable still draws — it just cannot be posed — and
     * refusing the model over it would be the wrong trade. */
    const GeometryBindPose *bindPoses;
    Unsigned32 bindPoseCount;

    /* The deformation channels the container declares — body sliders, face
     * shapes, pregnancy — named but not yet applied. What moves a vertex is the
     * morph elements, which are still passed over; these say what those
     * elements are for, which is the half worth knowing first.
     *
     * Read only when the bind pose above read cleanly, because the array sits
     * immediately after it and a cursor that has already lost its place cannot
     * find this one. */
    const GeometryMorphTarget *morphTargets;
    Unsigned32 morphTargetCount;

    /* What actually moves when a channel is turned up.
     *
     * The file spends one packed word per vertex and one set of deltas per
     * slot. A slot's byte in that word is the channel it belongs to, and zero
     * means the slot is unused — which is why morphTargets keeps its blank
     * first entry rather than compacting it away.
     *
     * Unpacked here into two flat arrays, both indexed [vertex * morphSlotCount
     * + slot], with the deltas carrying three floats each. The channel is
     * widened from the file's byte to a halfword, because joining several
     * containers renumbers every channel into one shared space and the sum of
     * a Sim's parts has no reason to stay under two hundred and fifty five. Null with a zero
     * count when the container declared channels but carried nothing to move
     * with them, which is a different thing from declaring none. */
    const Unsigned16 *morphSlotChannels;
    const Real32 *morphSlotDeltas;
    Unsigned32 morphSlotCount;
    /* Vertices whose map word was actually read, as against merely allocated
     * for. Needed because the two ways of ending up with no deformation look
     * identical from outside: a map element rejected for being shorter than its
     * component leaves the channels zeroed, and so does a map that is genuinely
     * all zeroes. One is this reader's problem and the other is the disc's, and
     * they call for opposite next moves. */
    Unsigned32 morphMappedVertexCount;
    /* True when the slot-to-channel assignment was inferred rather than read.
     *
     * See the note in geometryReader.c on where that happens and on what
     * evidence. A caller reporting a deformation should say so, because an
     * inference and a reading are not the same kind of fact. */
    Boolean morphChannelsInferred;

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
    /* The allocation that failed, when the refusal was for arena space. Zero
       otherwise. A reader that says only "not enough space" leaves the next
       person bisecting the reader to find out which array it was, which is a
       run of the disc per guess. */
    MemorySize arenaWantedBytes;
} GeometryMesh;

GeometryReadResult geometryReaderOpen(GeometryMesh *mesh, const Unsigned8 *bytes, MemorySize sizeInBytes,
                                      MemoryArena *arena);

/* The axis-aligned bounds, for framing a camera on a model whose scale is not
 * known in advance. Writes three floats to each. */
void geometryMeshGetBounds(const GeometryMesh *mesh, Real32 *minimum, Real32 *maximum);

/* Joins several meshes into one that can be drawn in a single call.
 *
 * A Sim is not one model: it is a body, a face and hair, each its own container
 * with its own material. They are merged here rather than drawn as three
 * meshes because the primitives of the result already say which range of
 * indices belongs to which part — that is what a GeometryPrimitive is — so one
 * upload can still be painted a part at a time.
 *
 * Every source's primitives come across with their ranges shifted, so a part
 * that was one primitive stays one and a part that was several stays several.
 * Their bone lists come across untouched, which is right only because a bone
 * number is a skeleton-wide identifier rather than a position in any one
 * container's table.
 *
 * The merged mesh carries normals and texture coordinates if ANY source did; a
 * source carrying none contributes zeroes rather than being refused, because
 * one part without coordinates should not cost the whole Sim its skin.
 *
 * Allocated once from the totals rather than grown, because the arena is a bump
 * pointer with nothing to grow into. Every source must still be readable here.
 *
 * The bind pose is taken from whichever source carries the most entries. That
 * is sound only while the parts share a skeleton — which a Sim's do — and a
 * caller joining unrelated models must not rely on it. */
GeometryReadResult geometryMeshMerge(GeometryMesh *merged, const GeometryMesh *const *sources,
                                     Unsigned32 sourceCount, MemoryArena *arena);

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

/* Deforms the mesh by its declared channels, each turned up by its own weight.
 *
 * One weight per entry in morphTargets, so channelWeights[n] is the weight of
 * morphTargets[n]. Entry nought is read and is expected to be ignored by the
 * file rather than by this: no slot ever names channel nought, so whatever is
 * passed there moves nothing.
 *
 * ORDER MATTERS, and it is the opposite way round from what the names suggest.
 * This runs BEFORE geometryMeshApplySkin, not after. A morph is a change to the
 * shape the model was authored in — a fatter body is a different bind pose, not
 * a differently posed one — so it belongs on the resting mesh, which the skin
 * then poses. Applying it to a posed mesh adds a rest-space displacement to
 * vertices that have already left rest space, and the further a limb has swung
 * the more wrong it is.
 *
 * Positions are rewritten in place, so a caller animating this has to restore
 * the resting mesh first, exactly as it must before skinning.
 *
 * Normals are left alone. The file carries deltas for them and they are not
 * read yet; a morphed vertex therefore keeps its resting normal, which is
 * slightly wrong shading on a shape that is already only slightly changed.
 *
 * Returns how many vertices it moved. Zero from a mesh that carries channels
 * means the weights named none of them, which is worth telling apart from a
 * mesh that carries none at all. */
Unsigned32 geometryMeshApplyMorph(GeometryMesh *mesh, const Real32 *channelWeights,
                                  Unsigned32 weightCount);

#endif
