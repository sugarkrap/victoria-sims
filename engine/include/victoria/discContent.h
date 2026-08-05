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

/* How many parts of a model are remembered. A Sim is a handful; a lot is a
   number chosen so that meeting the limit is worth reporting rather than
   normal. */
#define DISC_CONTENT_PART_LIMIT 32U

/* How many of a primitive's named bones are kept for reporting. Enough to see
   what kind of number they are, which is all the log needs to settle it. */
#define DISC_CONTENT_BONE_SAMPLE 6U

/* One drawable piece of a model: a mesh, the material it wears, and which node
   of the transform tree placed it. The transform itself is not applied yet —
   that is the skeleton's job — but the node is recorded now because finding it
   again later would mean walking the tree a second time. */
typedef struct DiscModelPart
{
    char meshName[RESOURCE_NAME_LIMIT];
    char materialName[RESOURCE_NAME_LIMIT];
    char shapeName[RESOURCE_NAME_LIMIT];
    Unsigned32 nodeIndex;
    Unsigned32 levelOfDetail;
    /* Its range in the mesh's index array, which is what a renderer that paints
       parts separately will draw. */
    Unsigned32 firstIndex;
    Unsigned32 indexCount;
} DiscModelPart;

/* What an animation did to one bone's chain, for diagnosing a pose rather than
   for building one. Filled only when a pose is applied. */
