#ifndef VICTORIA_RESOURCE_COLLECTION_HEADER
#define VICTORIA_RESOURCE_COLLECTION_HEADER

#include "victoria/coreTypes.h"
#include "victoria/packageReader.h"

#define RESOURCE_COLLECTION_MARK 0xFFFF0001UL

#define RESOURCE_NAME_LIMIT 64UL

#define RESOURCE_COLLECTION_LINK_LIMIT 32U

typedef struct ResourceCursor
{
    const Unsigned8 *bytes;
    MemorySize sizeInBytes;
    MemorySize position;
    Boolean overran;
} ResourceCursor;

void resourceCursorInitialize(ResourceCursor *cursor, const Unsigned8 *bytes, MemorySize sizeInBytes);

Boolean resourceCursorSkip(ResourceCursor *cursor, MemorySize count);

Unsigned8 resourceCursorReadUnsigned8(ResourceCursor *cursor);
Unsigned16 resourceCursorReadUnsigned16(ResourceCursor *cursor);
Unsigned32 resourceCursorReadUnsigned32(ResourceCursor *cursor);

Real32 resourceCursorReadReal32(ResourceCursor *cursor);

void resourceCursorReadString(ResourceCursor *cursor, char *destination, MemorySize capacity);

typedef struct PersistTypeInfo
{
    char name[RESOURCE_NAME_LIMIT];
    Unsigned32 typeIdentifier;
    Unsigned32 version;
} PersistTypeInfo;

void resourceCursorReadTypeInformation(ResourceCursor *cursor, PersistTypeInfo *typeInformation);

typedef enum ObjectReferenceKind
{
    OBJECT_REFERENCE_NONE = 0,
    OBJECT_REFERENCE_INTERNAL,
    OBJECT_REFERENCE_EXTERNAL
} ObjectReferenceKind;

typedef struct ObjectReference
{
    ObjectReferenceKind kind;
    Integer32 index;
} ObjectReference;

ObjectReference resourceCursorReadObjectReference(ResourceCursor *cursor);

typedef enum ResourceCollectionResult
{
    RESOURCE_COLLECTION_OK = 0,
    RESOURCE_COLLECTION_NOT_A_RESOURCE,
    RESOURCE_COLLECTION_OLDER,
    RESOURCE_COLLECTION_TRUNCATED,
    RESOURCE_COLLECTION_NO_BLOCKS
} ResourceCollectionResult;

const char *resourceCollectionResultGetName(ResourceCollectionResult result);

typedef struct ResourceCollection
{
    Unsigned32 versionMark;

    Unsigned32 linkCount;
    Unsigned32 storedLinkCount;
    PackageResourceKey links[RESOURCE_COLLECTION_LINK_LIMIT];

    Unsigned32 blockCount;
    Unsigned32 firstBlockType;
} ResourceCollection;

ResourceCollectionResult resourceCollectionOpen(ResourceCollection *collection, ResourceCursor *cursor,
                                                const Unsigned8 *bytes, MemorySize sizeInBytes);

const PackageResourceKey *resourceCollectionGetLink(const ResourceCollection *collection,
                                                    ObjectReference reference);

#endif
