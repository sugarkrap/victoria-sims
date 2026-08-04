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
    DISC_PHASE_INDEX,
    DISC_PHASE_FETCH_TEXTURE,
    DISC_PHASE_DONE
} DiscPhase;

static DiscPhase discPhase = DISC_PHASE_CONTENT;
static ResourceIndex textureIndex;

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
        appendCount(message, sizeof(message), textureIndex.count);
        stringAppend(message, sizeof(message), " texture(s)");
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
        discPhase = DISC_PHASE_FETCH_TEXTURE;
        return ENGINE_DISC_WORKING;
    }

    if (discPhase == DISC_PHASE_FETCH_TEXTURE)
    {
        char wanted[RESOURCE_NAME_LIMIT];
        const ResourceIndexEntry *found;

        materialBuildResourceName(wanted, sizeof(wanted), discSearch.textureName, "_txtr");
        found = resourceIndexFindNamed(&textureIndex, (Unsigned32)PACKAGE_TYPE_TXTR, wanted);

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

        discCatalogueIsBuilt = BOOLEAN_TRUE;
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
            static const Unsigned32 wantedTypes[1] = { (Unsigned32)PACKAGE_TYPE_TXTR };

            if (resourceIndexBegin(&textureIndex, discFileSystem, globalArena,
                                   TEXTURE_INDEX_CAPACITY, wantedTypes, 1U))
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
