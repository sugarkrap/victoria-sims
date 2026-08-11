#ifndef VICTORIA_MATERIAL_HEADER
#define VICTORIA_MATERIAL_HEADER

#include "victoria/coreTypes.h"
#include "victoria/resourceCollection.h"

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
    char materialName[RESOURCE_NAME_LIMIT];
    char definitionType[RESOURCE_NAME_LIMIT];
    Unsigned32 blockVersion;

    char baseTextureName[RESOURCE_NAME_LIMIT];

    Unsigned32 textureCount;
    Unsigned32 storedTextureCount;
    char textureNames[MATERIAL_TEXTURE_LIMIT][RESOURCE_NAME_LIMIT];
} MaterialDescription;

MaterialReadResult materialRead(MaterialDescription *material, const Unsigned8 *bytes,
                                MemorySize sizeInBytes);

void materialBuildResourceName(char *destination, MemorySize capacity, const char *name,
                               const char *suffix);

#endif
