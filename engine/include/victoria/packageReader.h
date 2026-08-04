#ifndef VICTORIA_PACKAGE_READER_HEADER
#define VICTORIA_PACKAGE_READER_HEADER

#include "victoria/coreTypes.h"
#include "victoria/memoryArena.h"

/* Reads the DBPF container: header and index only, no resource decoding.
 *
 * There is no file I/O here on purpose. The reader is handed bytes that already
 * exist and never reads from disk itself, which keeps it identical on every
 * platform — including the WebAssembly build, which has no filesystem — and
 * means a test can point it at a buffer without a platform layer underneath.
 *
 * The index is the only thing allocated, and it comes from the caller's arena.
 * A package whose index does not fit is rejected rather than truncated. */

#define PACKAGE_TYPE_CRES 0xE519C933UL
#define PACKAGE_TYPE_SHPE 0xFC6EB1F7UL
#define PACKAGE_TYPE_GMND 0x7BA3838CUL
#define PACKAGE_TYPE_GMDC 0xAC4F8687UL
#define PACKAGE_TYPE_TXMT 0x49596978UL
#define PACKAGE_TYPE_TXTR 0x1C4A276CUL
#define PACKAGE_TYPE_LIFO 0xED534136UL
#define PACKAGE_TYPE_ANIM 0xFB00791EUL
#define PACKAGE_TYPE_DIRECTORY 0xE86B1EEFUL

/* A resource's identity. All four words together: two resources differing only
 * in the high instance word are distinct, and a reader that ignores it will
 * collide them.
 *
 * Which of the two instance words the format calls "high" is not settled here.
 * Every entry in the fixtures has 0xFF in the top byte of the third word, which
 * is suggestive but not proof, and it does not matter for lookup: identity is
 * the whole tuple either way. Do not rely on the naming until it has been
 * checked against retail data. */
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

/* Points into the buffer the package was opened over. Null if the resource is
 * not wholly inside it. */
const Unsigned8 *packageReaderGetResourceBytes(const Package *package, const PackageResource *resource);

/* True when the package carries a compression directory, which is how a reader
 * knows some of its resources are compressed. Decompression is not implemented;
 * none of the current fixtures need it. */
Boolean packageReaderHasCompressedResources(const Package *package);

#endif
