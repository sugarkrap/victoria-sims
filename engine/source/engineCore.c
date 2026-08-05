#include "victoria/discContent.h"
#include "victoria/discReader.h"
#include "victoria/engineCore.h"
#include "utils/strings.h"
#include "victoria/graphicsMemoryBudget.h"
#include "victoria/memoryBudget.h"
#include "victoria/platformInterface.h"
#include "victoria/profiler.h"
#include "victoria/renderInterface.h"
#include "victoria/compression.h"
#include "victoria/resourceIndex.h"
#include "victoria/installerReader.h"
#include "victoria/archiveReader.h"
#include "victoria/programReader.h"
#include "victoria/textureDecode.h"

/* How often the text report is regenerated. Formatting is cheap but not free,
   and nothing reads it faster than a human can. */
#define ENGINE_REPORT_INTERVAL_MICROSECONDS 250000ULL

static MemoryArena *globalArena = NULL_POINTER;
static Boolean engineIsRunning = BOOLEAN_FALSE;
static char *profilerReportText = NULL_POINTER;
static Unsigned64 lastReportMicroseconds = 0ULL;

static void logMemoryBudget(void)
{
    char message[96];

    message[0] = '\0';
    stringAppend(message, sizeof(message), "memory budget: ");
    stringWriteUnsigned(message + stringLength(message),
                        sizeof(message) - stringLength(message),
                        globalArena->totalSizeInBytes / 1024UL / 1024UL);
    stringAppend(message, sizeof(message), " MiB reserved, ");
    stringWriteUnsigned(message + stringLength(message),
                        sizeof(message) - stringLength(message),
                        globalArena->usedSizeInBytes);
    stringAppend(message, sizeof(message), " bytes used");
    platformLogMessage(message);
}

static void establishGraphicsMemoryLimit(MemorySize overrideBytes)
{
    MemorySize detectedBytes;

    if (overrideBytes > 0UL)
    {
        graphicsMemoryBudgetInitialize(overrideBytes);
        graphicsMemoryBudgetSetLimitSource(GRAPHICS_MEMORY_LIMIT_SOURCE_OVERRIDE);
        platformLogMessage("graphics memory: limit taken from override");
        return;
    }

    detectedBytes = renderQueryGraphicsMemoryBytes();
    graphicsMemoryBudgetInitialize(detectedBytes);
    graphicsMemoryBudgetSetLimitSource(detectedBytes > 0UL ? GRAPHICS_MEMORY_LIMIT_SOURCE_DETECTED
                                                           : GRAPHICS_MEMORY_LIMIT_SOURCE_DEFAULT);

    if (detectedBytes == 0UL)
    {
        /* Neither OpenGL ES 2.0 nor WebGPU can be asked portably, so this is
           the common path rather than an error. */
        platformLogMessage("graphics memory: backend did not report a size, assuming the default");
    }
}


/* How many files a disc catalogue can hold.
 *
 * A retail disc lists a few hundred. This one lists 745, and then one of those
 * turns out to be an archive holding the whole installed game, every stored
 * entry of which becomes a file in the same catalogue. Sixteen thousand entries
 * cost about two and a half mebibytes of the budget, which is affordable; being
 * unable to see the game is not. */
#define DISC_FILE_LIMIT 16384U

static void appendCount(char *destination, MemorySize capacity, Unsigned32 value)
{
    char digits[24];

    if (stringWriteUnsigned(digits, sizeof(digits), (Unsigned64)value) > 0UL)
    {
        stringAppend(destination, capacity, digits);
    }
}

static void appendHexadecimal(char *destination, MemorySize capacity, Unsigned32 value)
{
    char digits[24];

    if (stringWriteHexadecimal(digits, sizeof(digits), (Unsigned64)value, 8UL) > 0UL)
    {
        stringAppend(destination, capacity, digits);
    }
}

/* Bytes as hexadecimal pairs, so a reader can match them against whatever
   reference documents the format. */
static void appendHexadecimalBytes(char *destination, MemorySize capacity, const Unsigned8 *bytes,
                                   MemorySize byteCount, MemorySize from, MemorySize howMany)
{
    MemorySize index;

    for (index = 0UL; index < howMany && from + index < byteCount; index++)
    {
        char digits[8];

        if (stringWriteHexadecimal(digits, sizeof(digits), (Unsigned64)bytes[from + index], 2UL) > 0UL)
        {
            stringAppend(destination, capacity, digits + 2UL);
            stringAppend(destination, capacity, " ");
        }
    }
}

/* Kibibytes, except below a kibibyte, where they would all read as zero and a
   ten byte file would be indistinguishable from an empty one. */
static void appendByteSize(char *destination, MemorySize capacity, Unsigned64 byteCount)
{
    char digits[24];
    Boolean asKibibytes = (Boolean)(byteCount >= 1024ULL);

    if (stringWriteUnsigned(digits, sizeof(digits),
                            asKibibytes ? byteCount / 1024ULL : byteCount) > 0UL)
    {
        stringAppend(destination, capacity, digits);
        stringAppend(destination, capacity, asKibibytes ? " KiB" : " bytes");
    }
}

/* How many of the disc's other files get named in the log. Enough to recognise
   an installer payload or an archive format by sight; not so many that a disc
   of loose files buries everything else. */
#define CATALOGUE_LISTING_LIMIT 12U

/* The next largest file that is not a package, after the one at
   (ceilingSize, ceilingIndex). False when there is no next.
 *
 * Largest first, because a file big enough to hold a disc's worth of art is the
 * only one worth looking at. Ties break by position, so a run of identically
 * sized files is walked rather than the first of them being picked over and
 * over — which is not hypothetical: a disc's loose files include several that
 * are exactly one sector.
 *
 * A selection scan rather than a sort: nothing may be moved, because the
 * catalogue's order is the file index every read is addressed by. */
static Boolean findNextLargestOther(const VirtualFileSystem *fileSystem, Unsigned64 ceilingSize,
                                    Unsigned32 ceilingIndex, Unsigned64 *foundSize,
                                    Unsigned32 *foundIndex)
{
    Unsigned32 which;
    Boolean found = BOOLEAN_FALSE;

    for (which = 0U; which < fileSystem->entryCount; which++)
    {
        const VirtualFileEntry *entry = virtualFileSystemGetEntry(fileSystem, which);
        Boolean belowCeiling;

        if (entry == NULL_POINTER || stringEndsWithIgnoringCase(entry->path, ".package"))
        {
            continue;
        }
        belowCeiling = (Boolean)(entry->sizeInBytes < ceilingSize ||
                                 (entry->sizeInBytes == ceilingSize && which > ceilingIndex));
        if (!belowCeiling)
        {
            continue;
        }
        if (!found || entry->sizeInBytes > *foundSize)
        {
            *foundSize = entry->sizeInBytes;
            *foundIndex = which;
            found = BOOLEAN_TRUE;
        }
    }
    return found;
}

/* What is on the disc besides packages.
 *
 * Six hundred packages hold a hundred and twenty seven images between them,
 * which is not what a disc with Sims on it looks like. The packages have been
 * counted and the resources in them have been counted; the files that are not
 * packages have never been looked at, and they are the only part of the disc
 * this engine has never had an opinion about.
 *
 * Nothing here reads a byte. The catalogue already knows every path and every
 * length, because the walk that built it had to. */
void engineReportDiscCatalogue(const VirtualFileSystem *fileSystem)
{
    char message[512];
    Unsigned32 which;
    Unsigned32 packageCount = 0U;
    Unsigned64 packageBytes = 0ULL;
    Unsigned32 otherCount = 0U;
    Unsigned64 otherBytes = 0ULL;
    Unsigned64 ceilingSize = 0xFFFFFFFFFFFFFFFFULL;
    Unsigned32 ceilingIndex = 0U;
    Unsigned32 listed;

    for (which = 0U; which < fileSystem->entryCount; which++)
    {
        const VirtualFileEntry *entry = virtualFileSystemGetEntry(fileSystem, which);

        if (entry == NULL_POINTER)
        {
            continue;
        }
        if (stringEndsWithIgnoringCase(entry->path, ".package"))
        {
            packageCount++;
            packageBytes += entry->sizeInBytes;
        }
        else
        {
            otherCount++;
            otherBytes += entry->sizeInBytes;
        }
    }

    message[0] = '\0';
    stringAppend(message, sizeof(message), "engine: ");
    appendCount(message, sizeof(message), packageCount);
    stringAppend(message, sizeof(message), " package(s) totalling ");
    appendByteSize(message, sizeof(message), packageBytes);
    stringAppend(message, sizeof(message), ", and ");
    appendCount(message, sizeof(message), otherCount);
    stringAppend(message, sizeof(message), " other file(s) totalling ");
    appendByteSize(message, sizeof(message), otherBytes);
    platformLogMessage(message);

    for (listed = 0U; listed < CATALOGUE_LISTING_LIMIT; listed++)
    {
        Unsigned64 bestSize = 0ULL;
        Unsigned32 bestIndex = 0U;

        if (!findNextLargestOther(fileSystem, ceilingSize, ceilingIndex, &bestSize, &bestIndex))
        {
            break;
        }
        ceilingSize = bestSize;
        ceilingIndex = bestIndex;

        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine:   ");
        appendByteSize(message, sizeof(message), bestSize);
        stringAppend(message, sizeof(message), "  ");
        stringAppend(message, sizeof(message),
                     virtualFileSystemGetEntry(fileSystem, bestIndex)->path);
        platformLogMessage(message);
    }
}


/* A disc load, one step at a time.
 *
 * Stepped rather than run to completion because a browser cannot answer a read
 * on the spot: it has to go back to its event loop, fetch the bytes and come
 * back. Reads there answer PENDING, and PENDING means "call me again", which
 * only works if there is something to call.
 *
 * The state lives here rather than on a caller's stack for the same reason —
 * the caller is a frame that has already returned. */
static VirtualFileSystem *discFileSystem = NULL_POINTER;
static DiscReader discReader;
static DiscContentSearch discSearch;
static EngineDiscLoadStatus discLoadStatus = ENGINE_DISC_IDLE;
static Boolean discCatalogueIsBuilt = BOOLEAN_FALSE;

/* How many textures the disc-wide index can remember. A retail disc holds
   thousands; this is generous enough for one and says so when it is not. */
#define TEXTURE_INDEX_CAPACITY 32768U

/* Which of the several things a load can be doing. The texture search only
   happens when a material named an image its own package did not hold, which
   is the usual case for a Sim and never the case for a teapot. */
typedef enum DiscPhase
{
    DISC_PHASE_CONTENT = 0,
    DISC_PHASE_PROBE,
    DISC_PHASE_INSTALLER,
    DISC_PHASE_INDEX,
    DISC_PHASE_FETCH_TEXTURE,
    DISC_PHASE_FETCH_LEVEL,
    DISC_PHASE_DONE
} DiscPhase;

static DiscPhase discPhase = DISC_PHASE_CONTENT;
static ResourceIndex textureIndex;

/* Where the arena stood before the texture was read, kept because the texture
   has to survive between steps.
 *
 * Following a reference means a second read, and a second read means a second
 * pend. Rewinding and starting again on a pend is right for one read and wrong
 * for two: the retry re-reads the first, which pends, and the two take turns
 * for ever. That is not hypothetical — it printed seventeen thousand identical
 * log lines before anyone looked. */
