#ifndef VICTORIA_RESOURCE_COLLECTION_HEADER
#define VICTORIA_RESOURCE_COLLECTION_HEADER

#include "victoria/coreTypes.h"
#include "victoria/packageReader.h"

/* The wrapper every scenegraph resource is written inside.
 *
 * A CRES, a SHPE, a GMND and a GMDC are not four formats. They are four kinds
 * of block inside one container: a version mark, a list of links to resources
 * in other packages, a list of block type identifiers, then the blocks. Only
 * the blocks differ.
 *
 * This exists so that is written once. The geometry reader grew its own copy of
 * the header walk, the string reader and the bounds-checked cursor; adding a
 * shape reader beside it would have made two, and the next one three. One
 * implementation of a format, called from everywhere, is the rule here.
 *
 * Nothing in this file allocates. The cursor points into bytes the caller
 * already owns and every read is bounds checked, so a truncated or hostile
 * resource costs a refusal rather than a fault. */

/* Newest, and the only layout read here. The older two write their file links
   differently and are reported rather than guessed at. */
#define RESOURCE_COLLECTION_MARK 0xFFFF0001UL

/* Names of blocks, resources and primitives. Retail names run to about thirty
   characters ("neighborhood_roundshadow"); one that overruns is truncated,
   because a name is for looking at and losing a mesh over it would be absurd. */
#define RESOURCE_NAME_LIMIT 64UL

/* Links held per collection. A shape naming more subsets than this keeps the
   ones that fit and says how many it had, rather than failing whole. */
#define RESOURCE_COLLECTION_LINK_LIMIT 32U

/* Reads a buffer without ever indexing past its end.
 *
 * A cursor that has overrun stays overrun, so a caller can do a run of reads
 * and test once at the end rather than after every one. Every read past the end
 * yields zero, which means a count read from a truncated buffer comes back as
 * zero — check overran before believing a count, or a short read will look like
 * an empty list. */
typedef struct ResourceCursor
{
    const Unsigned8 *bytes;
    MemorySize sizeInBytes;
    MemorySize position;
    Boolean overran;
} ResourceCursor;

void resourceCursorInitialize(ResourceCursor *cursor, const Unsigned8 *bytes, MemorySize sizeInBytes);

/* Advances by count, or marks the cursor overrun and advances nothing. */
Boolean resourceCursorSkip(ResourceCursor *cursor, MemorySize count);

Unsigned8 resourceCursorReadUnsigned8(ResourceCursor *cursor);
Unsigned16 resourceCursorReadUnsigned16(ResourceCursor *cursor);
Unsigned32 resourceCursorReadUnsigned32(ResourceCursor *cursor);

/* Assembled from bytes rather than cast: the payload is not aligned, and an
   unaligned load is a fault on ARMv5 rather than a slow path. */
Real32 resourceCursorReadReal32(ResourceCursor *cursor);

/* A length prefixed seven bits at a time, high bit set while more follows, then
   that many bytes. Copies what fits and skips the rest. Pass a null destination
   to skip one. */
void resourceCursorReadString(ResourceCursor *cursor, char *destination, MemorySize capacity);

/* The name, type identifier and version that prefix every block and most of the
   structures nested inside them. */
typedef struct PersistTypeInfo
{
    char name[RESOURCE_NAME_LIMIT];
    Unsigned32 typeIdentifier;
    Unsigned32 version;
} PersistTypeInfo;

void resourceCursorReadTypeInformation(ResourceCursor *cursor, PersistTypeInfo *typeInformation);

/* A pointer to another block or to another resource.
 *
 * Internal means a block in this same collection, by index. External means one
 * of the collection's file links, by index — which is how a geometry node
 * reaches the container holding its vertices. */
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
    /* Marked 0xFFFE0001 or 0xFFFD0001: a real collection, older, laid out
       differently. Distinguished from rubbish so a refusal can be acted on. */
    RESOURCE_COLLECTION_OLDER,
    RESOURCE_COLLECTION_TRUNCATED,
    RESOURCE_COLLECTION_NO_BLOCKS
} ResourceCollectionResult;

const char *resourceCollectionResultGetName(ResourceCollectionResult result);

typedef struct ResourceCollection
{
    Unsigned32 versionMark;

    /* What the file declared, which may exceed what is kept. */
    Unsigned32 linkCount;
    Unsigned32 storedLinkCount;
    PackageResourceKey links[RESOURCE_COLLECTION_LINK_LIMIT];

    Unsigned32 blockCount;
    /* The first block's type, from the type list rather than from the block's
       own prefix, so a caller can tell what it has before parsing it. */
    Unsigned32 firstBlockType;
} ResourceCollection;

/* Reads the header and leaves the cursor at the first block's type
   information. The block itself is not parsed: what a block means depends on
   its type, and that belongs to whoever knows the type. */
ResourceCollectionResult resourceCollectionOpen(ResourceCollection *collection, ResourceCursor *cursor,
                                                const Unsigned8 *bytes, MemorySize sizeInBytes);

/* The link at an index, or null when the reference is not an external one that
   was kept. Saves every caller the same three bounds checks. */
const PackageResourceKey *resourceCollectionGetLink(const ResourceCollection *collection,
                                                    ObjectReference reference);

#endif
