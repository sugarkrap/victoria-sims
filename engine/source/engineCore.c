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


/* How many files a disc catalogue can hold. A retail disc lists a few hundred;
   this leaves room without the catalogue itself becoming the cost. */
#define DISC_FILE_LIMIT 4096U

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
    DISC_PHASE_INDEX,
    DISC_PHASE_FETCH_TEXTURE,
    DISC_PHASE_DONE
} DiscPhase;

static DiscPhase discPhase = DISC_PHASE_CONTENT;
static ResourceIndex textureIndex;

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
    /* Where in the file the mark sits. Nearly always the very front, but a
       program that carries an archive puts its own mark past the header it had
       to start with — and that mark is the informative one, because "a Windows
       program" is what every installer on every disc looks like. */
    MemorySize offset;
    const char *bytes;
    MemorySize length;
    const char *name;
} FileSignature;

/* Installer loaders write their offset table at 0x30, past the DOS stub. Six
   characters rather than the full mark, because the two bytes after it are a
   format version and this only needs to say which family it is. */
#define INSTALLER_LOADER_OFFSET 0x30UL

static const FileSignature fileSignatures[] = {
    { 0UL, "DBPF", 4UL, "a package by content, whatever it is called" },
    { 0UL, "MSCF", 4UL, "a Microsoft cabinet" },
    { 0UL, "ISc(", 4UL, "an InstallShield cabinet" },
    { 0UL, "Rar!", 4UL, "a RAR archive" },
    { 0UL, "PK\x03\x04", 4UL, "a zip archive" },
    { 0UL, "PK\x05\x06", 4UL, "an empty zip archive" },
    { 0UL, "7z\xBC\xAF", 4UL, "a 7-zip archive" },
    { 0UL, "\x1F\x8B", 2UL, "gzip" },
    { 0UL, "BSDIFF", 6UL, "a binary patch" },
    /* Before the plain program marks, because this is a program and the fact
       that it is an installer carrying a payload is the part worth knowing. */
    { INSTALLER_LOADER_OFFSET, "rDlPtS", 6UL,
      "an Inno Setup installer — the payload is inside it, LZMA compressed" },
    /* Last, because a self-extracting archive of any of the above is also a
       Windows program, and the specific answer is the useful one. Delphi's
       linker writes MZP where Microsoft's writes MZ, which is worth separating:
       every installer builder worth the name is a Delphi program. */
    { 0UL, "MZP", 3UL, "a Delphi-built program, which on a file this size means an installer" },
    { 0UL, "MZ", 2UL, "a Windows program — anything inside is appended, not at the front" }
};

static const char *identifySignature(const Unsigned8 *bytes, MemorySize byteCount)
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
            return signature->name;
        }
    }
    return "unrecognised";
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

/* Reads the texture the index pointed at, out of whichever package holds it.
 * Answers false while the bytes are still on their way. */
static Boolean fetchIndexedTexture(const ResourceIndexEntry *found, Boolean *succeeded)
{
    Unsigned8 *bytes;
    VirtualReadResult read;
    MemorySize size = (MemorySize)found->sizeInBytes;

    *succeeded = BOOLEAN_FALSE;
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

    if (textureReaderOpen(&discSearch.texture, bytes, size) == TEXTURE_READ_OK)
    {
        discSearch.textureFound = BOOLEAN_TRUE;
        *succeeded = BOOLEAN_TRUE;
    }
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
            MemorySize marker = memoryArenaGetMarker(globalArena);

            if (!fetchIndexedTexture(found, &succeeded))
            {
                memoryArenaRewindToMarker(globalArena, marker);
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
            uploadFoundTexture();
            /* Held until the upload has copied it, then given back. */
            memoryArenaRewindToMarker(globalArena, marker);
        }
        else
        {
            message[0] = '\0';
            stringAppend(message, sizeof(message), "engine: ");
            stringAppend(message, sizeof(message), wanted);
            stringAppend(message, sizeof(message), " is nowhere on this disc");
            platformLogMessage(message);
        }

        renderSetMesh(&discSearch.mesh, globalArena);
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
            discPhase = DISC_PHASE_CONTENT;
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
        stringAppend(message, sizeof(message), identifySignature(head, headSize));
        stringAppend(message, sizeof(message), ", starting ");
        {
            /* The bytes as well as the verdict. A name this reader does not know
               is exactly the case where the bytes themselves are what somebody
               needs to see — and the same goes for the mark at 0x30, which is
               where a program that carries an archive says so. */
            appendHexadecimalBytes(message, sizeof(message), head, headSize, 0UL, 8UL);
            if (headSize >= INSTALLER_LOADER_OFFSET + 8UL)
            {
                stringAppend(message, sizeof(message), "and at 0x30 ");
                appendHexadecimalBytes(message, sizeof(message), head, headSize,
                                       INSTALLER_LOADER_OFFSET, 8UL);
            }
        }
        platformLogMessage(message);
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

        uploadFoundTexture();
        renderSetMesh(&discSearch.mesh, globalArena);
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