static MemorySize textureFetchMarker = 0UL;

/* Where the probe has got to. The ceiling is the last file it looked at, in the
   (size, position) order the listing uses, so the two walk the same files in the
   same order and a probe line can be read against a listing line. */
static Unsigned64 probeCeilingSize = 0xFFFFFFFFFFFFFFFFULL;
static Unsigned32 probeCeilingIndex = 0U;
static Unsigned32 probesDone = 0U;

/* How many files get identified. The largest are probed first, so this bounds
   the log rather than the search: the file that matters on a disc whose game is
   sealed inside one archive is, by definition, the biggest one there. */
#define PROBE_LIMIT 12U

/* The largest file the probe found worth opening, and how far into opening it
   the load has got. */
#define NO_INSTALLER 0xFFFFFFFFUL
static Unsigned32 installerFileIndex = (Unsigned32)NO_INSTALLER;
static Unsigned32 installerStage = 0U;
static InstallerOffsetTable installerTable;

/* How much of an installer gets searched when its table is not where the older
   loaders put it, and how much is read at a time.
 *
 * A newer loader keeps the table in one of the program's resources, which is
 * inside the program, which is at the front of the file. Thirty-two mebibytes
 * of a two point seven gibibyte installer is generous for something that lives
 * in the first one — and a limit that is reported when it is reached, rather
 * than a search that quietly stops. */
#define INSTALLER_SCAN_LIMIT_BYTES (32ULL * 1024ULL * 1024ULL)
#define INSTALLER_SCAN_CHUNK_BYTES (256UL * 1024UL)

/* How far into an archive the walk goes. Every entry costs a read, so this is
   real time on a browser's event loop — but the alternative is not seeing most
   of the game. Bounded so a misread length cannot walk for ever. */
#define ARCHIVE_WALK_LIMIT 20000U
/* And how many get named. The rest are counted. */
#define ARCHIVE_NAME_LIMIT_IN_LOG 8U

static Unsigned64 archiveBlockOffset = 0ULL;
static Unsigned32 archiveEntriesWalked = 0U;
static Unsigned32 archiveStoredCount = 0U;
static Unsigned32 archivePackedCount = 0U;
static Unsigned64 archiveStoredBytes = 0ULL;
static Unsigned32 archiveMountedCount = 0U;
static Unsigned32 archiveUnmountableCount = 0U;
/* The first package mounted, kept so the arithmetic that placed it can be
   checked against the bytes it points at. */
static Unsigned32 archiveFirstMountedIndex = 0xFFFFFFFFUL;

static Unsigned64 installerScanOffset = 0ULL;
static Unsigned64 installerScanFrom = 0ULL;
static Unsigned64 installerVersionOffset = (Unsigned64)INSTALLER_MARKER_NOT_FOUND;

/* Marks worth looking for in whatever an installer carries.
 *
 * Not a list of things this engine can read. A cabinet found here would still
 * have to be decoded, and DBPF found here would mean the packages were stored
 * whole — which would make everything downstream of this unnecessary. Either
 * answer is worth a pass over the bytes, and neither is worth guessing at. */
typedef struct SearchedMark
{
    const char *bytes;
    MemorySize length;
    const char *name;
} SearchedMark;

static const SearchedMark searchedMarks[] = {
    { "DBPF", 4UL, "a package stored whole" },
    { "MSCF", 4UL, "a Microsoft cabinet" },
    { "ISc(", 4UL, "an InstallShield cabinet" },
    { "PK\x03\x04", 4UL, "a zip archive" },
    { "Rar!", 4UL, "a RAR archive" },
    { "7z\xBC\xAF", 4UL, "a 7-zip archive" },
    { "ArC\x01", 4UL, "a FreeArc archive" }
};

static Unsigned64 searchedMarkOffsets[VICTORIA_ARRAY_LENGTH(searchedMarks)];

/* The first bytes of a file, and what they mean.
 *
 * This disc's entire game is inside a single two point seven gigabyte file that
 * is not a package, and the extension says only that somebody meant it to be
 * run. What it actually is decides whether the data inside it can be reached at
 * all, and the first four bytes are usually the whole answer.
 *
 * Not a decoder and not the beginning of one — a name for what was found, so
 * the next decision is made against evidence rather than against the file
 * extension. */
typedef struct FileSignature
{
    /* Whether a file carrying this mark is worth opening rather than merely
       naming. A cabinet is a container too, but nothing here can read one; an
       installer is a container this engine has a reader for. */
    Boolean worthFollowing;
    /* Where in the file the mark sits. Nearly always the very front, but a
       program that carries an archive puts its own mark past the header it had
       to start with — and that mark is the informative one, because "a Windows
       program" is what every installer on every disc looks like. */
    MemorySize offset;
    const char *bytes;
    MemorySize length;
    const char *name;
} FileSignature;

static const FileSignature fileSignatures[] = {
    { BOOLEAN_FALSE, 0UL, "DBPF", 4UL, "a package by content, whatever it is called" },
    { BOOLEAN_FALSE, 0UL, "MSCF", 4UL, "a Microsoft cabinet" },
    { BOOLEAN_FALSE, 0UL, "ISc(", 4UL, "an InstallShield cabinet" },
    { BOOLEAN_FALSE, 0UL, "Rar!", 4UL, "a RAR archive" },
    { BOOLEAN_FALSE, 0UL, "PK\x03\x04", 4UL, "a zip archive" },
    { BOOLEAN_FALSE, 0UL, "PK\x05\x06", 4UL, "an empty zip archive" },
    { BOOLEAN_FALSE, 0UL, "7z\xBC\xAF", 4UL, "a 7-zip archive" },
    { BOOLEAN_FALSE, 0UL, "\x1F\x8B", 2UL, "gzip" },
    { BOOLEAN_FALSE, 0UL, "BSDIFF", 6UL, "a binary patch" },
    /* Before the plain program marks, because this is a program and the fact
       that it is an installer carrying a payload is the part worth knowing. */
    { BOOLEAN_TRUE, INSTALLER_LOADER_HEADER_OFFSET, "rDlPtS", 6UL,
      "an Inno Setup installer" },
    /* Last, because a self-extracting archive of any of the above is also a
       Windows program, and the specific answer is the useful one. Delphi's
       linker writes MZP where Microsoft's writes MZ, which is worth separating:
       every installer builder worth the name is a Delphi program. Followed as
       well, because the mark at 0x30 is not the only place a loader may keep
       its table, and being told it is not one is worth a read. */
    { BOOLEAN_TRUE, 0UL, "MZP", 3UL, "a Delphi-built program, which on a file this size means an installer" },
    { BOOLEAN_FALSE, 0UL, "MZ", 2UL, "a Windows program — anything inside is appended, not at the front" }
};


static const FileSignature *identifySignature(const Unsigned8 *bytes, MemorySize byteCount)
{
    MemorySize which;

    for (which = 0UL; which < VICTORIA_ARRAY_LENGTH(fileSignatures); which++)
    {
        const FileSignature *signature = &fileSignatures[which];
        MemorySize index;
        Boolean matches = BOOLEAN_TRUE;

        if (byteCount < signature->offset + signature->length)
        {
            continue;
        }
        for (index = 0UL; index < signature->length; index++)
        {
            if (bytes[signature->offset + index] != (Unsigned8)signature->bytes[index])
            {
                matches = BOOLEAN_FALSE;
                break;
            }
        }
        if (matches)
        {
            return signature;
        }
    }
    return NULL_POINTER;
}

/* Presents one stored archive entry to the catalogue as a file of its own.
 *
 * A stored entry is a range of the containing file and nothing more, and the
 * catalogue is a list of ranges — so the engine can be handed the packages
 * inside an archive without learning that there is an archive. Everything
 * downstream, the disc-wide index and the scenegraph chain included, then works
 * on them unchanged.
 *
 * Only packages are mounted. The archive holds the whole installed game, most
 * of which is programs, configuration and text this engine has no reader for,
 * and a catalogue full of them is a catalogue with no room for the ones that
 * matter. */
static void mountArchiveEntry(const VirtualFileEntry *containingFile, const ArchiveEntry *archiveEntry)
{
    MemorySize nameLength;
    char *storedName;

    if (archiveEntry->isDirectory || archiveEntry->unpackedSizeInBytes == 0ULL ||
        !stringEndsWithIgnoringCase(archiveEntry->name, ".package"))
    {
        return;
    }

    /* The catalogue keeps the pointer, not the characters, so the name has to
       outlive this call. */
    nameLength = stringLength(archiveEntry->name);
    storedName = (char *)memoryArenaAllocate(globalArena, nameLength + 1UL, 1UL);
    if (storedName == NULL_POINTER)
    {
        archiveUnmountableCount++;
        return;
    }
    storedName[0] = '\0';
    stringAppend(storedName, nameLength + 1UL, archiveEntry->name);

    /* The archive's offsets are inside the file that holds it; the catalogue's
       are inside the store. One addition apart, and getting it wrong would
       point every mounted package at the wrong place by exactly the size of the
       program in front of them. */
    if (!virtualFileSystemAddEntry(discFileSystem, storedName,
                                   containingFile->offsetInBytes + archiveEntry->dataOffsetInBytes,
                                   archiveEntry->unpackedSizeInBytes))
    {
        archiveUnmountableCount++;
        return;
    }
    if (archiveFirstMountedIndex == 0xFFFFFFFFUL)
    {
        archiveFirstMountedIndex = discFileSystem->entryCount - 1U;
    }
    archiveMountedCount++;
}

static void reportDiscFailure(const char *what)
{
    char message[192];

    message[0] = '\0';
    stringAppend(message, sizeof(message), "engine: ");
    stringAppend(message, sizeof(message), what);
    platformLogMessage(message);
    discLoadStatus = ENGINE_DISC_FAILED;
}

void engineBeginDiscLoad(VirtualFileSystem *fileSystem)
{
    discFileSystem = fileSystem;
    discLoadStatus = ENGINE_DISC_IDLE;
    discCatalogueIsBuilt = BOOLEAN_FALSE;
    discPhase = DISC_PHASE_CONTENT;

    if (fileSystem == NULL_POINTER)
    {
        return;
    }

    /* A catalogue that is already filled came from a host that knows what it
       has — a chosen folder, where there is no image to walk. */
    if (fileSystem->entryCount > 0U)
    {
        discCatalogueIsBuilt = BOOLEAN_TRUE;
        engineReportDiscCatalogue(fileSystem);
        probeCeilingSize = 0xFFFFFFFFFFFFFFFFULL;
        probeCeilingIndex = 0U;
        probesDone = 0U;
        discPhase = DISC_PHASE_PROBE;
        discContentBegin(&discSearch, fileSystem, globalArena);
    }
    else if (discReaderBegin(&discReader, fileSystem, globalArena, DISC_FILE_LIMIT) !=
             DISC_READ_PENDING)
    {
        reportDiscFailure("not enough room to catalogue this disc");
        return;
    }
    discLoadStatus = ENGINE_DISC_WORKING;
}

