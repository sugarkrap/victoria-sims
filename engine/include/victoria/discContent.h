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
} DiscContentSearch;

void discContentBegin(DiscContentSearch *search, VirtualFileSystem *fileSystem, MemoryArena *arena);

/* Tries one package. */
DiscContentStatus discContentStep(DiscContentSearch *search);

/* Steps until it finds something or runs out. Only for a store that never
 * answers PENDING — a file descriptor, or bytes already in memory. */
DiscContentStatus discContentRunToCompletion(DiscContentSearch *search);

#endif
