#ifndef VICTORIA_MATERIAL_HEADER
#define VICTORIA_MATERIAL_HEADER

#include "victoria/coreTypes.h"
#include "victoria/resourceCollection.h"

/* Reads a TXMT — a cMaterialDefinition — far enough to find its texture.
 *
 * A shape says which material each of its parts wears, by name. The material
 * says which texture it draws with, also by name. Neither uses an identifier,
 * so the whole hop is string matching, and the names have suffixes: a part
 * wearing "ufocrash_cabin" wants the resource "ufocrash_cabin_txmt", which
 * names the texture "ufocrash-cabin", which lives in "ufocrash-cabin_txtr".
 *
 * A retail material carries about forty properties — culling, alpha modes,
 * texture coordinate animation. All of them are read, because a property list
 * has to be walked to get past it, and one of them is kept: the base texture's
 * name. The rest are skipped rather than stored, because storing forty strings
 * per material to use one of them would cost more than the texture.
 *
 * Nothing here allocates. */

#define MATERIAL_TEXTURE_LIMIT 8U

typedef enum MaterialReadResult
{
    MATERIAL_READ_OK = 0,
    MATERIAL_READ_NOT_A_RESOURCE,
    MATERIAL_READ_OLDER_COLLECTION,
    MATERIAL_READ_WRONG_TYPE,
    MATERIAL_READ_TRUNCATED,
    MATERIAL_READ_UNSUPPORTED_VERSION
} MaterialReadResult;

const char *materialReadResultGetName(MaterialReadResult result);

typedef struct MaterialDescription
{
    char resourceName[RESOURCE_NAME_LIMIT];
    /* The name a shape refers to it by, without the suffix. */
    char materialName[RESOURCE_NAME_LIMIT];
    /* "StandardMaterial" for everything met so far. */
    char definitionType[RESOURCE_NAME_LIMIT];
    Unsigned32 blockVersion;

    /* From the stdMatBaseTextureName property. Empty when the material has
       none, which is a material that paints with a colour rather than an
       image. */
    char baseTextureName[RESOURCE_NAME_LIMIT];

    /* The material's own texture list, which usually repeats the base texture
       and occasionally holds more. */
    Unsigned32 textureCount;
    Unsigned32 storedTextureCount;
    char textureNames[MATERIAL_TEXTURE_LIMIT][RESOURCE_NAME_LIMIT];
} MaterialDescription;

MaterialReadResult materialRead(MaterialDescription *material, const Unsigned8 *bytes,
                                MemorySize sizeInBytes);

/* Appends a suffix to a name, into a buffer the caller owns. The chain between
   these resources is spelled rather than numbered, and every hop needs this. */
void materialBuildResourceName(char *destination, MemorySize capacity, const char *name,
                               const char *suffix);

#endif