typedef struct DiscContentBoneReport
{
    char nodeName[RESOURCE_NAME_LIMIT];
    /* Nodes from this bone up to the root, and how many of them the animation
       named at all, and how many of those this understood well enough to
       apply. named above applied is a partly posed chain. */
    Unsigned32 chainLength;
    Unsigned32 chainNamed;
    Unsigned32 chainApplied;
    /* The first channel on the chain that named a node and was then skipped,
       so the log can say which kind is being dropped instead of only that
       something was. */
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
    /* The disc is walked twice: the game's own character meshes first, then
       everything else.
     *
     * Taking the first package that yields a model was right while nothing
       yielded one. It is wrong now: the first is a tutorial neighbourhood's
       character file, which holds a face and the skeleton it hangs on and no
       body at all — a Sim's body is assembled at run time from outfit
       resources, not linked from its scenegraph. The game's own meshes are
       whole models, and they are on the disc. */
    Boolean walkingPreferred;
    Boolean foundInPreferred;

    /* Whether a model has to be skinned to be taken.
     *
     * The first package that yields one is a face, and a face is rigid — it
     * hangs off a single joint whole. So the search kept landing somewhere
     * perfectly reasonable and never anywhere with a skeleton to apply, and no
     * amount of narrowing by directory name fixed that, because which package
     * holds a body is not written in its path.
     *
     * Whether a mesh carries bone assignments is, though, and the mesh answers
     * it. So the first round asks for one and walks past what it finds instead;
     * if the round ends with nothing skinned, it goes back for the first model
     * it passed over rather than coming away empty. */
    Boolean wantingSkinned;
    Boolean rigidModelFound;
    /* Which file it was in, not the model itself. Keeping the model would mean
       keeping a package's allocation under every later attempt, and the arena
       is a stack — one package is cheaper to read twice than to hold. */
    Unsigned32 rigidModelIndex;
    /* How many were walked past, which says whether the disc is full of rigid
       models or the search only ever met the one. */
    Unsigned32 rigidModelsPassed;

    /* Set when the search has been pointed at one package by name of index
       rather than left to walk. Nothing else is opened, and what that package
       holds is taken as it is. */
    Boolean limitedToOneFile;
    Unsigned32 onlyFileIndex;

    /* What the skeleton did to the mesh. Nought posed with weights present
       means the bone lists named nothing the tree could be indexed by, which is
       a different problem from a mesh with no weights and must not read as the
       same one. */
    Unsigned32 verticesPosed;
    Unsigned32 bonesInPalette;
    /* The first few bones a primitive named, kept for the log, and the names of
       the nodes they resolved to. The names are what an animation matches on,
       so a run that poses nothing can be read against them to see whether the
       animation and the model disagree about what a bone is called. */
    Unsigned32 firstBoneNames[DISC_CONTENT_BONE_SAMPLE];
    char firstBoneNodeNames[DISC_CONTENT_BONE_SAMPLE][RESOURCE_NAME_LIMIT];
    Unsigned32 firstBoneNameCount;

    /* How the primitives' bone numbers resolved against the tree. They are
       identifiers nodes carry rather than positions in the node list, so a
       number that matches no node is a real miss and not an index out of range;
       counting the two separately is what tells one from the other. */
    Unsigned32 bonesMatchedToANode;
    Unsigned32 bonesWithoutANode;

    /* What the container's own bind pose turned out to be, measured rather than
       assumed. Both are the largest deviation of any matrix cell over every
       bone measured.
       - bindPoseFromIdentity: how far the bone's world transform times the
         stored one lands from the identity. Near nought means the stored
         transform is already the INVERSE bind, and a pose palette needs no
         matrix inverse.
       - bindPoseFromWorld: how far the stored transform is from the world
         transform itself. Near nought means it is the FORWARD bind instead.
       Exactly one of them should be small. Both large means the bone numbering
       is wrong, not the direction. */
    Real32 bindPoseFromIdentity;
    Real32 bindPoseFromWorld;
    Unsigned32 bonesMeasured;

    /* What an animation did to the mesh, once one was applied. Bones posed with
       no channels applied means the tree and the animation share no node names,
       which is a different failure from an animation that would not read and
       must not report as the same one. */
    Unsigned32 channelsApplied;
    Unsigned32 bonesPosed;
    /* How far the pose moved the model, against how big the model is. A count
       of vertices posed cannot tell a pose from the spike this project drew
       once already — that moved every vertex too. A shift on the order of the
       span is a pose; one many times larger is the spike; nought is neither. */
    Real32 poseShift;
    Real32 poseSpan;

    /* Per bone the primitives actually use, how much of its chain to the root
     * the animation reached and how much of that this understood.
     *
     * The discriminator for a torn mesh. A bone posed by a convention that is
     * wrong but uniform moves coherently with its neighbours and looks like a
     * badly rotated face; one whose chain is only partly applied is dragged
     * against a neighbour still in its bind pose, and the mesh stretches
     * between them. Named channels far outnumbering applied ones says which of
     * those is happening, and naming the first type skipped says what to write
     * next. */
    DiscContentBoneReport boneReports[DISC_CONTENT_BONE_SAMPLE];
    Unsigned32 boneReportCount;

    /* The mesh's vertices as the container gave them, kept aside the first time
     * a pose is applied.
     *
     * geometryMeshApplySkin rewrites the mesh in place, so posing a mesh that
     * has already been posed skins an already-skinned model and compounds the
     * two. Restoring from this first makes a pose idempotent, which is what
     * lets one be rebuilt every frame from the same starting point rather than
     * drifting further from the bind pose with each one.
     *
     * Normals as well as positions: skinning rotates them too, and a mesh whose
     * positions were restored while its normals were not would light itself
     * from a pose it is no longer in. */
    Real32 *bindPositions;
    Real32 *bindNormals;
    Unsigned32 bindVertexCount;

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
    /* Whether the node the part hangs from actually moved it. False for a model
       whose one node is its root, which is most objects, and true for anything
       hanging off a skeleton. Reported because a transform that silently does
       nothing looks exactly like one that was never applied. */
    Boolean partWasMoved;

    /* Every part the model is made of, not only the one being drawn.
     *
     * A Sim is not a mesh. Its resource node names several shapes — a head, a
     * body, hands — and each shape names meshes and the materials they wear.
     * The chain has always stopped at the first of each, which is why what
     * arrives on screen is a face rather than a person.
     *
     * Collected before anything is drawn, because how many there are and what
     * they wear decides what the renderer has to be able to do, and that is not
     * a thing to guess at. */
    Unsigned32 partCount;
    Unsigned32 partsBeyondRoom;
    /* How many nodes of the tree name a shape, against how many of those name
       one this package holds. A Sim's tree has a hundred and twenty six nodes
       and yields one face, so the difference between these two is the whole
       question of where the rest of the body is: in another package, or not
       named at all. */
    /* What the shape said each primitive wears, kept because the primitives
       are not known until the container has been read — long after the shape's
       own bytes have been given back. */
    Unsigned32 bindingCount;
    ShapeMaterialBinding bindings[SCENEGRAPH_MATERIAL_LIMIT];

    Unsigned32 shapeReferences;
    Unsigned32 shapeReferencesResolved;
    /* Meshes set aside as a coarser copy of one already kept. */
    Unsigned32 coarserPartsDropped;
    DiscModelPart parts[DISC_CONTENT_PART_LIMIT];

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

/* Begins again over one package and no others.
 *
 * For when something else has already worked out which package is worth
 * reading — the index can say which containers carry bone assignments, and
 * that is a better answer than any rule about directory names. Takes whatever
 * that package yields: the caller has already decided this is the one, so a
 * model in it that turns out rigid is still the model to draw. */
void discContentBeginInFile(DiscContentSearch *search, VirtualFileSystem *fileSystem,
                            MemoryArena *arena, Unsigned32 fileIndex);

/* Tries one package. */
DiscContentStatus discContentStep(DiscContentSearch *search);

/* Steps until it finds something or runs out. Only for a store that never
 * answers PENDING — a file descriptor, or bytes already in memory. */
DiscContentStatus discContentRunToCompletion(DiscContentSearch *search);

/* Moves the found mesh into the pose the animation holds at a tick.
 *
 * This is the point of everything the skeleton work has been building towards,
 * and it is the only thing entitled to move a skinned mesh. The palette it
 * builds is each bone's animated world transform times the transform the
 * container stored for it — which is the inverse bind, measured rather than
 * assumed — so a mesh posed by the animation it was authored against comes back
 * exactly where it started, and one posed by any other moves.
 *
 * The mesh's own arrays are rewritten, so this builds one pose rather than
 * animating: playing an animation wants the blend on the graphics processor
 * with the mesh left alone.
 *
 * False when there was nothing to do — no skinning, no tree, no bind pose, no
 * channels — or when the palette would not fit. The counts on the search say
 * which, and a caller that reports "posed" without reading them would call a
 * mesh that never moved a success. */
Boolean discContentPoseFromAnimation(DiscContentSearch *search, const Animation *animation,
                                     Real32 tick, MemoryArena *arena);

/* Copies the mesh's vertices aside as the pose to skin from.
 *
 * Must be called before the first pose attempt and while the mesh is still in
 * the pose the container gave it, because that is what every pose is built
 * from. Taking it lazily inside the first pose looks equivalent and is not: a
 * pose attempt is made against an arena marker that a rejected animation
 * rewinds, so a copy taken there can be handed back while a later pose still
 * believes in it — and the next pose then skins a mesh that is already posed.
 *
 * That is not a hypothetical. It happened: the rest pose was applied, the copy
 * was dropped when the next animation was rejected, and the animation after
 * that was posed on top of the rest pose rather than from the bind pose. The
 * only visible sign was the model's measured span quietly changing from 0.242
 * to 0.230.
 *
 * False when the copy would not fit, which leaves posing unavailable rather
 * than compounding. */
Boolean discContentKeepBindPose(DiscContentSearch *search, MemoryArena *arena);

/* Follows one named model in an open package all the way to its vertices.
 *
 * CRES names a shape, the shape names a mesh, the mesh is in a container: the
 * same chain the search walks, but starting from a name rather than from
 * whatever the package happened to hold first. That is what a Sim needs — its
 * parts are known by name and there are three of them in one package, so
 * "the first model in this file" is not a question worth asking there.
 *
 * The mesh is left in the arena, so a caller merging several must not rewind
 * between them. materialName is filled with the material the shape binds, or
 * left empty when it binds none.
 *
 * False when any hop fails, and the log line the caller writes should say which
 * — a missing shape and an unreadable container are different problems. */
/* Where the chain from a name to a container stopped. One code per hop, never
   one for several: "did not resolve" was the first version of this and it named
   four different failures identically, which is a report that cannot be acted
   on. */
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
