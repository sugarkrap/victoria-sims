#ifndef VICTORIA_SCENEGRAPH_HEADER
#define VICTORIA_SCENEGRAPH_HEADER

#include "victoria/coreTypes.h"
#include "victoria/packageReader.h"
#include "victoria/resourceCollection.h"

#define SCENEGRAPH_MESH_LIMIT 8U
#define SCENEGRAPH_MATERIAL_LIMIT 8U

typedef enum ScenegraphReadResult
{
    SCENEGRAPH_READ_OK = 0,
    SCENEGRAPH_READ_NOT_A_RESOURCE,
    SCENEGRAPH_READ_OLDER_COLLECTION,
    SCENEGRAPH_READ_WRONG_TYPE,
    SCENEGRAPH_READ_TRUNCATED,
    SCENEGRAPH_READ_NO_REFERENCE,
    SCENEGRAPH_READ_UNSUPPORTED_VERSION
} ScenegraphReadResult;

const char *scenegraphReadResultGetName(ScenegraphReadResult result);

typedef struct ShapeMaterialBinding
{
    char primitiveName[RESOURCE_NAME_LIMIT];
    char materialName[RESOURCE_NAME_LIMIT];
} ShapeMaterialBinding;

typedef struct ShapeDescription
{
    char resourceName[RESOURCE_NAME_LIMIT];
    Unsigned32 blockVersion;

    Unsigned32 meshCount;
    Unsigned32 storedMeshCount;
    char meshNames[SCENEGRAPH_MESH_LIMIT][RESOURCE_NAME_LIMIT];
    Unsigned32 meshLevelsOfDetail[SCENEGRAPH_MESH_LIMIT];

    Unsigned32 materialCount;
    Unsigned32 storedMaterialCount;
    ShapeMaterialBinding materials[SCENEGRAPH_MATERIAL_LIMIT];
} ShapeDescription;

ScenegraphReadResult scenegraphReadShape(ShapeDescription *shape, const Unsigned8 *bytes,
                                         MemorySize sizeInBytes);

typedef struct GeometryNodeDescription
{
    char resourceName[RESOURCE_NAME_LIMIT];
    Unsigned32 blockVersion;

    Boolean hasGeometry;
    PackageResourceKey geometryKey;
} GeometryNodeDescription;

ScenegraphReadResult scenegraphReadGeometryNode(GeometryNodeDescription *node, const Unsigned8 *bytes,
                                                MemorySize sizeInBytes);

const PackageResource *scenegraphFindResource(const Package *package, const PackageResourceKey *key);

const PackageResource *scenegraphFindResourceByInstance(const Package *package,
                                                        Unsigned32 typeIdentifier,
                                                        Unsigned32 instanceIdentifier,
                                                        Unsigned32 instanceIdentifierHigh);

const Unsigned8 *scenegraphReadResourceBytes(MemoryArena *arena, const Package *package,
                                             const PackageResource *resource, MemorySize *sizeInBytes,
                                             Boolean *wasCompressed);

const PackageResource *scenegraphFindGeometryNamed(MemoryArena *arena, const Package *package,
                                                   const char *nodeName);

#endif
