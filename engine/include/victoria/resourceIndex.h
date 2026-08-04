#ifndef VICTORIA_RESOURCE_INDEX_HEADER
#define VICTORIA_RESOURCE_INDEX_HEADER

#include "victoria/coreTypes.h"
#include "victoria/memoryArena.h"
#include "victoria/packageReader.h"
#include "victoria/virtualFileSystem.h"

/* Where everything of a given kind lives, across a whole disc.
 *
 * A Sim's face material names its texture, and that texture is not in the
 * package the Sim is in — it is in one of the six hundred others. Finding it
 * means looking outside the package, and looking outside the package means
 * knowing what is in all of them.
 *
 * This is affordable only because a scenegraph resource's key is its name
 * hashed. Without that, finding a texture by name would mean opening every
 * package, decompressing every texture and reading the name inside it — six
 * hundred packages of content. With it, the name gives a key, and a key can be
 * matched against index entries. So this reads each package's header and index
 * and nothing else: a few hundred bytes and a few kilobytes per file, rather
 * than the file.
 *
 * Only the types asked for are kept. A retail disc holds far more resources
 * than there is room to remember, and remembering the ones nobody will ask for
 * is how a budget gets spent on nothing.
 *
 * Built a step at a time, because on the web every read has to go back to the
 * browser's event loop before it can answer. */

#define RESOURCE_INDEX_TYPE_LIMIT 8U

/* Where one resource is. Deliberately not a PackageResource: the group is
   dropped, because a lookup by hashed name does not know it and matching on it
   would fail every time. */
typedef struct ResourceIndexEntry
{
    Unsigned32 typeIdentifier;
    Unsigned32 instanceIdentifier;
    Unsigned32 instanceIdentifierHigh;
    /* Which file in the catalogue, and where inside it. */
    Unsigned32 fileIndex;
    Unsigned32 offsetInBytes;
    Unsigned32 sizeInBytes;
} ResourceIndexEntry;

typedef enum ResourceIndexStatus
{
    RESOURCE_INDEX_COMPLETE = 0,
    /* More to do; step again. Also covers a store that has not answered yet. */
    RESOURCE_INDEX_WORKING,
    RESOURCE_INDEX_OUT_OF_ROOM
} ResourceIndexStatus;

const char *resourceIndexStatusGetName(ResourceIndexStatus status);

typedef struct ResourceIndex
{
    VirtualFileSystem *fileSystem;
    MemoryArena *arena;

    ResourceIndexEntry *entries;
    Unsigned32 capacity;
    Unsigned32 count;
    /* Entries that would have been kept but did not fit. A full index that
       cannot say it is full is a lookup that fails for a reason nobody can
       see. */
    Unsigned32 dropped;

    Unsigned32 wantedTypes[RESOURCE_INDEX_TYPE_LIMIT];
    Unsigned32 wantedTypeCount;

    /* Where the walk is. */
    Unsigned32 nextFileIndex;
    Boolean readingHeader;
    Unsigned32 filesIndexed;
    Unsigned32 filesRefused;

    /* The package currently being read. */
    Unsigned32 pendingEntryCount;
    Unsigned32 pendingIndexOffset;
    Unsigned32 pendingIndexSize;
} ResourceIndex;

/* Begins an index over every package in the catalogue, keeping only resources
   whose type is in the list. Returns false when the arena cannot hold the
   entry table. */
Boolean resourceIndexBegin(ResourceIndex *index, VirtualFileSystem *fileSystem, MemoryArena *arena,
                           Unsigned32 entryCapacity, const Unsigned32 *wantedTypes,
                           Unsigned32 wantedTypeCount);

/* Reads a little more. Call until it stops saying WORKING. */
ResourceIndexStatus resourceIndexStep(ResourceIndex *index);

/* The entry with this type and these instance words, or null. The group is not
   part of the match, on purpose. */
const ResourceIndexEntry *resourceIndexFind(const ResourceIndex *index, Unsigned32 typeIdentifier,
                                            Unsigned32 instanceIdentifier,
                                            Unsigned32 instanceIdentifierHigh);

/* The same, given a name rather than a key. */
const ResourceIndexEntry *resourceIndexFindNamed(const ResourceIndex *index,
                                                 Unsigned32 typeIdentifier, const char *name);

#endif
