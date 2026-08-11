#ifndef VICTORIA_DISC_READER_HEADER
#define VICTORIA_DISC_READER_HEADER

#include "victoria/coreTypes.h"
#include "victoria/memoryArena.h"
#include "victoria/virtualFileSystem.h"

#define DISC_READER_SECTOR_SIZE 2048UL

#define DISC_READER_PATH_LIMIT 320UL

#define DISC_READER_PATH_BYTES_PER_FILE 128UL

#define DISC_READER_DIRECTORY_BUFFER_BYTES (128UL * 1024UL)

#define DISC_READER_PENDING_LIMIT 512U

#define DISC_READER_MAXIMUM_DEPTH 32U

typedef enum DiscReadStatus
{
    DISC_READ_COMPLETE = 0,
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

    Unsigned32 directoriesWalked;
} DiscReader;

DiscReadStatus discReaderBegin(DiscReader *reader, VirtualFileSystem *fileSystem, MemoryArena *arena,
                               Unsigned32 fileLimit);

DiscReadStatus discReaderStep(DiscReader *reader);

DiscReadStatus discReaderRunToCompletion(DiscReader *reader);

#endif