/* Decodes whatever texture the search settled on and hands it to the backend.
 *
 * The pixels are staged in the arena and given straight back. Every backend
 * copies during the call — the driver owns the image afterwards — so holding
 * them would be two of everything, and a 256 by 256 image is a quarter of a
 * megabyte decoded. */
static void uploadFoundTexture(void)
{
    char message[192];
    MemorySize marker;
    MemorySize wantedBytes;
    Unsigned8 *decoded;
    TextureDecodeResult decodeResult;

    if (!discSearch.textureFound)
    {
        return;
    }
    if (discSearch.mesh.textureCoordinates == NULL_POINTER)
    {
        /* Sampling an image with no coordinates to sample it at would paint
           every vertex with the same pixel, which reads as a broken decoder
           rather than as a mesh without texture coordinates. */
        platformLogMessage("engine: the mesh has no texture coordinates, so it is left unpainted");
        return;
    }

    marker = memoryArenaGetMarker(globalArena);
    wantedBytes = textureDecodeGetRequiredBytes(discSearch.texture.levelWidth,
                                                discSearch.texture.levelHeight);
    decoded = (Unsigned8 *)memoryArenaAllocate(globalArena, wantedBytes, 4UL);
    decodeResult = TEXTURE_DECODE_DESTINATION_TOO_SMALL;
    if (decoded != NULL_POINTER)
    {
        decodeResult = textureDecodeLevel(decoded, wantedBytes, discSearch.texture.bytes,
                                          discSearch.texture.byteCount, discSearch.texture.format,
                                          discSearch.texture.levelWidth,
                                          discSearch.texture.levelHeight);
    }
    if (decodeResult == TEXTURE_DECODE_OK)
    {
        renderSetTexture(decoded, (Unsigned32)discSearch.texture.levelWidth,
                         (Unsigned32)discSearch.texture.levelHeight);
    }
    else
    {
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: the texture would not decode — ");
        stringAppend(message, sizeof(message), textureDecodeResultGetName(decodeResult));
        platformLogMessage(message);
    }
    memoryArenaRewindToMarker(globalArena, marker);
}

/* Reads one indexed resource into the arena, unpacked. Answers false while the
 * bytes are still on their way; a null result with a true answer means the read
 * finished and failed. */
static Boolean readIndexedResource(const ResourceIndexEntry *found, Unsigned8 **resultBytes,
                                   MemorySize *resultSize)
{
    Unsigned8 *bytes;
    VirtualReadResult read;
    MemorySize size = (MemorySize)found->sizeInBytes;

    *resultBytes = NULL_POINTER;
    *resultSize = 0UL;

    bytes = (Unsigned8 *)memoryArenaAllocate(globalArena, size, 8UL);
    if (bytes == NULL_POINTER)
    {
        return BOOLEAN_TRUE;
    }
    read = virtualFileSystemReadFile(discFileSystem, found->fileIndex,
                                     (Unsigned64)found->offsetInBytes, size, bytes);
    if (read == VIRTUAL_READ_PENDING)
    {
        /* Given back and asked for again next step. The arena is a stack, so
           leaving it allocated across a pend would strand it. */
        return BOOLEAN_FALSE;
    }
    if (read != VIRTUAL_READ_OK)
    {
        return BOOLEAN_TRUE;
    }

    /* Compressed exactly as it would be inside its own package, so the same
       unpacking applies — the resource does not know which file it is in. */
    if (compressionLooksLikeRefPack(bytes, size))
    {
        MemorySize unpackedSize = compressionGetDecompressedSize(bytes, size);
        Unsigned8 *unpacked = (Unsigned8 *)memoryArenaAllocate(globalArena, unpackedSize, 8UL);

        if (unpacked == NULL_POINTER ||
            compressionDecompressRefPack(unpacked, unpackedSize, bytes, size, &unpackedSize) !=
                COMPRESSION_OK)
        {
            return BOOLEAN_TRUE;
        }
        bytes = unpacked;
        size = unpackedSize;
    }

    *resultBytes = bytes;
    *resultSize = size;
    return BOOLEAN_TRUE;
}

/* Reads the texture the index pointed at, out of whichever package holds it.
 * Answers false while the bytes are still on their way. */
static Boolean fetchIndexedTexture(const ResourceIndexEntry *found, Boolean *succeeded)
{
    Unsigned8 *bytes;
    MemorySize size;

    *succeeded = BOOLEAN_FALSE;
    if (!readIndexedResource(found, &bytes, &size))
    {
        return BOOLEAN_FALSE;
    }
    if (bytes == NULL_POINTER)
    {
        return BOOLEAN_TRUE;
    }
    if (textureReaderOpen(&discSearch.texture, bytes, size) == TEXTURE_READ_OK)
    {
        discSearch.textureFound = BOOLEAN_TRUE;
        *succeeded = BOOLEAN_TRUE;
    }
    return BOOLEAN_TRUE;
}

/* Follows a texture's reference to the resource holding its largest level.
 *
 * A TXTR stores its mip levels smallest first, and the largest is frequently
 * not in it at all: it is a named reference to a LIFO, a resource holding that
 * one level and nothing else. A face read without following it is a 128 by 128
 * face when the disc holds 512 by 512 — not wrong, just the wrong quarter of
 * the resolution, and silently.
 *
 * Everything needed to describe the image — its format, its full dimensions —
 * came from the TXTR and stays. Only the bytes and the level's own size are
 * replaced. Answers false while the bytes are on their way.
 *
 * The TXTR's own bytes are deliberately still allocated beneath this one, so a
 * LIFO that will not read leaves the smaller level intact to fall back on. */
static Boolean fetchLargestLevel(char *message, MemorySize messageCapacity)
{
    char wanted[RESOURCE_NAME_LIMIT];
    const ResourceIndexEntry *found;
    Unsigned8 *bytes;
    MemorySize size;
    TextureLevel largest;
    TextureReadResult opened;

    /* Both spellings. The reference is a resource name and the suffix is a
       convention, so which of the two the disc used is not knowable from here. */
    found = resourceIndexFindNamed(&textureIndex, (Unsigned32)PACKAGE_TYPE_LIFO,
                                   discSearch.texture.lifoName);
    if (found == NULL_POINTER)
    {
        materialBuildResourceName(wanted, sizeof(wanted), discSearch.texture.lifoName, "_lifo");
        found = resourceIndexFindNamed(&textureIndex, (Unsigned32)PACKAGE_TYPE_LIFO, wanted);
    }
    if (found == NULL_POINTER)
    {
        message[0] = '\0';
        stringAppend(message, messageCapacity, "engine: its largest level names ");
        stringAppend(message, messageCapacity, discSearch.texture.lifoName);
        stringAppend(message, messageCapacity, ", which is nowhere on this disc");
        platformLogMessage(message);
        return BOOLEAN_TRUE;
    }

    if (!readIndexedResource(found, &bytes, &size))
    {
        return BOOLEAN_FALSE;
    }

    message[0] = '\0';
    stringAppend(message, messageCapacity, "engine: its largest level ");
    if (bytes == NULL_POINTER)
    {
        stringAppend(message, messageCapacity, "would not read");
        platformLogMessage(message);
        return BOOLEAN_TRUE;
    }

    opened = textureReaderOpenLevel(&largest, bytes, size);
    if (opened != TEXTURE_READ_OK)
    {
        /* Said with the reason and the first bytes, because a LIFO this reader
           cannot open is a format question, and the answer to a format question
           is always in the bytes. */
        stringAppend(message, messageCapacity, "would not open — ");
        stringAppend(message, messageCapacity, textureReadResultGetName(opened));
        stringAppend(message, messageCapacity, ", starting ");
        appendHexadecimalBytes(message, messageCapacity, bytes, size, 0UL, 8UL);
        platformLogMessage(message);
        return BOOLEAN_TRUE;
    }
    if (largest.bytes == NULL_POINTER || largest.width <= discSearch.texture.levelWidth)
    {
        /* No bigger than what is already in hand. Following a reference that
           leads back to the same resolution would cost a read and change
           nothing, and reporting it as an improvement would be a lie. */
        stringAppend(message, messageCapacity, "is no larger than the one already read");
        platformLogMessage(message);
        return BOOLEAN_TRUE;
    }
    /* A level carries no format of its own — the texture that named it owns
       that — so the only check available is whether its length is what those
       dimensions cost in that format. A level that fails it would decode into
       noise, and noise on a face is harder to diagnose than a refusal. */
    if (largest.byteCount != textureFormatGetLevelBytes(discSearch.texture.format, largest.width,
                                                       largest.height))
    {
        stringAppend(message, messageCapacity, "is ");
        appendCount(message, messageCapacity, (Unsigned32)largest.byteCount);
        stringAppend(message, messageCapacity, " bytes, which is not what ");
        appendCount(message, messageCapacity, (Unsigned32)largest.width);
        stringAppend(message, messageCapacity, "x");
        appendCount(message, messageCapacity, (Unsigned32)largest.height);
        stringAppend(message, messageCapacity, " costs in ");
        stringAppend(message, messageCapacity, textureFormatGetName(discSearch.texture.format));
        platformLogMessage(message);
        return BOOLEAN_TRUE;
    }

    discSearch.texture.bytes = largest.bytes;
    discSearch.texture.byteCount = largest.byteCount;
    discSearch.texture.levelWidth = largest.width;
    discSearch.texture.levelHeight = largest.height;
    discSearch.texture.largestIsElsewhere = BOOLEAN_FALSE;

    appendCount(message, messageCapacity, (Unsigned32)largest.width);
    stringAppend(message, messageCapacity, "x");
    appendCount(message, messageCapacity, (Unsigned32)largest.height);
    stringAppend(message, messageCapacity, ", from ");
    stringAppend(message, messageCapacity, discSearch.texture.lifoName);
    platformLogMessage(message);
    return BOOLEAN_TRUE;
}

