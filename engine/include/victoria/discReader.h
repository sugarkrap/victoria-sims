#ifndef VICTORIA_DISC_READER_HEADER
#define VICTORIA_DISC_READER_HEADER

#include "victoria/coreTypes.h"
#include "victoria/memoryArena.h"
#include "victoria/virtualFileSystem.h"

/* Walks an ISO 9660 disc image into a virtual file system catalogue.
 *
 * We ship no game data, so a copy of the game is supplied at run time and a
 * disc image is the form it usually takes. Reading one is not the tooling's
 * job alone: the browser build has no other way to be handed a game, and the
 * same code lets any build open an image without it being mounted first.
 *
 * The image is never held. Reads go through the file system's store, which is a
 * file descriptor natively and a File the host page slices in a browser, and
 * what is kept is the catalogue — a path, an offset and a length per file.
 *
 * Because a browser cannot read synchronously, this is a state machine rather
 * than a function: step it until it stops saying PENDING. Retail discs finish
 * in a few dozen steps.
 *
 * Joliet is preferred where present. Retail discs carry both a plain tree with
 * 8.3 names and a Joliet tree with the real ones, and reporting the real ones
 * is what makes a path from here match what a user sees. */

#define DISC_READER_SECTOR_SIZE 2048UL

/* Longest path the reader will record. Retail paths run to about ninety
 * characters; this is generous rather than tight. */
#define DISC_READER_PATH_LIMIT 320UL

/* Bytes of path text reserved per file. Paths that overrun the pool as a whole
 * stop the walk rather than being truncated into something that would not open. */
#define DISC_READER_PATH_BYTES_PER_FILE 128UL

/* One directory's records are parsed from a single buffer. A hundred and
 * twenty-eight kibibytes is upwards of a thousand entries in one directory,
 * which no real disc approaches. */
#define DISC_READER_DIRECTORY_BUFFER_BYTES (128UL * 1024UL)

/* Directories waiting to be walked. Depth is bounded separately. */
#define DISC_READER_PENDING_LIMIT 512U

#define DISC_READER_MAXIMUM_DEPTH 32U

typedef enum DiscReadStatus
{
    DISC_READ_COMPLETE = 0,
    /* More to do, or waiting on the host. Either way: step again. */
    DISC_READ_PENDING,
    DISC_READ_NOT_A_DISC,
    DISC_READ_OUT_OF_ARENA,
    DISC_READ_TOO_MANY_FILES,
    DISC_READ_DIRECTORY_TOO_LARGE,
    DISC_READ_FAILED
} DiscReadStatus;

const char *discReadStatusGetName(DiscReadStatus status);

typedef struct DiscPendingDirectory
{
    Unsigned32 firstSector;
    Unsigned32 lengthInBytes;
    /* Where this directory's path starts in the pool. */
    MemorySize pathOffset;
    Unsigned32 depth;
} DiscPendingDirectory;

typedef struct DiscReader
{
    VirtualFileSystem *fileSystem;
    Integer32 stage;

    Unsigned32 descriptorSector;
    Boolean namesAreUCS2;
    Boolean usesJoliet;
    Boolean primaryFound;
    char volumeIdentifier[33];

    Unsigned32 rootSector;
    Unsigned32 rootLength;

    Unsigned8 *sectorBuffer;
    Unsigned8 *directoryBuffer;

    char *pathPool;
    MemorySize pathPoolCapacity;
    MemorySize pathPoolUsed;

    DiscPendingDirectory *pending;
    Unsigned32 pendingCount;

    /* Counts, so a caller can report progress rather than appearing to hang. */
    Unsigned32 directoriesWalked;
} DiscReader;

/* Claims everything the walk needs from the arena, including the catalogue the
 * file system will point at. Roughly fileLimit * 152 bytes plus a fixed 128 KiB
 * for directory records — under a megabyte for a retail disc. */
DiscReadStatus discReaderBegin(DiscReader *reader, VirtualFileSystem *fileSystem, MemoryArena *arena,
                               Unsigned32 fileLimit);

DiscReadStatus discReaderStep(DiscReader *reader);

/* Steps until the walk finishes or fails. Only for a store that never answers
 * PENDING — a file descriptor, or a buffer already in memory. Calling it on a
 * browser's store would spin. */
DiscReadStatus discReaderRunToCompletion(DiscReader *reader);

#endif
