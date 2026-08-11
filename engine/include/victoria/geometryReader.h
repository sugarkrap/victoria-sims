#ifndef VICTORIA_GEOMETRY_READER_HEADER
#define VICTORIA_GEOMETRY_READER_HEADER

#include "victoria/coreTypes.h"
#include "victoria/memoryArena.h"

#define GEOMETRY_UNUSED_ELEMENT_LIMIT 12U

#define GEOMETRY_ELEMENT_POSITION 0x5B830781UL
#define GEOMETRY_ELEMENT_NORMAL 0x3B83078BUL
#define GEOMETRY_ELEMENT_TEXTURE_COORDINATE 0xBB8307ABUL
#define GEOMETRY_ELEMENT_TANGENT 0x89D92BA0UL
#define GEOMETRY_ELEMENT_BONE_ASSIGNMENT 0xFBD70111UL
#define GEOMETRY_ELEMENT_BONE_WEIGHT 0x3BD70105UL

#define GEOMETRY_ELEMENT_MORPH_VERTEX_MAP 0xDCF2CFDCUL
#define GEOMETRY_ELEMENT_MORPH_VERTEX_DELTA 0x5CF2CFE1UL

#define GEOMETRY_MORPH_SLOT_LIMIT 4U

#define GEOMETRY_BONE_NONE 255U

const char *geometryElementGetName(Unsigned32 identifier);

#define GEOMETRY_NAME_LIMIT 64UL

typedef enum GeometryReadResult
{
    GEOMETRY_READ_OK = 0,
    GEOMETRY_READ_NOT_A_RESOURCE,
    GEOMETRY_READ_WRONG_TYPE,
    GEOMETRY_READ_UNSUPPORTED_VERSION,
    GEOMETRY_READ_TRUNCATED,
    GEOMETRY_READ_NO_GEOMETRY,
    GEOMETRY_READ_OUT_OF_ARENA,
    GEOMETRY_READ_OLDER_COLLECTION,
    GEOMETRY_READ_TOO_MANY_ELEMENTS,
    GEOMETRY_READ_TOO_MANY_VERTICES
} GeometryReadResult;

#define GEOMETRY_READ_RESULT_COUNT 10U

const char *geometryReadResultGetName(GeometryReadResult result);

typedef struct GeometryPrimitive
{
    char name[GEOMETRY_NAME_LIMIT];
    Unsigned32 componentIndex;
    Unsigned32 firstIndex;
    Unsigned32 indexCount;

    const Unsigned32 *boneRemap;
    Unsigned32 boneRemapCount;
    Unsigned32 firstVertex;
    Unsigned32 vertexCount;
} GeometryPrimitive;

typedef struct GeometryBindPose
{
    Real32 rotation[4];
    Real32 translation[3];
} GeometryBindPose;

typedef struct GeometryMorphTarget
{
    char groupName[GEOMETRY_NAME_LIMIT];
    char channelName[GEOMETRY_NAME_LIMIT];
} GeometryMorphTarget;

typedef struct GeometryMesh
{
    char name[GEOMETRY_NAME_LIMIT];
    char resourceName[GEOMETRY_NAME_LIMIT];

    Real32 *positions;
    Real32 *normals;
    Real32 *textureCoordinates;
    Unsigned32 vertexCount;

    const Unsigned16 *indices;
    Unsigned32 indexCount;

    const GeometryPrimitive *primitives;
    Unsigned32 primitiveCount;
    Unsigned32 storedPrimitiveCount;
    Unsigned32 componentCount;

    Unsigned8 *boneAssignments;
    Real32 *boneWeights;
    Unsigned32 weightsStoredPerVertex;
    Unsigned32 skinnedVertexCount;

    const GeometryBindPose *bindPoses;
    Unsigned32 bindPoseCount;

    const GeometryMorphTarget *morphTargets;
    Unsigned32 morphTargetCount;

    const Unsigned16 *morphSlotChannels;
    const Real32 *morphSlotDeltas;
    Unsigned32 morphSlotCount;
    Unsigned32 morphMappedVertexCount;
    Boolean morphChannelsInferred;

    Unsigned32 unusedElements[GEOMETRY_UNUSED_ELEMENT_LIMIT];
    Unsigned32 unusedElementFormats[GEOMETRY_UNUSED_ELEMENT_LIMIT];
    Unsigned32 unusedElementCount;

    Unsigned32 versionMark;
    Unsigned32 containerVersion;
    Unsigned32 elementCount;
    MemorySize arenaWantedBytes;
} GeometryMesh;

GeometryReadResult geometryReaderOpen(GeometryMesh *mesh, const Unsigned8 *bytes, MemorySize sizeInBytes,
                                      MemoryArena *arena);

void geometryMeshGetBounds(const GeometryMesh *mesh, Real32 *minimum, Real32 *maximum);

GeometryReadResult geometryMeshMerge(GeometryMesh *merged, const GeometryMesh *const *sources,
                                     Unsigned32 sourceCount, MemoryArena *arena);

void geometryMeshApplyTransform(GeometryMesh *mesh, const Real32 *matrix);

Unsigned32 geometryMeshApplySkin(GeometryMesh *mesh, const Real32 *boneMatrices,
                                 Unsigned32 boneCount);

Unsigned32 geometryMeshApplyMorph(GeometryMesh *mesh, const Real32 *channelWeights,
                                  Unsigned32 weightCount);

#endif