EngineDiscLoadStatus engineStepDiscLoad(void)
{
    /* Wide enough for every refusal reason at once. A truncated diagnostic is
       worse than none: it looks complete. */
    char message[512];

    if (discLoadStatus != ENGINE_DISC_WORKING)
    {
        return discLoadStatus;
    }

    if (discPhase == DISC_PHASE_INDEX)
    {
        ResourceIndexStatus indexStatus = resourceIndexStep(&textureIndex);

        if (indexStatus == RESOURCE_INDEX_WORKING)
        {
            return ENGINE_DISC_WORKING;
        }

        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: indexed ");
        appendCount(message, sizeof(message), textureIndex.filesIndexed);
        stringAppend(message, sizeof(message), " package(s), ");
        appendCount(message, sizeof(message), textureIndex.entriesSeen);
        stringAppend(message, sizeof(message), " resource(s): ");
        appendCount(message, sizeof(message), textureIndex.countByType[0]);
        stringAppend(message, sizeof(message), " image(s), ");
        appendCount(message, sizeof(message), textureIndex.countByType[1]);
        stringAppend(message, sizeof(message), " mip level(s)");
        if (textureIndex.dropped > 0U)
        {
            stringAppend(message, sizeof(message), ", ");
            appendCount(message, sizeof(message), textureIndex.dropped);
            stringAppend(message, sizeof(message), " past the index's room");
        }
        if (textureIndex.filesRefused > 0U)
        {
            stringAppend(message, sizeof(message), ", ");
            appendCount(message, sizeof(message), textureIndex.filesRefused);
            stringAppend(message, sizeof(message), " would not be read");
        }
        platformLogMessage(message);

        /* What the disc is actually made of. Looking for one type and finding
           little of it says nothing about whether the disc is unusual or the
           search is; this says which, in the same run. */
        {
            Unsigned32 rank;

            message[0] = '\0';
            stringAppend(message, sizeof(message), "engine: most common —");
            for (rank = 0U; rank < 8U; rank++)
            {
                Unsigned32 typeIdentifier;
                Unsigned32 howMany;

                if (!resourceIndexGetCensusRank(&textureIndex, rank, &typeIdentifier, &howMany))
                {
                    break;
                }
                stringAppend(message, sizeof(message), " ");
                appendHexadecimal(message, sizeof(message), typeIdentifier);
                stringAppend(message, sizeof(message), " x");
                appendCount(message, sizeof(message), howMany);
                stringAppend(message, sizeof(message), ";");
            }
            stringAppend(message, sizeof(message), " of ");
            appendCount(message, sizeof(message), textureIndex.censusCount);
            stringAppend(message, sizeof(message), " distinct type(s)");
            if (textureIndex.censusOverflow > 0U)
            {
                /* The census ran out of room. Everything above is a tally of
                   what fitted, which is not the same as a tally of the disc. */
                stringAppend(message, sizeof(message), ", ");
                appendCount(message, sizeof(message), textureIndex.censusOverflow);
                stringAppend(message, sizeof(message), " entries of untallied types");
            }
            platformLogMessage(message);
        }

        discPhase = DISC_PHASE_FETCH_TEXTURE;
        return ENGINE_DISC_WORKING;
    }

    /* Following the reference to the largest level, with the texture that named
     * it still allocated beneath. Its own marker, so a pend gives back only what
     * this step claimed and leaves the texture alone — and so a LIFO that will
     * not read falls back on the smaller level rather than on nothing. */
    if (discPhase == DISC_PHASE_FETCH_LEVEL)
    {
        MemorySize marker = memoryArenaGetMarker(globalArena);

        if (!fetchLargestLevel(message, sizeof(message)))
        {
            memoryArenaRewindToMarker(globalArena, marker);
            return ENGINE_DISC_WORKING;
        }

        renderSetMesh(&discSearch.mesh, globalArena);
        uploadFoundTexture();
        memoryArenaRewindToMarker(globalArena, textureFetchMarker);
        discPhase = DISC_PHASE_DONE;
        discLoadStatus = ENGINE_DISC_READY;
        return discLoadStatus;
    }

    if (discPhase == DISC_PHASE_FETCH_TEXTURE)
    {
        char wanted[RESOURCE_NAME_LIMIT];
        const ResourceIndexEntry *found;

        /* Both spellings, and both types. The suffix is a convention rather
           than a rule, and a texture whose only copy on the disc is the mip
           level resource is still the texture. Trying one shape and reporting
           "nowhere on this disc" would be reporting the convention's failure
           as the disc's. */
        materialBuildResourceName(wanted, sizeof(wanted), discSearch.textureName, "_txtr");
        found = resourceIndexFindNamed(&textureIndex, (Unsigned32)PACKAGE_TYPE_TXTR, wanted);
        if (found == NULL_POINTER)
        {
            found = resourceIndexFindNamed(&textureIndex, (Unsigned32)PACKAGE_TYPE_TXTR,
                                           discSearch.textureName);
        }
        if (found == NULL_POINTER)
        {
            materialBuildResourceName(wanted, sizeof(wanted), discSearch.textureName, "_lifo");
            found = resourceIndexFindNamed(&textureIndex, (Unsigned32)PACKAGE_TYPE_LIFO, wanted);
        }
        if (found == NULL_POINTER)
        {
            found = resourceIndexFindNamed(&textureIndex, (Unsigned32)PACKAGE_TYPE_LIFO,
                                           discSearch.textureName);
        }

        if (found != NULL_POINTER)
        {
            Boolean succeeded = BOOLEAN_FALSE;

            textureFetchMarker = memoryArenaGetMarker(globalArena);
            if (!fetchIndexedTexture(found, &succeeded))
            {
                memoryArenaRewindToMarker(globalArena, textureFetchMarker);
                return ENGINE_DISC_WORKING;
            }
            message[0] = '\0';
            stringAppend(message, sizeof(message), "engine: ");
            stringAppend(message, sizeof(message), wanted);
            if (succeeded)
            {
                stringAppend(message, sizeof(message), " found elsewhere on the disc, ");
                appendCount(message, sizeof(message), (Unsigned32)discSearch.texture.levelWidth);
                stringAppend(message, sizeof(message), "x");
                appendCount(message, sizeof(message), (Unsigned32)discSearch.texture.levelHeight);
                stringAppend(message, sizeof(message), " ");
                stringAppend(message, sizeof(message),
                             textureFormatGetName(discSearch.texture.format));
            }
            else
            {
                stringAppend(message, sizeof(message), " was indexed but would not read");
            }
            platformLogMessage(message);
            /* The level in the TXTR is the largest one it holds, which is not
               always the largest one there is. Handed to a phase of its own so
               the texture just read stays where it is: the reference costs
               another read, and a read that pends must not take the first one
               with it. */
            if (succeeded && discSearch.texture.largestIsElsewhere &&
                discSearch.texture.lifoName[0] != '\0')
            {
                discPhase = DISC_PHASE_FETCH_LEVEL;
                return ENGINE_DISC_WORKING;
            }
            /* The mesh before the texture. A backend binds an image to the
               pipeline the mesh created, so there is nothing to bind it to
               until the mesh has been uploaded — an ordering that held by
               accident for as long as no texture was ever found, and broke on
               the first disc that yielded one. */
            renderSetMesh(&discSearch.mesh, globalArena);
            uploadFoundTexture();
            /* Held until the upload has copied it, then given back. */
            memoryArenaRewindToMarker(globalArena, textureFetchMarker);
        }
        else
        {
            message[0] = '\0';
            stringAppend(message, sizeof(message), "engine: ");
            stringAppend(message, sizeof(message), wanted);
            stringAppend(message, sizeof(message), " is nowhere on this disc");
            platformLogMessage(message);
            renderSetMesh(&discSearch.mesh, globalArena);
        }

        discPhase = DISC_PHASE_DONE;
        discLoadStatus = ENGINE_DISC_READY;
        return discLoadStatus;
    }

    /* Says what the disc's large non-package files actually are, before any of
       the work that assumes the answer is "nothing that matters". Sixteen bytes
       apiece; the phase exists at all because on the web even sixteen bytes have
       to go back to the event loop. */
    if (discPhase == DISC_PHASE_PROBE)
    {
        const VirtualFileEntry *entry;
        Unsigned8 head[64];
        MemorySize headSize;
        VirtualReadResult read;
        Unsigned64 nextSize = 0ULL;
        Unsigned32 nextIndex = 0U;

        if (probesDone >= PROBE_LIMIT ||
            !findNextLargestOther(discFileSystem, probeCeilingSize, probeCeilingIndex, &nextSize,
                                  &nextIndex))
        {
            discPhase = (installerFileIndex == NO_INSTALLER) ? DISC_PHASE_CONTENT
                                                             : DISC_PHASE_INSTALLER;
            installerStage = 0U;
            return ENGINE_DISC_WORKING;
        }

        entry = virtualFileSystemGetEntry(discFileSystem, nextIndex);
        if (entry == NULL_POINTER)
        {
            probeCeilingSize = nextSize;
            probeCeilingIndex = nextIndex;
            return ENGINE_DISC_WORKING;
        }

        /* Whatever the file holds, capped at what it holds: a file shorter than
           the head buffer still has a signature, and asking for more than exists
           is refused outright rather than answered short. */
        headSize = (entry->sizeInBytes < (Unsigned64)sizeof(head)) ? (MemorySize)entry->sizeInBytes
                                                                  : sizeof(head);
        read = virtualFileSystemReadFile(discFileSystem, nextIndex, 0U, headSize, head);
        if (read == VIRTUAL_READ_PENDING)
        {
            return ENGINE_DISC_WORKING;
        }

        probeCeilingSize = nextSize;
        probeCeilingIndex = nextIndex;
        probesDone++;

        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine:   ");
        stringAppend(message, sizeof(message), entry->path);
        stringAppend(message, sizeof(message), " — ");
        if (read != VIRTUAL_READ_OK)
        {
            /* Said rather than skipped. A probe that goes quiet on a file it
               could not read looks exactly like a disc with nothing on it. */
            stringAppend(message, sizeof(message), "would not read: ");
            stringAppend(message, sizeof(message), virtualReadResultGetName(read));
            platformLogMessage(message);
            return ENGINE_DISC_WORKING;
        }
        {
            const FileSignature *signature = identifySignature(head, headSize);

            stringAppend(message, sizeof(message),
                         (signature != NULL_POINTER) ? signature->name : "unrecognised");
            /* The largest one worth opening, which is the first met: the walk
               is largest first, and the file holding a disc's game is not the
               second biggest thing on it. */
            if (signature != NULL_POINTER && signature->worthFollowing &&
                installerFileIndex == NO_INSTALLER)
            {
                installerFileIndex = nextIndex;
            }
        }
        stringAppend(message, sizeof(message), ", starting ");
        {
            /* The bytes as well as the verdict. A name this reader does not know
               is exactly the case where the bytes themselves are what somebody
               needs to see — and the same goes for the mark at 0x30, which is
               where a program that carries an archive says so. */
            appendHexadecimalBytes(message, sizeof(message), head, headSize, 0UL, 8UL);
            if (headSize >= INSTALLER_LOADER_HEADER_OFFSET + 8UL)
            {
                stringAppend(message, sizeof(message), "and at 0x30 ");
                appendHexadecimalBytes(message, sizeof(message), head, headSize,
                                       INSTALLER_LOADER_HEADER_OFFSET, 8UL);
            }
        }
        platformLogMessage(message);
        return ENGINE_DISC_WORKING;
    }

    /* Opening the installer the probe found.
     *
     * Three reads: the front of the file, to find where the offset table is;
     * the table itself, which ends with the two offsets everything else hangs
     * off; and the version string at the first of them, because which fields
     * the setup header holds depends on which version wrote it.
     *
     * Nothing is decompressed here. This establishes that the installer can be
     * navigated and says what would have to be decoded next — a reader that
     * announced it could open an archive before it could find its way around
     * one would be announcing nothing. */
    if (discPhase == DISC_PHASE_INSTALLER)
    {
        static Unsigned64 tableOffsetInBytes = 0ULL;
        Unsigned8 buffer[INSTALLER_TABLE_LARGEST_BYTES > INSTALLER_VERSION_STRING_BYTES
                             ? INSTALLER_TABLE_LARGEST_BYTES
                             : INSTALLER_VERSION_STRING_BYTES];
        const VirtualFileEntry *entry = virtualFileSystemGetEntry(discFileSystem, installerFileIndex);
        VirtualReadResult read;
        InstallerReadResult opened;

        if (entry == NULL_POINTER)
        {
            discPhase = DISC_PHASE_CONTENT;
            return ENGINE_DISC_WORKING;
        }

        if (installerStage == 0U)
        {
            read = virtualFileSystemReadFile(discFileSystem, installerFileIndex, 0U,
                                             INSTALLER_LOADER_HEADER_OFFSET + 12UL, buffer);
            if (read == VIRTUAL_READ_PENDING)
            {
                return ENGINE_DISC_WORKING;
            }
            opened = (read == VIRTUAL_READ_OK)
                         ? installerFindOffsetTable(buffer, INSTALLER_LOADER_HEADER_OFFSET + 12UL,
                                                    &tableOffsetInBytes)
                         : INSTALLER_READ_TRUNCATED;

            message[0] = '\0';
            stringAppend(message, sizeof(message), "engine: ");
            stringAppend(message, sizeof(message), entry->path);
            if (opened == INSTALLER_READ_NOT_AN_INSTALLER)
            {
                /* Not where the older loaders keep it. A newer one keeps it in
                   a resource inside the program, which is three formats deep
                   from here — so it gets looked for instead, because the table
                   says what it is and can be recognised on sight. */
                stringAppend(message, sizeof(message),
                             " keeps no table at 0x30, so its payload is appended");
                platformLogMessage(message);
                installerScanOffset = 0ULL;
                installerVersionOffset = (Unsigned64)INSTALLER_MARKER_NOT_FOUND;
                installerStage = 3U;
                return ENGINE_DISC_WORKING;
            }
            if (opened != INSTALLER_READ_OK)
            {
                stringAppend(message, sizeof(message), " — ");
                stringAppend(message, sizeof(message), installerReadResultGetName(opened));
                platformLogMessage(message);
                discPhase = DISC_PHASE_CONTENT;
                return ENGINE_DISC_WORKING;
            }
            stringAppend(message, sizeof(message), " keeps its offset table at ");
            appendHexadecimal(message, sizeof(message), (Unsigned32)tableOffsetInBytes);
            platformLogMessage(message);
            installerStage = 1U;
            return ENGINE_DISC_WORKING;
        }

        /* Where the program stops.
         *
         * Nothing that identifies an installer is in the first thirty-three
         * mebibytes of this one, which is not evidence that there is nothing
         * there — it is evidence that the thing is not at the front. A program
         * carrying an archive has to start with a real program, so the payload
         * is past the end of it, and the section table says where that is. */
        if (installerStage == 3U)
        {
            MemorySize marker = memoryArenaGetMarker(globalArena);
            /* However much of the front there is. Asking for more than the file
               holds is refused outright rather than answered short, and a small
               program is still a program. */
            MemorySize frontBytes = (entry->sizeInBytes < (Unsigned64)PROGRAM_LAYOUT_BYTES_NEEDED)
                                        ? (MemorySize)entry->sizeInBytes
                                        : PROGRAM_LAYOUT_BYTES_NEEDED;
            Unsigned8 *front = (Unsigned8 *)memoryArenaAllocate(globalArena, frontBytes, 4UL);
            ProgramLayout layout;
            ProgramReadResult layoutResult;

            if (front == NULL_POINTER)
            {
                platformLogMessage("engine: no room to read the program's layout");
                discPhase = DISC_PHASE_CONTENT;
                return ENGINE_DISC_WORKING;
            }
            read = virtualFileSystemReadFile(discFileSystem, installerFileIndex, 0U, frontBytes,
                                             front);
            if (read == VIRTUAL_READ_PENDING)
            {
                memoryArenaRewindToMarker(globalArena, marker);
                return ENGINE_DISC_WORKING;
            }
            layoutResult = (read == VIRTUAL_READ_OK)
                               ? programReadLayout(front, frontBytes, entry->sizeInBytes, &layout)
                               : PROGRAM_READ_TRUNCATED;
            memoryArenaRewindToMarker(globalArena, marker);

            message[0] = '\0';
            stringAppend(message, sizeof(message), "engine: ");
            if (layoutResult != PROGRAM_READ_OK)
            {
                stringAppend(message, sizeof(message), "its layout — ");
                stringAppend(message, sizeof(message), programReadResultGetName(layoutResult));
                platformLogMessage(message);
                discPhase = DISC_PHASE_CONTENT;
                return ENGINE_DISC_WORKING;
            }
            stringAppend(message, sizeof(message), "the program in it is ");
            appendCount(message, sizeof(message), layout.sectionCount);
            stringAppend(message, sizeof(message), " section(s) ending at ");
            appendHexadecimal(message, sizeof(message), (Unsigned32)layout.endOfProgramInBytes);
            stringAppend(message, sizeof(message), ", leaving ");
            appendByteSize(message, sizeof(message),
                           entry->sizeInBytes - layout.endOfProgramInBytes);
            stringAppend(message, sizeof(message), " appended past it");
            platformLogMessage(message);

            installerScanFrom = layout.endOfProgramInBytes;
            installerScanOffset = layout.endOfProgramInBytes;
            installerStage = 4U;
            return ENGINE_DISC_WORKING;
        }

        /* What the appended part starts with, named the same way any other file
           is. The front of a container is where it says what it is. */
        if (installerStage == 4U)
        {
            Unsigned8 appendedHead[64];
            Unsigned32 which;

            read = virtualFileSystemReadFile(discFileSystem, installerFileIndex, installerScanFrom,
                                             sizeof(appendedHead), appendedHead);
            if (read == VIRTUAL_READ_PENDING)
            {
                return ENGINE_DISC_WORKING;
            }
            message[0] = '\0';
            stringAppend(message, sizeof(message), "engine: what is appended starts ");
            if (read == VIRTUAL_READ_OK)
            {
                const FileSignature *signature =
                    identifySignature(appendedHead, sizeof(appendedHead));

                appendHexadecimalBytes(message, sizeof(message), appendedHead,
                                       sizeof(appendedHead), 0UL, 16UL);
                stringAppend(message, sizeof(message), "— ");
                stringAppend(message, sizeof(message),
                             (signature != NULL_POINTER) ? signature->name : "unrecognised");
            }
            else
            {
                stringAppend(message, sizeof(message), virtualReadResultGetName(read));
            }
            platformLogMessage(message);

            /* An archive says what it is in seven bytes, and if it is one this
               reader can walk then walking it beats searching it: the block
               chain says where every entry is, and a search only says where one
               mark happened to land. */
            if (read == VIRTUAL_READ_OK &&
                archiveReadMark(appendedHead, sizeof(appendedHead), installerScanFrom,
                                &archiveBlockOffset) == ARCHIVE_READ_OK)
            {
                archiveEntriesWalked = 0U;
                archiveStoredCount = 0U;
                archivePackedCount = 0U;
                archiveStoredBytes = 0ULL;
                installerStage = 6U;
                return ENGINE_DISC_WORKING;
            }

            for (which = 0U; which < (Unsigned32)VICTORIA_ARRAY_LENGTH(searchedMarks); which++)
            {
                searchedMarkOffsets[which] = (Unsigned64)INSTALLER_MARKER_NOT_FOUND;
            }
            installerStage = 5U;
            return ENGINE_DISC_WORKING;
        }

        /* Walking the archive's blocks.
         *
         * Each block says how long it is, so this is a matter of addition — and
         * what it is really asking is whether the entries were stored or
         * packed. A stored entry is a range of the file and can be handed
         * straight to the package reader; a packed one needs an unpacker for a
         * format whose only complete implementation cannot be borrowed from
         * here. The answer decides how much of the rest of this there is. */
        if (installerStage == 6U)
        {
            MemorySize marker = memoryArenaGetMarker(globalArena);
            MemorySize wanted = ARCHIVE_BLOCK_BYTES_NEEDED;
            Unsigned8 *block;
            ArchiveEntry archiveEntry;
            ArchiveReadResult blockResult;

            if (archiveEntriesWalked >= ARCHIVE_WALK_LIMIT ||
                archiveBlockOffset >= entry->sizeInBytes)
            {
                message[0] = '\0';
                stringAppend(message, sizeof(message), "engine: walked ");
                appendCount(message, sizeof(message), archiveEntriesWalked);
                stringAppend(message, sizeof(message), " archive entries — ");
                appendCount(message, sizeof(message), archiveStoredCount);
                stringAppend(message, sizeof(message), " stored (");
                appendByteSize(message, sizeof(message), archiveStoredBytes);
                stringAppend(message, sizeof(message), "), ");
                appendCount(message, sizeof(message), archivePackedCount);
                stringAppend(message, sizeof(message), " packed, ");
                appendCount(message, sizeof(message), archiveMountedCount);
                stringAppend(message, sizeof(message), " package(s) mounted");
                if (archiveUnmountableCount > 0U)
                {
                    /* Said rather than swallowed: a catalogue that filled up
                       looks exactly like an archive that held less. */
                    stringAppend(message, sizeof(message), ", ");
                    appendCount(message, sizeof(message), archiveUnmountableCount);
                    stringAppend(message, sizeof(message), " would not fit");
                }
                platformLogMessage(message);
                installerStage = 7U;
                return ENGINE_DISC_WORKING;
            }

            if (archiveBlockOffset + (Unsigned64)wanted > entry->sizeInBytes)
            {
                wanted = (MemorySize)(entry->sizeInBytes - archiveBlockOffset);
            }
            block = (Unsigned8 *)memoryArenaAllocate(globalArena, wanted, 4UL);
            if (block == NULL_POINTER)
            {
                platformLogMessage("engine: no room to walk the archive");
                discPhase = DISC_PHASE_CONTENT;
                return ENGINE_DISC_WORKING;
            }
            read = virtualFileSystemReadFile(discFileSystem, installerFileIndex, archiveBlockOffset,
                                             wanted, block);
            if (read == VIRTUAL_READ_PENDING)
            {
                memoryArenaRewindToMarker(globalArena, marker);
                return ENGINE_DISC_WORKING;
            }
            if (read != VIRTUAL_READ_OK)
            {
                memoryArenaRewindToMarker(globalArena, marker);
                message[0] = '\0';
                stringAppend(message, sizeof(message), "engine: the walk stopped at ");
                appendHexadecimal(message, sizeof(message), (Unsigned32)archiveBlockOffset);
                stringAppend(message, sizeof(message), " — ");
                stringAppend(message, sizeof(message), virtualReadResultGetName(read));
                platformLogMessage(message);
                discPhase = DISC_PHASE_CONTENT;
                return ENGINE_DISC_WORKING;
            }

            blockResult = archiveReadBlock(block, wanted, archiveBlockOffset, &archiveEntry);
            memoryArenaRewindToMarker(globalArena, marker);

            if (blockResult == ARCHIVE_READ_OK)
            {
                archiveEntriesWalked++;
                if (archiveEntry.method == (Unsigned8)ARCHIVE_METHOD_STORED)
                {
                    archiveStoredCount++;
                    archiveStoredBytes += archiveEntry.unpackedSizeInBytes;
                    mountArchiveEntry(entry, &archiveEntry);
                }
                else
                {
                    archivePackedCount++;
                }
                if (archiveEntriesWalked <= ARCHIVE_NAME_LIMIT_IN_LOG)
                {
                    message[0] = '\0';
                    stringAppend(message, sizeof(message), "engine:   ");
                    stringAppend(message, sizeof(message),
                                 (archiveEntry.method == (Unsigned8)ARCHIVE_METHOD_STORED)
                                     ? "stored "
                                     : "packed ");
                    appendByteSize(message, sizeof(message), archiveEntry.unpackedSizeInBytes);
                    stringAppend(message, sizeof(message), " at ");
                    appendHexadecimal(message, sizeof(message),
                                      (Unsigned32)archiveEntry.dataOffsetInBytes);
                    stringAppend(message, sizeof(message), "  ");
                    stringAppend(message, sizeof(message), archiveEntry.name);
                    platformLogMessage(message);
                }
            }
            else if (blockResult != ARCHIVE_READ_NOT_A_FILE)
            {
                message[0] = '\0';
                stringAppend(message, sizeof(message), "engine: the archive stopped making sense at ");
                appendHexadecimal(message, sizeof(message), (Unsigned32)archiveBlockOffset);
                stringAppend(message, sizeof(message), " — ");
                stringAppend(message, sizeof(message), archiveReadResultGetName(blockResult));
                platformLogMessage(message);
                discPhase = DISC_PHASE_CONTENT;
                return ENGINE_DISC_WORKING;
            }

            /* A block that did not advance would be walked for ever. */
            if (archiveEntry.nextBlockOffsetInBytes <= archiveBlockOffset)
            {
                platformLogMessage("engine: an archive block that does not advance, stopping");
                discPhase = DISC_PHASE_CONTENT;
                return ENGINE_DISC_WORKING;
            }
            archiveBlockOffset = archiveEntry.nextBlockOffsetInBytes;
            return ENGINE_DISC_WORKING;
        }

        /* Reading the first package that was mounted, to see whether it is one.
         *
         * The archive counts from the start of the file that holds it and the
         * catalogue counts from the start of the store, so mounting is one
         * addition — and getting it wrong would point every mounted package at
         * the wrong place by exactly the size of the program in front of them,
         * which is the kind of mistake that produces six hundred unreadable
         * files and no explanation. Four bytes settle it. */
        if (installerStage == 7U)
        {
            Unsigned8 head[4];

            if (archiveFirstMountedIndex == 0xFFFFFFFFUL)
            {
                discPhase = DISC_PHASE_CONTENT;
                return ENGINE_DISC_WORKING;
            }
            read = virtualFileSystemReadFile(discFileSystem, archiveFirstMountedIndex, 0U,
                                             sizeof(head), head);
            if (read == VIRTUAL_READ_PENDING)
            {
                return ENGINE_DISC_WORKING;
            }

            message[0] = '\0';
            stringAppend(message, sizeof(message), "engine: the first mounted package ");
            if (read != VIRTUAL_READ_OK)
            {
                stringAppend(message, sizeof(message), "would not read: ");
                stringAppend(message, sizeof(message), virtualReadResultGetName(read));
            }
            else if (head[0] == (Unsigned8)'D' && head[1] == (Unsigned8)'B' &&
                     head[2] == (Unsigned8)'P' && head[3] == (Unsigned8)'F')
            {
                stringAppend(message, sizeof(message), "really is one");
            }
            else
            {
                stringAppend(message, sizeof(message), "starts ");
                appendHexadecimalBytes(message, sizeof(message), head, sizeof(head), 0UL, 4UL);
                stringAppend(message, sizeof(message), "rather than DBPF, so the offsets are wrong");
            }
            platformLogMessage(message);
            discPhase = DISC_PHASE_CONTENT;
            return ENGINE_DISC_WORKING;
        }

        /* Reading the appended part a chunk at a time, looking for any mark
           worth knowing about. Chunks overlap, so a mark lying across a boundary
           is still met whole — a search that reads adjacent blocks and finds
           nothing at the seam reports "not there" about something that is. */
        if (installerStage == 5U)
        {
            MemorySize marker = memoryArenaGetMarker(globalArena);
            Unsigned8 *chunk;
            MemorySize wanted = INSTALLER_SCAN_CHUNK_BYTES;
            Unsigned64 foundTable;
            Unsigned32 which;

            if (installerScanOffset >= entry->sizeInBytes ||
                installerScanOffset - installerScanFrom >= (Unsigned64)INSTALLER_SCAN_LIMIT_BYTES)
            {
                Unsigned32 found = 0U;

                for (which = 0U; which < (Unsigned32)VICTORIA_ARRAY_LENGTH(searchedMarks); which++)
                {
                    if (searchedMarkOffsets[which] != (Unsigned64)INSTALLER_MARKER_NOT_FOUND)
                    {
                        found++;
                    }
                }
                message[0] = '\0';
                stringAppend(message, sizeof(message), "engine: searched the first ");
                appendByteSize(message, sizeof(message), installerScanOffset - installerScanFrom);
                stringAppend(message, sizeof(message), " past the program, ");
                appendCount(message, sizeof(message), found);
                stringAppend(message, sizeof(message), " mark(s) found, no offset table");
                platformLogMessage(message);

                if (installerVersionOffset != (Unsigned64)INSTALLER_MARKER_NOT_FOUND)
                {
                    installerTable.headerOffsetInBytes = (Unsigned32)installerVersionOffset;
                    installerStage = 2U;
                    return ENGINE_DISC_WORKING;
                }
                discPhase = DISC_PHASE_CONTENT;
                return ENGINE_DISC_WORKING;
            }

            if (installerScanOffset + (Unsigned64)wanted > entry->sizeInBytes)
            {
                wanted = (MemorySize)(entry->sizeInBytes - installerScanOffset);
            }
            chunk = (Unsigned8 *)memoryArenaAllocate(globalArena, wanted, 4UL);
            if (chunk == NULL_POINTER)
            {
                platformLogMessage("engine: no room to search the installer");
                discPhase = DISC_PHASE_CONTENT;
                return ENGINE_DISC_WORKING;
            }
            read = virtualFileSystemReadFile(discFileSystem, installerFileIndex,
                                             installerScanOffset, wanted, chunk);
            if (read == VIRTUAL_READ_PENDING)
            {
                memoryArenaRewindToMarker(globalArena, marker);
                return ENGINE_DISC_WORKING;
            }
            if (read != VIRTUAL_READ_OK)
            {
                memoryArenaRewindToMarker(globalArena, marker);
                message[0] = '\0';
                stringAppend(message, sizeof(message), "engine: the search stopped at ");
                appendHexadecimal(message, sizeof(message), (Unsigned32)installerScanOffset);
                stringAppend(message, sizeof(message), " — ");
                stringAppend(message, sizeof(message), virtualReadResultGetName(read));
                platformLogMessage(message);
                discPhase = DISC_PHASE_CONTENT;
                return ENGINE_DISC_WORKING;
            }

            foundTable = installerFindTableMarker(chunk, wanted, installerScanOffset);
            if (installerVersionOffset == (Unsigned64)INSTALLER_MARKER_NOT_FOUND)
            {
                installerVersionOffset = installerFindVersionMarker(chunk, wanted, installerScanOffset);
            }
            for (which = 0U; which < (Unsigned32)VICTORIA_ARRAY_LENGTH(searchedMarks); which++)
            {
                Unsigned64 at;

                if (searchedMarkOffsets[which] != (Unsigned64)INSTALLER_MARKER_NOT_FOUND)
                {
                    continue;
                }
                at = installerFindMark(chunk, wanted, installerScanOffset,
                                       searchedMarks[which].bytes, searchedMarks[which].length);
                if (at == (Unsigned64)INSTALLER_MARKER_NOT_FOUND)
                {
                    continue;
                }
                searchedMarkOffsets[which] = at;
                /* Said as it is met rather than gathered for the end. The search
                   stops the moment it finds an offset table, and a summary
                   printed after that would never be printed at all. */
                message[0] = '\0';
                stringAppend(message, sizeof(message), "engine:   ");
                stringAppend(message, sizeof(message), searchedMarks[which].name);
                stringAppend(message, sizeof(message), " at ");
                appendHexadecimal(message, sizeof(message), (Unsigned32)at);
                platformLogMessage(message);
            }
            memoryArenaRewindToMarker(globalArena, marker);

            if (foundTable != (Unsigned64)INSTALLER_MARKER_NOT_FOUND)
            {
                tableOffsetInBytes = foundTable;
                message[0] = '\0';
                stringAppend(message, sizeof(message), "engine: found an offset table at ");
                appendHexadecimal(message, sizeof(message), (Unsigned32)foundTable);
                platformLogMessage(message);
                installerStage = 1U;
                return ENGINE_DISC_WORKING;
            }

            /* Back by the overlap, so nothing is missed at the seam. */
            installerScanOffset += (Unsigned64)wanted;
            if (wanted > INSTALLER_MARKER_OVERLAP_BYTES)
            {
                installerScanOffset -= (Unsigned64)INSTALLER_MARKER_OVERLAP_BYTES;
            }
            return ENGINE_DISC_WORKING;
        }

        if (installerStage == 1U)
        {
            read = virtualFileSystemReadFile(discFileSystem, installerFileIndex, tableOffsetInBytes,
                                             INSTALLER_TABLE_LARGEST_BYTES, buffer);
            if (read == VIRTUAL_READ_PENDING)
            {
                return ENGINE_DISC_WORKING;
            }
            opened = (read == VIRTUAL_READ_OK)
                         ? installerReadOffsetTable(buffer, INSTALLER_TABLE_LARGEST_BYTES,
                                                    tableOffsetInBytes, &installerTable)
                         : INSTALLER_READ_TRUNCATED;

            message[0] = '\0';
            stringAppend(message, sizeof(message), "engine: ");
            if (opened != INSTALLER_READ_OK)
            {
                stringAppend(message, sizeof(message), "its offset table — ");
                stringAppend(message, sizeof(message), installerReadResultGetName(opened));
                stringAppend(message, sizeof(message), ", starting ");
                appendHexadecimalBytes(message, sizeof(message), buffer,
                                       INSTALLER_TABLE_LARGEST_BYTES, 0UL, 12UL);
                platformLogMessage(message);
                discPhase = DISC_PHASE_CONTENT;
                return ENGINE_DISC_WORKING;
            }
            stringAppend(message, sizeof(message), "table revision ");
            appendCount(message, sizeof(message), installerTable.tableRevision);
            stringAppend(message, sizeof(message), ", ");
            appendCount(message, sizeof(message), installerTable.wordCount);
            stringAppend(message, sizeof(message), " fields: accounts for ");
            appendByteSize(message, sizeof(message), (Unsigned64)installerTable.totalSizeInBytes);
            stringAppend(message, sizeof(message), " of ");
            appendByteSize(message, sizeof(message), entry->sizeInBytes);
            stringAppend(message, sizeof(message), ", header at ");
            appendHexadecimal(message, sizeof(message), installerTable.headerOffsetInBytes);
            stringAppend(message, sizeof(message), ", data at ");
            appendHexadecimal(message, sizeof(message), installerTable.dataOffsetInBytes);
            platformLogMessage(message);
            installerStage = 2U;
            return ENGINE_DISC_WORKING;
        }

        read = virtualFileSystemReadFile(discFileSystem, installerFileIndex,
                                         (Unsigned64)installerTable.headerOffsetInBytes,
                                         INSTALLER_VERSION_STRING_BYTES, buffer);
        if (read == VIRTUAL_READ_PENDING)
        {
            return ENGINE_DISC_WORKING;
        }

        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: built with ");
        if (read == VIRTUAL_READ_OK)
        {
            char version[INSTALLER_VERSION_STRING_BYTES + 1UL];

            opened = installerReadVersionString(buffer, INSTALLER_VERSION_STRING_BYTES, version,
                                                sizeof(version));
            if (opened == INSTALLER_READ_OK)
            {
                stringAppend(message, sizeof(message), version);
            }
            else
            {
                stringAppend(message, sizeof(message), installerReadResultGetName(opened));
                stringAppend(message, sizeof(message), ", starting ");
                appendHexadecimalBytes(message, sizeof(message), buffer,
                                       INSTALLER_VERSION_STRING_BYTES, 0UL, 12UL);
            }
        }
        else
        {
            stringAppend(message, sizeof(message), virtualReadResultGetName(read));
        }
        platformLogMessage(message);
        discPhase = DISC_PHASE_CONTENT;
        return ENGINE_DISC_WORKING;
    }

    if (!discCatalogueIsBuilt)
    {
        DiscReadStatus walk = discReaderStep(&discReader);

        if (walk == DISC_READ_PENDING)
        {
            return ENGINE_DISC_WORKING;
        }
        if (walk != DISC_READ_COMPLETE)
        {
            reportDiscFailure(discReadStatusGetName(walk));
            return discLoadStatus;
        }

        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: disc ");
        stringAppend(message, sizeof(message), discReader.volumeIdentifier);
        stringAppend(message, sizeof(message), " holds ");
        appendCount(message, sizeof(message), discFileSystem->entryCount);
        stringAppend(message, sizeof(message), " files");
        platformLogMessage(message);
        engineReportDiscCatalogue(discFileSystem);

        discCatalogueIsBuilt = BOOLEAN_TRUE;
        probeCeilingSize = 0xFFFFFFFFFFFFFFFFULL;
        probeCeilingIndex = 0U;
        probesDone = 0U;
        discPhase = DISC_PHASE_PROBE;
        discContentBegin(&discSearch, discFileSystem, globalArena);
        return ENGINE_DISC_WORKING;
    }

    {
        DiscContentStatus status = discContentStep(&discSearch);

        if (status == DISC_CONTENT_PENDING)
        {
            return ENGINE_DISC_WORKING;
        }

        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: opened ");
        appendCount(message, sizeof(message), discSearch.packagesOpened);
        stringAppend(message, sizeof(message), " packages, ");
        appendCount(message, sizeof(message), discSearch.packagesCompressed);
        stringAppend(message, sizeof(message), " compressed, ");
        appendCount(message, sizeof(message), discSearch.packagesWithGeometry);
        stringAppend(message, sizeof(message), " with geometry, ");
        appendCount(message, sizeof(message), discSearch.packagesWithShapes);
        stringAppend(message, sizeof(message), " with a shape, ");
        appendCount(message, sizeof(message), discSearch.packagesWithTrees);
        stringAppend(message, sizeof(message), " with a tree, ");
        appendCount(message, sizeof(message), discSearch.modelsResolved);
        stringAppend(message, sizeof(message), " followed to a model");
        platformLogMessage(message);

        /* Why the ones that were refused were refused. A count on its own says
           something is wrong; this says what, and whether it is one thing or
           several. */
        if (discSearch.geometryRefused > 0U)
        {
            Unsigned32 reason;

            message[0] = '\0';
            stringAppend(message, sizeof(message), "engine: refused ");
            appendCount(message, sizeof(message), discSearch.geometryRefused);
            stringAppend(message, sizeof(message), " meshes —");
            if (discSearch.decompressionRefused > 0U)
            {
                stringAppend(message, sizeof(message), " would not decompress x");
                appendCount(message, sizeof(message), discSearch.decompressionRefused);
                stringAppend(message, sizeof(message), ";");
            }
            for (reason = 0U; reason < GEOMETRY_READ_RESULT_COUNT; reason++)
            {
                if (discSearch.refusalsByReason[reason] == 0U)
                {
                    continue;
                }
                stringAppend(message, sizeof(message), " ");
                stringAppend(message, sizeof(message),
                             geometryReadResultGetName((GeometryReadResult)reason));
                stringAppend(message, sizeof(message), " x");
                appendCount(message, sizeof(message), discSearch.refusalsByReason[reason]);
                stringAppend(message, sizeof(message), ";");
            }
            platformLogMessage(message);
        }

        /* What the disc holds, as opposed to what was done with it. A reason
           only says where this engine stopped; the versions say which layouts
           the game actually shipped, and that is the part guessing cannot
           supply. It doubles as proof the page is not a cached older build. */
        {
            Unsigned32 bucket;
            Boolean anyVersion = BOOLEAN_FALSE;

            message[0] = '\0';
            stringAppend(message, sizeof(message), "engine: container versions seen —");
            for (bucket = 0U; bucket < DISC_CONTENT_VERSION_BUCKETS; bucket++)
            {
                if (discSearch.versionsSeen[bucket] == 0U)
                {
                    continue;
                }
                anyVersion = BOOLEAN_TRUE;
                stringAppend(message, sizeof(message), " v");
                appendCount(message, sizeof(message), bucket);
                if (bucket == DISC_CONTENT_VERSION_BUCKETS - 1U)
                {
                    stringAppend(message, sizeof(message), "+");
                }
                stringAppend(message, sizeof(message), " x");
                appendCount(message, sizeof(message), discSearch.versionsSeen[bucket]);
                stringAppend(message, sizeof(message), ";");
            }
            if (!anyVersion)
            {
                stringAppend(message, sizeof(message), " none reached the block header");
            }
            if (discSearch.sawUnknownMark)
            {
                stringAppend(message, sizeof(message), " first non-0xFFFF0001 mark ");
                appendHexadecimal(message, sizeof(message), discSearch.firstUnknownMark);
                stringAppend(message, sizeof(message), ";");
            }
            if (discSearch.largestElementCount > 0U)
            {
                stringAppend(message, sizeof(message), " largest element count ");
                appendCount(message, sizeof(message), discSearch.largestElementCount);
                stringAppend(message, sizeof(message), ";");
            }
            platformLogMessage(message);
        }

        if (status != DISC_CONTENT_FOUND)
        {
            reportDiscFailure(discContentStatusGetName(status));
            return discLoadStatus;
        }

        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: drawing ");
        stringAppend(message, sizeof(message), discSearch.mesh.name);
        /* Named only when a shape led here. A container taken outright is not a
           model, and saying it is would make the log agree with a claim the
           engine has not earned. */
        if (discSearch.foundThroughScenegraph)
        {
            stringAppend(message, sizeof(message), " of model ");
            stringAppend(message, sizeof(message), discSearch.modelName);
        }
        else
        {
            stringAppend(message, sizeof(message), " (no shape, container taken directly)");
        }
        stringAppend(message, sizeof(message), " from ");
        stringAppend(message, sizeof(message), discSearch.packagePath);
        platformLogMessage(message);

        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: ");
        appendCount(message, sizeof(message), discSearch.mesh.vertexCount);
        stringAppend(message, sizeof(message), " vertices, ");
        appendCount(message, sizeof(message), discSearch.mesh.indexCount / 3U);
        stringAppend(message, sizeof(message), " triangles, ");
        appendCount(message, sizeof(message), discSearch.mesh.storedPrimitiveCount);
        stringAppend(message, sizeof(message), " of ");
        appendCount(message, sizeof(message), discSearch.mesh.primitiveCount);
        stringAppend(message, sizeof(message), " primitive(s) drawn, from ");
        appendCount(message, sizeof(message), discSearch.mesh.componentCount);
        stringAppend(message, sizeof(message), " component(s)");
        platformLogMessage(message);

        /* Where the model's tree says this part belongs. Nothing applies it
           yet — one shape is drawn, and a single part's own transform is
           usually identity — so it is reported rather than claimed. The number
           to watch is whether every block was walked: a tree that stopped short
           has parts this engine cannot reach at all. */
        if (discSearch.modelHasTree)
        {
            const TransformNode *node = &discSearch.modelTree.nodes[discSearch.modelNodeIndex];

            message[0] = '\0';
            stringAppend(message, sizeof(message), "engine: tree ");
            stringAppend(message, sizeof(message), discSearch.modelTree.resourceName);
            stringAppend(message, sizeof(message), " — ");
            appendCount(message, sizeof(message), discSearch.modelTree.blocksRead);
            stringAppend(message, sizeof(message), " of ");
            appendCount(message, sizeof(message), discSearch.modelTree.blockCount);
            stringAppend(message, sizeof(message), " blocks walked, ");
            appendCount(message, sizeof(message), discSearch.modelTree.storedNodeCount);
            stringAppend(message, sizeof(message), " of ");
            appendCount(message, sizeof(message), discSearch.modelTree.nodeCount);
            stringAppend(message, sizeof(message), " node(s) kept");
            if (discSearch.modelTree.storedNodeCount > 0U)
            {
                stringAppend(message, sizeof(message), ", bone ");
                appendCount(message, sizeof(message), node->boneIdentifier);
            }
            /* Whether the node the part hangs from actually moved it. A model
               whose one node is its root is placed at the origin either way,
               and a transform that silently does nothing looks exactly like one
               that was never applied. */
            stringAppend(message, sizeof(message),
                         discSearch.partWasMoved ? ", which places the part"
                                                 : ", which leaves the part where it was");
            platformLogMessage(message);
        }

        /* What the model is made of, against what is being drawn. One part of
           several is a face where a Sim should be, and until this was counted
           nobody could see the difference in a log. */
        if (discSearch.partCount > 0U)
        {
            Unsigned32 part;

            message[0] = '\0';
            stringAppend(message, sizeof(message), "engine: ");
            appendCount(message, sizeof(message), discSearch.partCount);
            stringAppend(message, sizeof(message), " part(s) —");
            for (part = 0U; part < discSearch.partCount && part < 8U; part++)
            {
                stringAppend(message, sizeof(message), " ");
                stringAppend(message, sizeof(message), discSearch.parts[part].meshName);
                stringAppend(message, sizeof(message), " (");
                appendCount(message, sizeof(message), discSearch.parts[part].indexCount / 3U);
                stringAppend(message, sizeof(message), " triangles)");
                if (discSearch.parts[part].materialName[0] != '\0')
                {
                    stringAppend(message, sizeof(message), " wearing ");
                    stringAppend(message, sizeof(message), discSearch.parts[part].materialName);
                }
                stringAppend(message, sizeof(message), ";");
            }
            /* Which one the whole model is painted with, while it is still
               painted with one. Naming it beside the others is what makes the
               compromise visible rather than mysterious. */
            if (discSearch.partCount > 1U && discSearch.materialName[0] != '\0')
            {
                stringAppend(message, sizeof(message), " all painted with ");
                stringAppend(message, sizeof(message), discSearch.materialName);
            }
            if (discSearch.coarserPartsDropped > 0U)
            {
                stringAppend(message, sizeof(message), " ");
                appendCount(message, sizeof(message), discSearch.coarserPartsDropped);
                stringAppend(message, sizeof(message), " coarser copy(s) set aside;");
            }
            if (discSearch.partsBeyondRoom > 0U)
            {
                stringAppend(message, sizeof(message), " and ");
                appendCount(message, sizeof(message), discSearch.partsBeyondRoom);
                stringAppend(message, sizeof(message), " with no room to remember them");
            }
            platformLogMessage(message);
        }

        /* Where the rest of the model is. A Sim's tree names its body meshes by
           key, and those keys are not in the file that describes one Sim — they
           are in the packages the game ships, which are now mounted and
           indexed. This says how many of them there are to go after. */
        if (!discSearch.foundInPreferred && discSearch.modelHasTree)
        {
            /* Said when the model came from outside the game's own mesh
               directory. A character file describes one Sim's face and the
               skeleton under it; the body it wears is chosen at run time from
               resources it does not name, so there is nothing else in there to
               draw however hard this looks. */
            platformLogMessage("engine: this is not one of the game's own meshes, so it may hold "
                               "only a part of a model");
        }

        if (discSearch.shapeReferences > 0U)
        {
            message[0] = '\0';
            stringAppend(message, sizeof(message), "engine: the tree names ");
            appendCount(message, sizeof(message), discSearch.shapeReferences);
            stringAppend(message, sizeof(message), " shape(s), ");
            appendCount(message, sizeof(message), discSearch.shapeReferencesResolved);
            stringAppend(message, sizeof(message), " of them in this package");
            platformLogMessage(message);
        }

        /* The material and its texture. Nothing samples it yet, so this reports
           what was found rather than what is on screen — but a name that failed
           to match and a file that would not read are different problems, and
           this is where the difference shows. */
        if (discSearch.materialName[0] != '\0')
        {
            message[0] = '\0';
            stringAppend(message, sizeof(message), "engine: material ");
            stringAppend(message, sizeof(message), discSearch.materialName);
            if (!discSearch.materialFound)
            {
                /* The count separates a name that did not match from a package
                   with no materials in it at all. The second is not a lookup
                   bug: a Sim's face material is built from shared resources
                   that live elsewhere on the disc, and finding it needs a
                   wider search rather than a better comparison. */
                stringAppend(message, sizeof(message), " — not among the ");
                appendCount(message, sizeof(message), discSearch.materialsInPackage);
                stringAppend(message, sizeof(message), " material(s) in this package");
            }
            else if (!discSearch.textureFound)
            {
                stringAppend(message, sizeof(message), " — read, but its texture is not among the ");
                appendCount(message, sizeof(message), discSearch.texturesInPackage);
                stringAppend(message, sizeof(message), " here");
            }
            else
            {
                stringAppend(message, sizeof(message), " — texture ");
                stringAppend(message, sizeof(message), discSearch.texture.resourceName);
                stringAppend(message, sizeof(message), " ");
                appendCount(message, sizeof(message), (Unsigned32)discSearch.texture.width);
                stringAppend(message, sizeof(message), "x");
                appendCount(message, sizeof(message), (Unsigned32)discSearch.texture.height);
                stringAppend(message, sizeof(message), " ");
                stringAppend(message, sizeof(message),
                             textureFormatGetName(discSearch.texture.format));
                stringAppend(message, sizeof(message), ", level ");
                appendCount(message, sizeof(message), (Unsigned32)discSearch.texture.levelWidth);
                stringAppend(message, sizeof(message), "x");
                appendCount(message, sizeof(message), (Unsigned32)discSearch.texture.levelHeight);
                if (discSearch.texture.largestIsElsewhere)
                {
                    stringAppend(message, sizeof(message), ", top level in ");
                    stringAppend(message, sizeof(message), discSearch.texture.lifoName);
                }
            }
            platformLogMessage(message);
        }

        /* Each part by name. A model that arrives as one silhouette is hard to
           tell from a model that arrived as one part, and the difference
           matters as soon as materials do. */
        if (discSearch.mesh.storedPrimitiveCount > 1U)
        {
            Unsigned32 part;

            message[0] = '\0';
            stringAppend(message, sizeof(message), "engine: parts —");
            for (part = 0U; part < discSearch.mesh.storedPrimitiveCount; part++)
            {
                stringAppend(message, sizeof(message), " ");
                stringAppend(message, sizeof(message), discSearch.mesh.primitives[part].name);
                stringAppend(message, sizeof(message), " (");
                appendCount(message, sizeof(message),
                            discSearch.mesh.primitives[part].indexCount / 3U);
                stringAppend(message, sizeof(message), " triangles);");
            }
            platformLogMessage(message);
        }

        /* The image the material asked for was not in its own package. A Sim's
           face texture lives in the shared skin packages, so this is the normal
           case rather than a failure — but it needs a search of the whole disc,
           which is a phase of its own. */
        if (!discSearch.textureFound && discSearch.textureName[0] != '\0' &&
            discSearch.mesh.textureCoordinates != NULL_POINTER)
        {
            /* Images and the resources that hold a single mip level of one.
               Both are indexed because a texture whose top level lives in a
               LIFO may be filed either way, and looking for only one of them
               and finding nothing proves nothing about the other. */
            static const Unsigned32 wantedTypes[2] = { (Unsigned32)PACKAGE_TYPE_TXTR,
                                                       (Unsigned32)PACKAGE_TYPE_LIFO };

            if (resourceIndexBegin(&textureIndex, discFileSystem, globalArena,
                                   TEXTURE_INDEX_CAPACITY, wantedTypes, 2U))
            {
                message[0] = '\0';
                stringAppend(message, sizeof(message), "engine: looking for ");
                stringAppend(message, sizeof(message), discSearch.textureName);
                stringAppend(message, sizeof(message), " across the rest of the disc");
                platformLogMessage(message);
                discPhase = DISC_PHASE_INDEX;
                return ENGINE_DISC_WORKING;
            }
            platformLogMessage("engine: not enough room to index the disc for a texture");
        }

        /* The mesh first here too, for the same reason: this is the path taken
           when a package held its own texture, and it had the same ordering. */
        renderSetMesh(&discSearch.mesh, globalArena);
        uploadFoundTexture();
        discPhase = DISC_PHASE_DONE;
        discLoadStatus = ENGINE_DISC_READY;
    }
    return discLoadStatus;
}

