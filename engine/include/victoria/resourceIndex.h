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

/* How many resource types one index may be asked for.
 *
 * Was eight, which was exactly the number a Sim needed until a ninth was added
 * for the catalogue's sidecar — and the ninth was clamped away in silence, so
 * the hop that needed it resolved nothing while the log said the disc simply
 * had none. Twice what is needed now, and asking for more than this is
 * reported rather than trimmed. */
#define RESOURCE_INDEX_TYPE_LIMIT 16U

/* Distinct resource types the census remembers.

   Forty eight was the first guess and a retail disc filled it exactly, which
   is the shape of a cap being hit rather than a disc that happens to use
   forty eight types. Whatever was first met after the forty eighth went
   uncounted, and the count that would have said so was gathered and never
   printed — a counter nobody reads is not a diagnostic.

   The Sims 2 defines fewer than a hundred types, so this has room to spare and
   the overflow is now reported either way. */
#define RESOURCE_INDEX_CENSUS_LIMIT 256U

/* Where one resource is. Deliberately not a PackageResource: the group is
   dropped, because a lookup by hashed name does not know it and matching on it
   would fail every time. */
typedef struct ResourceIndexEntry
{
    Unsigned32 typeIdentifier;
    /* Which collection the resource was filed under. Not part of a lookup by
       name — a name hashes to the instance words and says nothing about the
       group — but it is what tells two resources apart when they share an
       instance, which sidecar resources routinely do. */
    Unsigned32 groupIdentifier;
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
    /* Types asked for and not taken, because there were more than the limit. A
       lookup for one of those finds nothing, which is indistinguishable from a
       disc that holds none of it — so the number is kept and reported rather
       than the request being quietly trimmed. */
    Unsigned32 wantedTypesRefused;
    /* Kept per type, and a total of every entry met whether wanted or not.
       A search that finds nothing needs to distinguish "the disc holds few of
       these" from "the disc holds none and I am looking for the wrong thing",
       and only a count of what was actually there can do that. */
    Unsigned32 countByType[RESOURCE_INDEX_TYPE_LIMIT];
    Unsigned32 entriesSeen;

    /* A census of every type met, wanted or not, most common first.
       Looking for one type and finding few of it says nothing about whether
       the disc is unusual or the search is. A tally of what is actually there
       answers that without another round trip. */
    Unsigned32 censusTypes[RESOURCE_INDEX_CENSUS_LIMIT];
    Unsigned32 censusCounts[RESOURCE_INDEX_CENSUS_LIMIT];
    Unsigned32 censusCount;
    /* Entries whose type did not fit in the census. */
    Unsigned32 censusOverflow;

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

/* The same, with the group as well.
 *
 * For a sidecar: a resource that shares the group AND instance of the one it
 * belongs to. Matching on instance alone finds a key list, but not reliably the
 * right one — 266 of 600 catalogue entries resolved against a list belonging to
 * some other resource that happened to share their instance, and every one of
 * those looked like an entry indexing past the end of its own list rather than
 * like a wrong list. */
const ResourceIndexEntry *resourceIndexFindInGroup(const ResourceIndex *index,
                                                   Unsigned32 typeIdentifier,
                                                   Unsigned32 groupIdentifier,
                                                   Unsigned32 instanceIdentifier,
                                                   Unsigned32 instanceIdentifierHigh);

/* The same, given a name rather than a key. */
const ResourceIndexEntry *resourceIndexFindNamed(const ResourceIndex *index,
                                                 Unsigned32 typeIdentifier, const char *name);

/* The nth most common type on the disc, by count. False when there is no nth. */
Boolean resourceIndexGetCensusRank(const ResourceIndex *index, Unsigned32 rank,
                                   Unsigned32 *typeIdentifier, Unsigned32 *count);

#endif
