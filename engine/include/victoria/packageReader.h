#ifndef VICTORIA_PACKAGE_READER_HEADER
#define VICTORIA_PACKAGE_READER_HEADER

#include "victoria/coreTypes.h"
#include "victoria/memoryArena.h"

#define PACKAGE_TYPE_CRES 0xE519C933UL
#define PACKAGE_TYPE_SHPE 0xFC6EB1F7UL
#define PACKAGE_TYPE_GMND 0x7BA3838CUL
#define PACKAGE_TYPE_GMDC 0xAC4F8687UL
#define PACKAGE_TYPE_TXMT 0x49596978UL
#define PACKAGE_TYPE_TXTR 0x1C4A276CUL
#define PACKAGE_TYPE_LIFO 0xED534136UL
#define PACKAGE_TYPE_ANIM 0xFB00791EUL
#define PACKAGE_TYPE_SKIN_ENTRY 0xEBCF3E27UL
#define PACKAGE_TYPE_RESOURCE_KEY_LIST 0xAC506764UL
#define PACKAGE_TYPE_SIM_APPEARANCE 0xAC598EACUL
#define PACKAGE_TYPE_DIRECTORY 0xE86B1EEFUL

typedef struct PackageResourceKey
{
    Unsigned32 typeIdentifier;
    Unsigned32 groupIdentifier;
    Unsigned32 instanceIdentifier;
    Unsigned32 instanceIdentifierHigh;
} PackageResourceKey;

typedef struct PackageResource
{
    PackageResourceKey key;
    Unsigned32 offsetInBytes;
    Unsigned32 sizeInBytes;
} PackageResource;

typedef struct Package
{
    const Unsigned8 *bytes;
    MemorySize sizeInBytes;
    Unsigned32 majorVersion;
    Unsigned32 minorVersion;
    Unsigned32 resourceCount;
    const PackageResource *resources;
} Package;

typedef enum PackageReadResult
{
    PACKAGE_READ_OK = 0,
    PACKAGE_READ_NOT_A_PACKAGE,
    PACKAGE_READ_TRUNCATED,
    PACKAGE_READ_BAD_INDEX,
    PACKAGE_READ_OUT_OF_ARENA
} PackageReadResult;

PackageReadResult packageReaderOpen(Package *package, const Unsigned8 *bytes, MemorySize sizeInBytes,
                                    MemoryArena *arena);

const char *packageReadResultGetName(PackageReadResult result);

Unsigned32 packageReaderCountResourcesOfType(const Package *package, Unsigned32 typeIdentifier);
const PackageResource *packageReaderFindFirstOfType(const Package *package, Unsigned32 typeIdentifier);

const Unsigned8 *packageReaderGetResourceBytes(const Package *package, const PackageResource *resource);

Boolean packageReaderHasCompressedResources(const Package *package);

#endif