/* For a platform whose reads never answer PENDING. Nothing here is fatal: an
   engine that will not start because a disc was unreadable is worse than one
   that starts and says so. */
static void loadDiscContent(VirtualFileSystem *fileSystem)
{
    Unsigned32 remaining = 1000000U;

    if (fileSystem == NULL_POINTER)
    {
        return;
    }
    engineBeginDiscLoad(fileSystem);
    while (engineStepDiscLoad() == ENGINE_DISC_WORKING && remaining > 0U)
    {
        remaining--;
    }
}

Boolean engineInitialize(const EngineConfiguration *configuration)
{
    Boolean renderIsReady;

    if (engineIsRunning == BOOLEAN_TRUE)
    {
        return BOOLEAN_TRUE;
    }

    globalArena = memoryBudgetGetGlobalArena();

    if (profilerInitialize(globalArena) == BOOLEAN_FALSE)
    {
        platformLogMessage("engine: profiler unavailable, continuing without it");
    }

    profilerReportText = (char *)memoryArenaAllocate(globalArena, VICTORIA_PROFILER_REPORT_CAPACITY, 16UL);
    if (profilerReportText != NULL_POINTER)
    {
        profilerReportText[0] = '\0';
    }

    /* Before the renderer, so the very first resource it creates is already
       counted against the ceiling. */
    establishGraphicsMemoryLimit(configuration->graphicsMemoryLimitBytes);

    /* Bracketed as a frame of its own. Shader compilation and warm-up are the
       most expensive things the engine ever does, and zones entered outside a
       frame accumulate nothing — they would have reported as zero, hiding
       exactly the cost this is here to expose. Startup therefore shows up as
       frame one, and as the worst frame until something beats it. */
    profilerBeginFrame();
    renderIsReady = renderInitialize(globalArena, configuration->widthInPixels,
                                     configuration->heightInPixels);
    profilerEndFrame();

    if (renderIsReady == BOOLEAN_FALSE)
    {
        platformLogMessage("engine: renderer failed to initialize");
        return BOOLEAN_FALSE;
    }

    /* After the renderer, because the backend may want to upload what it is
       given, and before the first frame so nothing is drawn twice. */
    loadDiscContent(configuration->fileSystem);

    engineIsRunning = BOOLEAN_TRUE;
    platformLogMessage("engine: initialized");
    logMemoryBudget();
    return BOOLEAN_TRUE;
}

