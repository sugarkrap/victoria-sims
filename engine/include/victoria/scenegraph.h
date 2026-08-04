#ifndef VICTORIA_SCENEGRAPH_HEADER
#define VICTORIA_SCENEGRAPH_HEADER

#include "victoria/coreTypes.h"
#include "victoria/packageReader.h"
#include "victoria/resourceCollection.h"

/* The indirection between a model's name and its vertices.
 *
 * A GMDC on its own is a bag of triangles with no idea what it belongs to. The
 * chain that gives it meaning runs CRES to SHPE to GMND to GMDC: the resource
 * node names the model and holds its transform tree, the shape says which
 * meshes make it up and what material each part wears, the geometry node is a
 * named handle, and only the container holds vertices.
 *
 * Two of those hops are here. A shape gives mesh names; a geometry node turns
 * one of those names into the key of the container holding its vertices. That
 * is enough to ask a package for a model by name instead of taking whichever
 * mesh happens to come first, which is what the engine has been doing.
 *
 * The transform tree is not read yet. Without it a model with separate parts
 * has them all at the origin, which is right for a Sim's body and wrong for a
 * door in a wall.
 *
 * Nothing here allocates. Everything is written into a description the caller
 * owns, and a list longer than the description holds keeps what fits and says
 * how many there were, rather than failing whole over a mesh nobody asked
 * for. */

#define SCENEGRAPH_MESH_LIMIT 8U
#define SCENEGRAPH_MATERIAL_LIMIT 8U

typedef enum ScenegraphReadResult
{
    SCENEGRAPH_READ_OK = 0,
    SCENEGRAPH_READ_NOT_A_RESOURCE,
    SCENEGRAPH_READ_OLDER_COLLECTION,
    SCENEGRAPH_READ_WRONG_TYPE,
    SCENEGRAPH_READ_TRUNCATED,
    /* Read, but carrying no reference to follow. A geometry node below block
       version 8 genuinely has none — the game stops reading there too. */
    SCENEGRAPH_READ_NO_REFERENCE,
    SCENEGRAPH_READ_UNSUPPORTED_VERSION
} ScenegraphReadResult;

const char *scenegraphReadResultGetName(ScenegraphReadResult result);

/* One part of a shape: the primitive it names inside the container, and the
   material that part wears. */
typedef struct ShapeMaterialBinding
{
    char primitiveName[RESOURCE_NAME_LIMIT];
    char materialName[RESOURCE_NAME_LIMIT];
} ShapeMaterialBinding;

typedef struct ShapeDescription
{
    char resourceName[RESOURCE_NAME_LIMIT];
    Unsigned32 blockVersion;

    /* The geometry nodes this shape is built from, by name. Retail shapes name
       one per level of detail; the level is kept so a caller can prefer the
       nearest rather than whichever came first. */
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

    /* Where the vertices are. Only meaningful when the read returned OK. */
    Boolean hasGeometry;
    PackageResourceKey geometryKey;
} GeometryNodeDescription;

ScenegraphReadResult scenegraphReadGeometryNode(GeometryNodeDescription *node, const Unsigned8 *bytes,
                                                MemorySize sizeInBytes);

/* The resource with this key, or null. The package index is not sorted, so this
   is a walk — which is fine at a few hundred entries and would not be at a few
   hundred thousand. */
const PackageResource *scenegraphFindResource(const Package *package, const PackageResourceKey *key);

#endif
