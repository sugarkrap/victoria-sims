#ifndef VICTORIA_VIRTUAL_FILE_SYSTEM_HEADER
#define VICTORIA_VIRTUAL_FILE_SYSTEM_HEADER

#include "victoria/coreTypes.h"

/* Where the engine's data comes from, without the engine knowing where that is.
 *
 * We ship no game data, so a copy of the game has to be supplied at run time: a
 * disc image, a mounted CD, an installed directory. None of those are things the
 * engine can open for itself. On the WebAssembly build there is no filesystem at
 * all, and the host page holds a File the browser reads from lazily; on Linux it
 * is a file descriptor. Both look the same from here — a store that answers
 * "give me these bytes" — and the engine never learns which it got.
 *
 * The store is never copied into the engine. A retail DVD is more than twenty
 * times the whole memory budget, and both backends can read a range off disk
 * without holding the rest, so nothing is gained by pulling it in. What the
 * engine holds is a catalogue of what is on the disc plus whichever resource it
 * is decoding, and both come out of an arena like everything else.
 *
 * Reads can answer PENDING. That is not a failure and not a mistake: a browser
 * cannot read a file synchronously, so the host is being asked and the caller
 * should come back next frame. Callers must be written as state machines that
 * can be told "not yet" — a blocking read is not implementable on the platform
 * this exists for. The native backend never returns it. */

typedef enum VirtualReadResult
{
    VIRTUAL_READ_OK = 0,
    /* The host has been asked and does not have the bytes yet. Ask again. */
    VIRTUAL_READ_PENDING,
    /* The range lies outside the store. Corruption, not a transient. */
    VIRTUAL_READ_OUT_OF_RANGE,
    VIRTUAL_READ_FAILED
} VirtualReadResult;

const char *virtualReadResultGetName(VirtualReadResult result);

/* A backend. Fills destination with sizeInBytes bytes starting at
 * offsetInBytes, or says why it cannot yet. */
typedef VirtualReadResult (*VirtualStoreRead)(void *context, Unsigned64 offsetInBytes,
                                              MemorySize sizeInBytes, Unsigned8 *destination);

typedef struct VirtualFileEntry
{
    /* Forward slashes, no leading slash. Owned by the arena the catalogue was
     * built in, not by this struct. */
    const char *path;
    Unsigned64 offsetInBytes;
    Unsigned64 sizeInBytes;
} VirtualFileEntry;

typedef struct VirtualFileSystem
{
    VirtualStoreRead read;
    void *readContext;
    Unsigned64 storeSizeInBytes;

    VirtualFileEntry *entries;
    Unsigned32 entryCount;
    Unsigned32 entryCapacity;
} VirtualFileSystem;

void virtualFileSystemInitialize(VirtualFileSystem *fileSystem, VirtualStoreRead read, void *readContext,
                                 Unsigned64 storeSizeInBytes);

/* Reads straight from the store, bypassing the catalogue. This is how the disc
 * reader gets at directory records, which are not files. */
VirtualReadResult virtualFileSystemReadStore(VirtualFileSystem *fileSystem, Unsigned64 offsetInBytes,
                                             MemorySize sizeInBytes, Unsigned8 *destination);

/* Negative when there is no such file. Comparison is case-insensitive: a disc
 * written with 8.3 names and one written with Joliet disagree about case for
 * the same file, and neither spelling is more correct than the other. */
Integer32 virtualFileSystemFind(const VirtualFileSystem *fileSystem, const char *path);

const VirtualFileEntry *virtualFileSystemGetEntry(const VirtualFileSystem *fileSystem, Unsigned32 index);

/* A range within one file. Reading past its end is OUT_OF_RANGE rather than a
 * short read: a caller that asked for more than exists has already gone wrong,
 * and a short read would be parsed as though it were whole. */
VirtualReadResult virtualFileSystemReadFile(VirtualFileSystem *fileSystem, Unsigned32 index,
                                            Unsigned64 offsetInFile, MemorySize sizeInBytes,
                                            Unsigned8 *destination);

/* Adds an entry. Used by the disc reader, and by a host that already knows what
 * it has — a chosen folder arrives as a list of files, with no image to walk.
 * False when the catalogue is full. */
Boolean virtualFileSystemAddEntry(VirtualFileSystem *fileSystem, const char *path, Unsigned64 offsetInBytes,
                                  Unsigned64 sizeInBytes);

#endif