void engineResize(Unsigned32 widthInPixels, Unsigned32 heightInPixels)
{
    if (engineIsRunning == BOOLEAN_FALSE)
    {
        return;
    }
    renderResize(widthInPixels, heightInPixels);
}

void engineBeginFrame(void)
{
    if (engineIsRunning == BOOLEAN_FALSE)
    {
        return;
    }
    profilerBeginFrame();
}

void engineRenderFrame(Real32 elapsedSeconds)
{
    if (engineIsRunning == BOOLEAN_FALSE)
    {
        return;
    }

    VICTORIA_PROFILE_ZONE_BEGIN("engineRenderFrame");
    renderDrawFrame(elapsedSeconds);
    VICTORIA_PROFILE_ZONE_END();
}

void engineEndFrame(void)
{
    Unsigned64 nowMicroseconds;

    if (engineIsRunning == BOOLEAN_FALSE)
    {
        return;
    }

    profilerEndFrame();

    if (profilerReportText == NULL_POINTER)
    {
        return;
    }

    /* The first frame always produces a report. Waiting for the interval would
       otherwise leave readers with nothing for a quarter second, and on a clock
       whose origin is near zero it would look like the profiler was dead. */
    nowMicroseconds = platformGetMicroseconds();
    if (profilerReportText[0] == '\0' ||
        nowMicroseconds - lastReportMicroseconds >= ENGINE_REPORT_INTERVAL_MICROSECONDS)
    {
        MemorySize reportLength =
            profilerWriteReport(profilerReportText, VICTORIA_PROFILER_REPORT_CAPACITY);

        /* Composed here rather than inside the profiler: system memory and
           graphics memory are tracked by different modules and neither should
           have to know about the other. */
        graphicsMemoryBudgetWriteReport(profilerReportText + reportLength,
                                        VICTORIA_PROFILER_REPORT_CAPACITY - reportLength);
        lastReportMicroseconds = nowMicroseconds;
    }
}

const char *engineGetProfilerReportText(void)
{
    if (profilerReportText == NULL_POINTER)
    {
        return "";
    }
    return profilerReportText;
}

void engineShutdown(void)
{
    if (engineIsRunning == BOOLEAN_FALSE)
    {
        return;
    }

    renderShutdown();
    engineIsRunning = BOOLEAN_FALSE;
    platformLogMessage("engine: shut down");
}

MemoryArena *engineGetGlobalArena(void)
{
    return globalArena;
}
