#ifndef VICTORIA_DISC_CONTENT_HEADER
#define VICTORIA_DISC_CONTENT_HEADER

#include "victoria/coreTypes.h"
#include "victoria/geometryReader.h"
#include "victoria/memoryArena.h"
#include "victoria/virtualFileSystem.h"

/* Finds something to draw on a disc.
 *
 * Given a catalogue, this opens each package in turn and stops at the first one
 * carrying geometry it can read. That is a deliberately blunt rule: there is no
 * scenegraph traversal here, no CRES to SHPE to GMND chain, because none of that
 * is needed to answer the question this exists to answer — is the whole path
 * from a disc to a triangle actually connected.
 *
 * It counts what it passed over on the way. A retail disc will have every
 * package refused for the same reason, and a caller that can only say "nothing
 * found" cannot tell that from an empty disc. */

#define DISC_CONTENT_PATH_LIMIT 320UL

/* Container versions 0 to 14, then everything above in the last. */
#define DISC_CONTENT_VERSION_BUCKETS 16U

typedef enum DiscContentStatus
{
    DISC_CONTENT_FOUND = 0,
    /* More packages to try; step again. Also covers a store that has not
     * answered yet, which is what a browser's does. */
    DISC_CONTENT_PENDING,
    DISC_CONTENT_NONE_FOUND,
    DISC_CONTENT_OUT_OF_ARENA
} DiscContentStatus;

const char *discContentStatusGetName(DiscContentStatus status);

typedef struct DiscContentSearch
{
    VirtualFileSystem *fileSystem;
    MemoryArena *arena;
    MemorySize arenaMarker;
    Unsigned32 nextIndex;

    /* Filled in when the status is FOUND. */
    char packagePath[DISC_CONTENT_PATH_LIMIT];
    GeometryMesh mesh;

    /* What the search met on the way, so a report can be specific. */
    Unsigned32 packagesOpened;
    Unsigned32 packagesCompressed;
    Unsigned32 packagesWithGeometry;
    Unsigned32 geometryRefused;
    /* Why the refusals happened, one bucket per GeometryReadResult. A disc that
       refuses hundreds of meshes for one reason and a disc that refuses them for
       six are different problems, and a bare count cannot tell them apart. */
    Unsigned32 refusalsByReason[GEOMETRY_READ_RESULT_COUNT];
    /* Refusals that never reached the geometry reader because the stream would
       not decompress. */
    Unsigned32 decompressionRefused;

    /* Which container versions the disc actually holds, read or not. A reason
       says what this engine did; this says what the game shipped, which is the
       part no amount of reasoning about the reader can supply. Anything past
       the last bucket lands in it. */
    Unsigned32 versionsSeen[DISC_CONTENT_VERSION_BUCKETS];
    /* Collection marks that were not 0xFFFF0001, most recent first seen. Kept
       whole rather than bucketed: there are only a few, and knowing it was
       0xFFFE0001 rather than "some other mark" is the whole question. */
    Unsigned32 firstUnknownMark;
} DiscContentSearch;

void discContentBegin(DiscContentSearch *search, VirtualFileSystem *fileSystem, MemoryArena *arena);

/* Tries one package. */
DiscContentStatus discContentStep(DiscContentSearch *search);

/* Steps until it finds something or runs out. Only for a store that never
 * answers PENDING — a file descriptor, or bytes already in memory. */
DiscContentStatus discContentRunToCompletion(DiscContentSearch *search);

#endif
