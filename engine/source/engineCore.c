#include "victoria/discContent.h"
#include "victoria/discReader.h"
#include "victoria/engineCore.h"
#include "victoria/jpegReader.h"
#include "victoria/pngReader.h"
#include "victoria/tgaReader.h"
#include "victoria/uiLayoutReader.h"
#include "victoria/freestandingRuntime.h"
#include "utils/strings.h"
#include "victoria/graphicsMemoryBudget.h"
#include "victoria/memoryBudget.h"
#include "victoria/platformInterface.h"
#include "victoria/profiler.h"
#include "victoria/propertySet.h"
#include "victoria/resourceKeyList.h"
#include "victoria/renderInterface.h"
#include "victoria/compression.h"
#include "victoria/resourceIndex.h"
#include "victoria/installerReader.h"
#include "victoria/archiveReader.h"
#include "victoria/programReader.h"
#include "victoria/textureDecode.h"
#include "victoria/wardrobe.h"
#include "victoria/debugMenu.h"
#include "victoria/resourceCache.h"
#include "victoria/fontAtlas.h"
#include "victoria/engineText.h"
#include "utils/checksum.h"

#define ENGINE_REPORT_INTERVAL_MICROSECONDS 250000ULL

static MemoryArena *globalArena = NULL_POINTER;
static Boolean engineIsRunning = BOOLEAN_FALSE;
static char *profilerReportText = NULL_POINTER;
static Unsigned64 lastReportMicroseconds = 0ULL;

#define MAIN_MENU_MAX_FRAMES_PER_IMAGE 64U
#define MAIN_MENU_SECONDS_PER_FRAME 0.125f

#define MAIN_MENU_SURFACE_WIDTH 1024U
#define MAIN_MENU_SURFACE_HEIGHT 768U
#define UI_IMAGE_TYPE_IDENTIFIER 0x856DDBACUL

typedef struct MainMenuImage
{
    const Unsigned8 *pixels;
    Unsigned32 width;
    Unsigned32 height;
    Unsigned32 frameCount;
    Unsigned32 frameWidth;
    Unsigned32 frameHeight;
    Unsigned32 currentFrame;
    const Unsigned8 *framePixels[MAIN_MENU_MAX_FRAMES_PER_IMAGE];
    Boolean isAnimated;
    Real32 frameAccumulator;
} MainMenuImage;

typedef enum MainMenuPhase
{
    MAIN_MENU_PHASE_DISC_LOAD = 0,
    MAIN_MENU_PHASE_BUILD_INDEX,
    MAIN_MENU_PHASE_LOAD_LAYOUT,
    MAIN_MENU_PHASE_READ_LAYOUT,
    MAIN_MENU_PHASE_LOAD_IMAGES,
    MAIN_MENU_PHASE_RESUME_DISC_LOAD,
    MAIN_MENU_PHASE_READY
} MainMenuPhase;

static InterfaceSurface mainMenuSurface;
static UILayoutDescription mainMenuLayout;
static MainMenuImage mainMenuImages[UI_LAYOUT_ELEMENT_LIMIT];
static Unsigned32 mainMenuImageCursor = 0U;
static Boolean mainMenuScreenDrawn = BOOLEAN_FALSE;
static Boolean mainMenuPointerIsInside = BOOLEAN_FALSE;
static Integer32 mainMenuPointerX = 0;
static Integer32 mainMenuPointerY = 0;
static Integer32 mainMenuHoveredElementIndex = -1;
static Integer32 mainMenuPressedElementIndex = -1;
static Boolean gameModeIsReal = BOOLEAN_FALSE;
static MainMenuPhase mainMenuPhase = MAIN_MENU_PHASE_DISC_LOAD;
static ResourceIndex mainMenuIndex;
static const ResourceIndexEntry *mainMenuLayoutEntry = NULL_POINTER;
static Unsigned32 mainMenuWindowWidth = 1024U;
static Unsigned32 mainMenuWindowHeight = 768U;

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
        platformLogMessage("graphics memory: backend did not report a size, assuming the default");
    }
}

#define DISC_FILE_LIMIT 16384U

static void appendCount(char *destination, MemorySize capacity, Unsigned32 value)
{
    char digits[24];

    if (stringWriteUnsigned(digits, sizeof(digits), (Unsigned64)value) > 0UL)
    {
        stringAppend(destination, capacity, digits);
    }
}

static void appendThousandths(char *destination, MemorySize capacity, Real32 value)
{
    Unsigned32 scaled;
    Unsigned32 fraction;

    if (value < 0.0f)
    {
        value = -value;
        stringAppend(destination, capacity, "-");
    }
    if (value >= 1000000.0f)
    {
        stringAppend(destination, capacity, "a great deal");
        return;
    }
    scaled = (Unsigned32)(value * 1000.0f + 0.5f);
    appendCount(destination, capacity, scaled / 1000U);
    stringAppend(destination, capacity, ".");
    fraction = scaled % 1000U;
    if (fraction < 100U)
    {
        stringAppend(destination, capacity, "0");
    }
    if (fraction < 10U)
    {
        stringAppend(destination, capacity, "0");
    }
    appendCount(destination, capacity, fraction);
}

static void appendHexadecimal(char *destination, MemorySize capacity, Unsigned32 value)
{
    char digits[24];

    if (stringWriteHexadecimal(digits, sizeof(digits), (Unsigned64)value, 8UL) > 0UL)
    {
        stringAppend(destination, capacity, digits);
    }
}

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

#define CATALOGUE_LISTING_LIMIT 12U

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

static VirtualFileSystem *discFileSystem = NULL_POINTER;
static DiscReader discReader;
static DiscContentSearch discSearch;
static EngineDiscLoadStatus discLoadStatus = ENGINE_DISC_IDLE;
static Boolean discCatalogueIsBuilt = BOOLEAN_FALSE;

#define TEXTURE_INDEX_CAPACITY 32768U

typedef enum DiscPhase
{
    DISC_PHASE_CONTENT = 0,
    DISC_PHASE_PROBE,
    DISC_PHASE_INSTALLER,
    DISC_PHASE_INDEX,
    DISC_PHASE_FETCH_TEXTURE,
    DISC_PHASE_FETCH_LEVEL,
    DISC_PHASE_SEEK_SKIN,
    DISC_PHASE_SEEK_SIM,
    DISC_PHASE_SEEK_ANIMATION,
    DISC_PHASE_LIST_ANIMATIONS,
    DISC_PHASE_PLAY_CHOSEN,
    DISC_PHASE_DONE
} DiscPhase;

static DiscPhase discPhase = DISC_PHASE_CONTENT;
static ResourceIndex textureIndex;

static ResourceIndex skinIndex;
static Boolean skinIndexBegun = BOOLEAN_FALSE;
static Boolean skinIndexBuilding = BOOLEAN_FALSE;
static Unsigned32 skinCursor = 0U;
static Unsigned32 skinScanned = 0U;

#define SKIN_INDEX_CAPACITY 32768U

#define SKIN_SCAN_LIMIT 256U

static ResourceIndex animationIndex;
static Animation posedAnimation;
static Boolean animationIndexBegun = BOOLEAN_FALSE;
static Boolean animationIndexBuilding = BOOLEAN_FALSE;
static Unsigned32 animationCursor = 0U;
static Unsigned32 animationScanned = 0U;

#define ANIMATION_INDEX_CAPACITY 32768U
#define ANIMATION_SCAN_LIMIT 64U
#define ANIMATION_POSE_TICK 0.0f

#define ANIMATION_REST_POSE_NAME "a-pose-neutral-stand_anim"
static Boolean animationTriedNamed = BOOLEAN_FALSE;
static Boolean animationUsedRestPose = BOOLEAN_FALSE;

static Boolean poseIsAnimated = BOOLEAN_FALSE;
static Real32 poseTick = 0.0f;

#define SIM_PART_COUNT 4U
static char simArchetype[WARDROBE_ARCHETYPE_LIMIT] = "am";

static DebugMenu debugMenu;
#define MENU_BODY_CAPACITY 32U
static char menuBodyRows[MENU_BODY_CAPACITY][DEBUG_MENU_NAME_LIMIT];
static char menuBodyArchetypes[MENU_BODY_CAPACITY][WARDROBE_ARCHETYPE_LIMIT];
static char menuText[2048];

#define MENU_CLOTHING_CAPACITY (WARDROBE_PART_COUNT * WARDROBE_ALTERNATIVE_LIMIT)
static Unsigned8 menuClothingParts[MENU_CLOTHING_CAPACITY];
static char menuClothingNames[MENU_CLOTHING_CAPACITY][WARDROBE_NAME_LIMIT];
static Unsigned32 menuClothingCount = 0U;
static const ResourceIndexEntry *menuClothingMaterials[MENU_CLOTHING_CAPACITY];

#define THUMBNAIL_SIZE 64U
#define THUMBNAIL_SLOT_COUNT 64U

typedef struct ThumbnailSlot
{
    Unsigned32 row;
    Boolean ready;
    Unsigned8 pixels[THUMBNAIL_SIZE * THUMBNAIL_SIZE * 4U];
} ThumbnailSlot;

static ThumbnailSlot thumbnailSlots[THUMBNAIL_SLOT_COUNT];

typedef enum ThumbnailHop
{
    THUMBNAIL_HOP_IDLE = 0,
    THUMBNAIL_HOP_JPEG
} ThumbnailHop;

static ThumbnailHop thumbnailHop = THUMBNAIL_HOP_IDLE;
static Unsigned32 thumbnailNextRow = 0U;
static Unsigned32 thumbnailActiveSlot = 0U;
static const ResourceIndexEntry *thumbnailLoadEntry = NULL_POINTER;

#define PACKAGE_TYPE_JPEG          0x856DDBACUL
#define PACKAGE_TYPE_CATALOG_INDEX 0x43494745UL

static const Unsigned8 *bstCatalogData  = NULL_POINTER;
static Unsigned32        bstCatalogCount = 0U;
static Boolean           bstCatalogLoading = BOOLEAN_FALSE;

#define MENU_ANIMATION_CAPACITY 512U
static const ResourceIndexEntry *menuAnimationEntries[MENU_ANIMATION_CAPACITY];
static Unsigned32 menuAnimationCount = 0U;
static Unsigned32 menuAnimationCursor = 0U;
static Unsigned32 menuAnimationOpened = 0U;
static const ResourceIndexEntry *simWardrobeAnimationWanted = NULL_POINTER;

#define ANIMATION_ARENA_BYTES (8UL * 1024UL * 1024UL)
static MemoryArena animationArena;
static Boolean animationArenaReady = BOOLEAN_FALSE;

static ResourceCache resourceCache;
static char simPartNames[SIM_PART_COUNT][RESOURCE_NAME_LIMIT];
static ResourceIndex simIndex;
static ResourceNodeDescription simPartTree;
static Boolean simIndexBegun = BOOLEAN_FALSE;
static Boolean simIndexBuilding = BOOLEAN_FALSE;
static Unsigned32 simPartCursor = 0U;
static Unsigned32 simPartsFound = 0U;
static Unsigned32 simDrawnPartsFound = 0U;
static Unsigned32 simPartFileIndex = 0U;
static Boolean saidWhatTheDiscHas = BOOLEAN_FALSE;

typedef enum SimHop
{
    SIM_HOP_TREE = 0,
    SIM_HOP_SHAPE,
    SIM_HOP_NODE,
    SIM_HOP_CONTAINER,
    SIM_HOP_MERGE,
    SIM_HOP_SKELETON,
    SIM_HOP_MATERIAL,
    SIM_HOP_TEXTURE,
    SIM_HOP_TOP_LEVEL,
    SIM_HOP_CATALOGUE,
    SIM_HOP_WARDROBE,
    SIM_HOP_FINISHED
} SimHop;

static SimHop simHop = SIM_HOP_TREE;
static Unsigned32 simHopPart = 0U;
static ResourceNodeDescription simHopTree;
static ShapeDescription simHopShape;
static const ResourceIndexEntry *simHopEntry = NULL_POINTER;
static TextureDescription simHopTexture;
static char simHopTextureName[RESOURCE_NAME_LIMIT];
static Boolean simHopOverrode = BOOLEAN_FALSE;
static Boolean simHopWearsItsOwn = BOOLEAN_FALSE;

#define SIM_PART_COUNT_DRAWN 5U
#define SIM_PART_BODY 0U
#define SIM_PART_FACE 1U
#define SIM_PART_HAIR 2U
#define SIM_PART_TOP 3U
#define SIM_PART_BOTTOM 4U
#define SIM_BASE_PART_COUNT 3U
static char simDrawnPartNames[SIM_PART_COUNT_DRAWN][RESOURCE_NAME_LIMIT];
static GeometryMesh simParts[SIM_PART_COUNT_DRAWN];
static Boolean simPartLoaded[SIM_PART_COUNT_DRAWN];
static const ResourceIndexEntry *simPartMaterialEntries[SIM_PART_COUNT_DRAWN][RENDER_PART_LIMIT];
static Unsigned32 simJoinParts[SIM_PART_COUNT_DRAWN];
static Unsigned32 simJoinCount = 0U;

#define SIM_MORPH_WEIGHT_LIMIT 64U
static Real32 simMorphWeights[SIM_MORPH_WEIGHT_LIMIT];

static Boolean poseIsHeldStill = BOOLEAN_FALSE;
static Real32 poseHeldTick = 0.0f;
static Unsigned32 simMorphChannels = 0U;

#define SIM_MORPH_MOVER_LIMIT 32U
#define SIM_MORPH_VISIBLE_SHIFT 0.002f
#define SIM_MORPH_SECONDS_PER_CHANNEL 4.0f
static Unsigned32 simMorphMovers[SIM_MORPH_MOVER_LIMIT];
static Unsigned32 simMorphMoverCount = 0U;
static Unsigned32 simMorphShowing = 0xFFFFFFFFUL;
static Unsigned32 simMorphHeldChannel = 0U;
static char simPartMaterials[SIM_PART_COUNT_DRAWN][RENDER_PART_LIMIT][RESOURCE_NAME_LIMIT];

static Unsigned32 simRangeCount = 0U;
static char simRangeMaterials[RENDER_PART_LIMIT][RESOURCE_NAME_LIMIT];
static const ResourceIndexEntry *simRangeMaterialEntries[RENDER_PART_LIMIT];
static Unsigned32 simRangeOfPart[RENDER_PART_LIMIT];

#define CATALOGUE_OVERRIDE_LIMIT 8U

static Wardrobe simWardrobe;
static const ResourceIndexEntry *simWardrobeShapes[WARDROBE_PART_COUNT];
static char simWardrobeOverrideSubsets[WARDROBE_PART_COUNT][CATALOGUE_OVERRIDE_LIMIT]
                                      [PROPERTY_NAME_LIMIT];
static const ResourceIndexEntry
    *simWardrobeOverrideMaterials[WARDROBE_PART_COUNT][CATALOGUE_OVERRIDE_LIMIT];
static Unsigned32 simWardrobeOverrideCount[WARDROBE_PART_COUNT];
static const ResourceIndexEntry
    *simWardrobeAlternativeMaterials[WARDROBE_PART_COUNT][WARDROBE_ALTERNATIVE_LIMIT];
static char simWardrobeWanted[WARDROBE_NAME_LIMIT];
static char simWardrobeWantedPart[WARDROBE_PART_COUNT][WARDROBE_NAME_LIMIT];
static Boolean simWardrobeWorn = BOOLEAN_FALSE;
static Unsigned32 simWardrobePart = 0U;
static const ResourceIndexEntry *simWardrobeEntry = NULL_POINTER;
static ShapeDescription simWardrobeShape;
static Unsigned32 simWardrobeDressed = 0U;

typedef enum WardrobeStage
{
    WARDROBE_STAGE_SHAPE = 0,
    WARDROBE_STAGE_NODE,
    WARDROBE_STAGE_CONTAINER
} WardrobeStage;

static WardrobeStage simWardrobeStage = WARDROBE_STAGE_SHAPE;

static char simPartTextureStems[SIM_PART_COUNT_DRAWN][RESOURCE_NAME_LIMIT];
static char simSkinTone[RESOURCE_NAME_LIMIT];
static Boolean simIsAssembled = BOOLEAN_FALSE;

static MemorySize simAssemblyMarker = 0UL;
static Boolean simAssemblyMarkerValid = BOOLEAN_FALSE;

#define SIM_INDEX_CAPACITY 131072U

static MemorySize textureFetchMarker = 0UL;

static Unsigned64 probeCeilingSize = 0xFFFFFFFFFFFFFFFFULL;
static Unsigned32 probeCeilingIndex = 0U;
static Unsigned32 probesDone = 0U;

#define PROBE_LIMIT 12U

#define NO_INSTALLER 0xFFFFFFFFUL
static Unsigned32 installerFileIndex = (Unsigned32)NO_INSTALLER;
static Unsigned32 installerStage = 0U;
static InstallerOffsetTable installerTable;

#define INSTALLER_SCAN_LIMIT_BYTES (32ULL * 1024ULL * 1024ULL)
#define INSTALLER_SCAN_CHUNK_BYTES (256UL * 1024UL)

#define ARCHIVE_WALK_LIMIT 20000U
#define ARCHIVE_NAME_LIMIT_IN_LOG 8U

static Unsigned64 archiveBlockOffset = 0ULL;
static Unsigned32 archiveEntriesWalked = 0U;
static Unsigned32 archiveStoredCount = 0U;
static Unsigned32 archivePackedCount = 0U;
static Unsigned64 archiveStoredBytes = 0ULL;
static Unsigned32 archiveMountedCount = 0U;
static Unsigned32 archiveUnmountableCount = 0U;
static Unsigned32 archiveFirstMountedIndex = 0xFFFFFFFFUL;

static Unsigned64 installerScanOffset = 0ULL;
static Unsigned64 installerScanFrom = 0ULL;
static Unsigned64 installerVersionOffset = (Unsigned64)INSTALLER_MARKER_NOT_FOUND;

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

typedef struct FileSignature
{
    Boolean worthFollowing;
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
    { BOOLEAN_TRUE, INSTALLER_LOADER_HEADER_OFFSET, "rDlPtS", 6UL,
      "an Inno Setup installer" },
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

static void mountArchiveEntry(const VirtualFileEntry *containingFile, const ArchiveEntry *archiveEntry)
{
    MemorySize nameLength;
    char *storedName;

    if (archiveEntry->isDirectory || archiveEntry->unpackedSizeInBytes == 0ULL ||
        !stringEndsWithIgnoringCase(archiveEntry->name, ".package"))
    {
        return;
    }

    nameLength = stringLength(archiveEntry->name);
    storedName = (char *)memoryArenaAllocate(globalArena, nameLength + 1UL, 1UL);
    if (storedName == NULL_POINTER)
    {
        archiveUnmountableCount++;
        return;
    }
    storedName[0] = '\0';
    stringAppend(storedName, nameLength + 1UL, archiveEntry->name);

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
        return BOOLEAN_FALSE;
    }
    if (read != VIRTUAL_READ_OK)
    {
        return BOOLEAN_TRUE;
    }

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

static Boolean fetchLargestLevel(char *message, MemorySize messageCapacity)
{
    char wanted[RESOURCE_NAME_LIMIT];
    const ResourceIndexEntry *found;
    Unsigned8 *bytes;
    MemorySize size;
    TextureLevel largest;
    TextureReadResult opened;

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
        stringAppend(message, messageCapacity, "would not open — ");
        stringAppend(message, messageCapacity, textureReadResultGetName(opened));
        stringAppend(message, messageCapacity, ", starting ");
        appendHexadecimalBytes(message, messageCapacity, bytes, size, 0UL, 8UL);
        platformLogMessage(message);
        return BOOLEAN_TRUE;
    }
    if (largest.bytes == NULL_POINTER || largest.width <= discSearch.texture.levelWidth)
    {
        stringAppend(message, messageCapacity, "is no larger than the one already read");
        platformLogMessage(message);
        return BOOLEAN_TRUE;
    }
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

typedef enum SimAssembly
{
    SIM_ASSEMBLY_PENDING = 0,
    SIM_ASSEMBLY_FAILED,
    SIM_ASSEMBLY_DONE
} SimAssembly;

static void reportSimPart(Unsigned32 partIndex, DiscModelResult result)
{
    char message[256];

    message[0] = '\0';
    stringAppend(message, sizeof(message), "engine:   ");
    stringAppend(message, sizeof(message), simDrawnPartNames[partIndex]);
    stringAppend(message, sizeof(message), " — ");
    stringAppend(message, sizeof(message), discModelResultGetName(result));
    platformLogMessage(message);
}

static void reportWholeShape(const char *partName, const ShapeDescription *shape)
{
    char message[512];
    Unsigned32 index;

    message[0] = '\0';
    stringAppend(message, sizeof(message), "engine:   ");
    stringAppend(message, sizeof(message), partName);
    stringAppend(message, sizeof(message), " names ");
    appendCount(message, sizeof(message), shape->meshCount);
    stringAppend(message, sizeof(message), " geometry node(s) and ");
    appendCount(message, sizeof(message), shape->materialCount);
    stringAppend(message, sizeof(message), " material binding(s)");
    platformLogMessage(message);

    for (index = 0U; index < shape->storedMeshCount; index++)
    {
        if (shape->meshNames[index][0] == '\0')
        {
            continue;
        }
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine:     node ");
        stringAppend(message, sizeof(message), shape->meshNames[index]);
        stringAppend(message, sizeof(message), " at detail ");
        appendCount(message, sizeof(message), shape->meshLevelsOfDetail[index]);
        stringAppend(message, sizeof(message),
                     (resourceIndexFindNamed(&simIndex, (Unsigned32)PACKAGE_TYPE_GMND,
                                             shape->meshNames[index]) != NULL_POINTER)
                         ? " — on this disc"
                         : " — not found by that name");
        platformLogMessage(message);
    }
    for (index = 0U; index < shape->storedMaterialCount; index++)
    {
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine:     binds ");
        stringAppend(message, sizeof(message), shape->materials[index].primitiveName);
        stringAppend(message, sizeof(message), " to ");
        stringAppend(message, sizeof(message), shape->materials[index].materialName);
        platformLogMessage(message);
    }
}

static void reportMorphTargets(const GeometryMesh *part)
{
    char message[512];
    Unsigned32 index;

    if (part->morphTargetCount == 0U)
    {
        return;
    }

    message[0] = '\0';
    stringAppend(message, sizeof(message), "engine:     ");
    appendCount(message, sizeof(message), part->morphTargetCount);
    stringAppend(message, sizeof(message), " deformation channel(s) declared — ");
    for (index = 0U; index < part->morphTargetCount; index++)
    {
        if (index == 8U)
        {
            stringAppend(message, sizeof(message), "and ");
            appendCount(message, sizeof(message), part->morphTargetCount - index);
            stringAppend(message, sizeof(message), " more");
            break;
        }
        stringAppend(message, sizeof(message), part->morphTargets[index].groupName);
        stringAppend(message, sizeof(message), "/");
        stringAppend(message, sizeof(message), part->morphTargets[index].channelName);
        stringAppend(message, sizeof(message), "; ");
    }
    platformLogMessage(message);

    message[0] = '\0';
    stringAppend(message, sizeof(message), "engine:     ");
    if (part->morphSlotCount == 0U)
    {
        stringAppend(message, sizeof(message), "and carries nothing to move by them");
    }
    else
    {
        Unsigned32 touched = 0U;
        Unsigned32 vertex;

        for (vertex = 0U; vertex < part->vertexCount; vertex++)
        {
            Unsigned32 slot;

            for (slot = 0U; slot < part->morphSlotCount; slot++)
            {
                if (part->morphSlotChannels[(MemorySize)vertex * part->morphSlotCount + slot] != 0U)
                {
                    touched++;
                    break;
                }
            }
        }
        stringAppend(message, sizeof(message), "carrying ");
        appendCount(message, sizeof(message), part->morphSlotCount);
        stringAppend(message, sizeof(message),
                     (part->morphChannelsInferred == BOOLEAN_TRUE)
                         ? " delta set(s) whose channels were INFERRED from an empty map read for "
                         : " delta set(s) over a map read for ");
        appendCount(message, sizeof(message), part->morphMappedVertexCount);
        stringAppend(message, sizeof(message), " vertices, reaching ");
        appendCount(message, sizeof(message), touched);
        stringAppend(message, sizeof(message), " of its ");
        appendCount(message, sizeof(message), part->vertexCount);
    }
    platformLogMessage(message);
}

static void reportDeformationReach(const GeometryMesh *mesh)
{
    char message[512];
    Unsigned32 channel;
    Unsigned32 unreached = 0U;

    simMorphMoverCount = 0U;
    if (mesh->morphTargetCount == 0U)
    {
        return;
    }
    if (mesh->morphSlotChannels == NULL_POINTER || mesh->morphSlotCount == 0U)
    {
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: ");
        appendCount(message, sizeof(message), mesh->morphTargetCount);
        stringAppend(message, sizeof(message),
                     " deformation channel(s) declared and nothing carried to move with them");
        platformLogMessage(message);
        return;
    }

    for (channel = 1U; channel < mesh->morphTargetCount; channel++)
    {
        Unsigned32 vertices = 0U;
        Real32 furthest = 0.0f;
        Unsigned32 vertex;

        for (vertex = 0U; vertex < mesh->vertexCount; vertex++)
        {
            Unsigned32 slot;

            for (slot = 0U; slot < mesh->morphSlotCount; slot++)
            {
                MemorySize at = (MemorySize)vertex * mesh->morphSlotCount + slot;
                Real32 lengthSquared;

                if ((Unsigned32)mesh->morphSlotChannels[at] != channel)
                {
                    continue;
                }
                vertices++;
                lengthSquared = (mesh->morphSlotDeltas[at * 3UL] * mesh->morphSlotDeltas[at * 3UL]) +
                                (mesh->morphSlotDeltas[at * 3UL + 1UL] *
                                 mesh->morphSlotDeltas[at * 3UL + 1UL]) +
                                (mesh->morphSlotDeltas[at * 3UL + 2UL] *
                                 mesh->morphSlotDeltas[at * 3UL + 2UL]);
                if (lengthSquared > furthest)
                {
                    furthest = lengthSquared;
                }
            }
        }
        if (vertices == 0U)
        {
            unreached++;
            continue;
        }
        if (mathSquareRoot(furthest) >= SIM_MORPH_VISIBLE_SHIFT &&
            simMorphMoverCount < SIM_MORPH_MOVER_LIMIT)
        {
            simMorphMovers[simMorphMoverCount] = channel;
            simMorphMoverCount++;
        }
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine:   channel ");
        appendCount(message, sizeof(message), channel);
        stringAppend(message, sizeof(message), " ");
        stringAppend(message, sizeof(message), mesh->morphTargets[channel].groupName);
        stringAppend(message, sizeof(message), "/");
        stringAppend(message, sizeof(message), mesh->morphTargets[channel].channelName);
        stringAppend(message, sizeof(message), " moves ");
        appendCount(message, sizeof(message), vertices);
        stringAppend(message, sizeof(message), " vertices, furthest by ");
        appendThousandths(message, sizeof(message), mathSquareRoot(furthest));
        platformLogMessage(message);
    }

    message[0] = '\0';
    stringAppend(message, sizeof(message), "engine: ");
    appendCount(message, sizeof(message), mesh->morphTargetCount - 1U - unreached);
    stringAppend(message, sizeof(message), " of ");
    appendCount(message, sizeof(message), mesh->morphTargetCount - 1U);
    stringAppend(message, sizeof(message), " declared channel(s) are reached by a vertex, ");
    appendCount(message, sizeof(message), simMorphMoverCount);
    stringAppend(message, sizeof(message), " of them by enough to see");
    platformLogMessage(message);
}

static void restartTheAssembly(void)
{
    Unsigned32 index;

    if (simAssemblyMarkerValid)
    {
        memoryArenaRewindToMarker(globalArena, simAssemblyMarker);
    }
    simAssemblyMarker = memoryArenaGetMarker(globalArena);
    simAssemblyMarkerValid = BOOLEAN_TRUE;

    simHop = SIM_HOP_TREE;
    simHopPart = 0U;
    simHopEntry = NULL_POINTER;
    simJoinCount = 0U;
    simRangeCount = 0U;
    for (index = 0U; index < (Unsigned32)SIM_PART_COUNT_DRAWN; index++)
    {
        simPartLoaded[index] = BOOLEAN_FALSE;
    }
    for (index = 0U; index < (Unsigned32)WARDROBE_PART_COUNT; index++)
    {
        simWardrobeShapes[index] = NULL_POINTER;
        simWardrobeOverrideCount[index] = 0U;
    }
    simWardrobeWorn = BOOLEAN_FALSE;
    simWardrobePart = 0U;
    simWardrobeStage = WARDROBE_STAGE_SHAPE;
    simWardrobeEntry = NULL_POINTER;
    simWardrobeDressed = 0U;
    simIsAssembled = BOOLEAN_FALSE;
    simPartCursor = 0U;
    simPartsFound = 0U;
    simDrawnPartsFound = 0U;
    saidWhatTheDiscHas = BOOLEAN_FALSE;
    simSkinTone[0] = '\0';
    poseIsAnimated = BOOLEAN_FALSE;

    discPhase = DISC_PHASE_SEEK_SIM;
    discLoadStatus = ENGINE_DISC_WORKING;
}

static Unsigned32 menuArchetypeCount = 0U;

static void reportArchetypesOnThisDisc(void)
{
    static const char *const ages[] = { "b", "p", "c", "t", "y", "a", "e" };
    static const char *const genders[] = { "m", "f", "u" };
    char message[512];
    Unsigned32 age;

    debugMenuClearPage(&debugMenu, DEBUG_MENU_PAGE_BODY);
    menuArchetypeCount = 0U;

    for (age = 0U; age < VICTORIA_ARRAY_LENGTH(ages); age++)
    {
        char skeleton[RESOURCE_NAME_LIMIT];
        Unsigned32 gender;
        Unsigned32 found = 0U;

        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine:   ");
        skeleton[0] = '\0';
        stringAppend(skeleton, sizeof(skeleton), ages[age]);
        stringAppend(skeleton, sizeof(skeleton), "uskel_cres");
        stringAppend(message, sizeof(message), skeleton);
        stringAppend(message, sizeof(message),
                     (resourceIndexFindNamed(&simIndex, (Unsigned32)PACKAGE_TYPE_CRES, skeleton) !=
                      NULL_POINTER)
                         ? " — on this disc, wearing"
                         : " — NOT on this disc, so nothing of this age can be posed; wearing");

        for (gender = 0U; gender < VICTORIA_ARRAY_LENGTH(genders); gender++)
        {
            static const char *const parts[SIM_BASE_PART_COUNT] = { "BodyNaked_cres", "Face_cres",
                                                                    "HairBald_cres" };
            Unsigned32 which;
            Unsigned32 present = 0U;
            char had[64];

            had[0] = '\0';
            for (which = 0U; which < (Unsigned32)SIM_BASE_PART_COUNT; which++)
            {
                char name[RESOURCE_NAME_LIMIT];

                name[0] = '\0';
                stringAppend(name, sizeof(name), ages[age]);
                stringAppend(name, sizeof(name), genders[gender]);
                stringAppend(name, sizeof(name), parts[which]);
                if (resourceIndexFindNamed(&simIndex, (Unsigned32)PACKAGE_TYPE_CRES, name) !=
                    NULL_POINTER)
                {
                    present++;
                    stringAppend(had, sizeof(had), (which == 0U) ? "b" : ((which == 1U) ? "f" : "h"));
                }
            }
            if (present > 0U)
            {
                if (menuArchetypeCount < (Unsigned32)MENU_BODY_CAPACITY)
                {
                    char label[DEBUG_MENU_NAME_LIMIT];

                    label[0] = '\0';
                    stringAppend(label, sizeof(label), ages[age]);
                    stringAppend(label, sizeof(label), genders[gender]);
                    menuBodyArchetypes[menuArchetypeCount][0] = '\0';
                    stringAppend(menuBodyArchetypes[menuArchetypeCount],
                                 WARDROBE_ARCHETYPE_LIMIT, label);
                    stringAppend(label, sizeof(label), "  ");
                    stringAppend(label, sizeof(label), had);
                    if (stringEqualsIgnoringCase(menuBodyArchetypes[menuArchetypeCount],
                                                 simArchetype))
                    {
                        debugMenuSetInEffect(&debugMenu, DEBUG_MENU_PAGE_BODY,
                                             menuArchetypeCount);
                    }
                    (void)debugMenuAddRow(&debugMenu, DEBUG_MENU_PAGE_BODY, label);
                    menuArchetypeCount++;
                }
                found++;
                stringAppend(message, sizeof(message), " ");
                stringAppend(message, sizeof(message), ages[age]);
                stringAppend(message, sizeof(message), genders[gender]);
                stringAppend(message, sizeof(message), "(");
                stringAppend(message, sizeof(message), had);
                stringAppend(message, sizeof(message), ");");
            }
        }
        if (found == 0U)
        {
            stringAppend(message, sizeof(message), " nothing");
        }
        platformLogMessage(message);
    }
}

static void settleTheSkeleton(void)
{
    char message[384];

    if (simPartNames[0][0] == '\0' ||
        resourceIndexFindNamed(&simIndex, (Unsigned32)PACKAGE_TYPE_CRES, simPartNames[0]) !=
            NULL_POINTER)
    {
        return;
    }
    message[0] = '\0';
    stringAppend(message, sizeof(message), "engine: ");
    stringAppend(message, sizeof(message), simPartNames[0]);
    stringAppend(message, sizeof(message), " is not on this disc, so this Sim is hung on ");
    if (resourceIndexFindNamed(&simIndex, (Unsigned32)PACKAGE_TYPE_CRES, "auskel_cres") !=
        NULL_POINTER)
    {
        simPartNames[0][0] = '\0';
        stringAppend(simPartNames[0], RESOURCE_NAME_LIMIT, "auskel_cres");
        stringAppend(message, sizeof(message),
                     "auskel_cres instead — an age with no skeleton of its own is one whose "
                     "meshes are weighted to the adult's");
    }
    else
    {
        stringAppend(message, sizeof(message),
                     "nothing at all, so it will be drawn in its bind pose");
    }
    platformLogMessage(message);
}

static void composeTheArchetype(void)
{
    static const char *const partSuffixes[SIM_PART_COUNT_DRAWN] = { "BodyNaked_cres",
                                                                    "Face_cres",
                                                                    "HairBald_cres", "", "" };
    Unsigned32 part;

    simPartNames[0][0] = '\0';
    if (simArchetype[0] != '\0')
    {
        char age[2];

        age[0] = simArchetype[0];
        age[1] = '\0';
        stringAppend(simPartNames[0], RESOURCE_NAME_LIMIT, age);
        stringAppend(simPartNames[0], RESOURCE_NAME_LIMIT, "uskel_cres");
    }

    for (part = 0U; part < (Unsigned32)SIM_PART_COUNT_DRAWN; part++)
    {
        simDrawnPartNames[part][0] = '\0';
        simPartTextureStems[part][0] = '\0';
        if (partSuffixes[part][0] == '\0')
        {
            stringAppend(simDrawnPartNames[part], RESOURCE_NAME_LIMIT,
                         (part == (Unsigned32)SIM_PART_TOP) ? "a top" : "a bottom");
            continue;
        }
        stringAppend(simDrawnPartNames[part], RESOURCE_NAME_LIMIT, simArchetype);
        stringAppend(simDrawnPartNames[part], RESOURCE_NAME_LIMIT, partSuffixes[part]);
        simPartNames[part + 1U][0] = '\0';
        stringAppend(simPartNames[part + 1U], RESOURCE_NAME_LIMIT, simDrawnPartNames[part]);
    }

    stringAppend(simPartTextureStems[SIM_PART_FACE], RESOURCE_NAME_LIMIT, simArchetype);
    stringAppend(simPartTextureStems[SIM_PART_FACE], RESOURCE_NAME_LIMIT, "face");
}

static void bindPartMaterials(Unsigned32 slot, const ShapeDescription *shape)
{
    Unsigned32 primitive;
    Unsigned32 index;

    for (primitive = 0U; primitive < (Unsigned32)RENDER_PART_LIMIT; primitive++)
    {
        simPartMaterials[slot][primitive][0] = '\0';
        simPartMaterialEntries[slot][primitive] = NULL_POINTER;
    }
    for (primitive = 0U; primitive < simParts[slot].storedPrimitiveCount &&
                        primitive < (Unsigned32)RENDER_PART_LIMIT;
         primitive++)
    {
        for (index = 0U; index < shape->storedMaterialCount; index++)
        {
            if (simParts[slot].primitives[primitive].name[0] == '\0' ||
                shape->materials[index].primitiveName[0] == '\0')
            {
                continue;
            }
            if (stringEqualsIgnoringCase(shape->materials[index].primitiveName,
                                         simParts[slot].primitives[primitive].name))
            {
                stringAppend(simPartMaterials[slot][primitive], RESOURCE_NAME_LIMIT,
                             shape->materials[index].materialName);
                break;
            }
        }
    }
}

static SimAssembly finishThePart(MemorySize marker)
{
    char message[256];
    MemorySize wantedBytes =
        textureDecodeGetRequiredBytes(simHopTexture.levelWidth, simHopTexture.levelHeight);
    MemorySize stage = memoryArenaGetMarker(globalArena);
    Unsigned8 *decoded = (Unsigned8 *)memoryArenaAllocate(globalArena, wantedBytes, 4UL);
    TextureDecodeResult decodeResult = TEXTURE_DECODE_DESTINATION_TOO_SMALL;

    (void)marker;
    if (decoded != NULL_POINTER)
    {
        decodeResult = textureDecodeLevel(decoded, wantedBytes, simHopTexture.bytes,
                                          simHopTexture.byteCount, simHopTexture.format,
                                          simHopTexture.levelWidth, simHopTexture.levelHeight);
    }
    message[0] = '\0';
    stringAppend(message, sizeof(message), "engine:   range ");
    appendCount(message, sizeof(message), simHopPart);
    stringAppend(message, sizeof(message), " of ");
    stringAppend(message, sizeof(message), simDrawnPartNames[simRangeOfPart[simHopPart]]);
    stringAppend(message, sizeof(message), " painted with ");
    stringAppend(message, sizeof(message), simHopTextureName);
    if (simHopWearsItsOwn)
    {
        stringAppend(message, sizeof(message), " (which its catalogue entry named, not its shape)");
    }
    else if (simHopOverrode)
    {
        stringAppend(message, sizeof(message), " (overriding what its shape bound)");
    }
    if (decodeResult == TEXTURE_DECODE_OK)
    {
        renderSetPartTexture(simHopPart, decoded, (Unsigned32)simHopTexture.levelWidth,
                             (Unsigned32)simHopTexture.levelHeight);
        stringAppend(message, sizeof(message), " at ");
        appendCount(message, sizeof(message), (Unsigned32)simHopTexture.levelWidth);
        stringAppend(message, sizeof(message), "x");
        appendCount(message, sizeof(message), (Unsigned32)simHopTexture.levelHeight);
        if (!simHopOverrode && simSkinTone[0] == '\0')
        {
            MemorySize at = stringLength(simHopTextureName);

            while (at > 0UL && simHopTextureName[at - 1UL] != '-')
            {
                at -= 1UL;
            }
            if (at > 0UL)
            {
                stringAppend(simSkinTone, sizeof(simSkinTone), &simHopTextureName[at]);
            }
        }
    }
    else
    {
        stringAppend(message, sizeof(message), " — which would not decode: ");
        stringAppend(message, sizeof(message), textureDecodeResultGetName(decodeResult));
    }
    platformLogMessage(message);
    memoryArenaRewindToMarker(globalArena, stage);
    simHopPart++;
    simHop = SIM_HOP_MATERIAL;
    return SIM_ASSEMBLY_PENDING;
}

#define CATALOGUE_SAMPLE_LIMIT 2000U
#define CATALOGUE_KIND_LIMIT 16U
static Unsigned32 catalogueCursor = 0U;
static Unsigned32 catalogueStride = 0U;
static Unsigned32 catalogueSeen = 0U;
static Unsigned32 catalogueTotalEntries = 0U;
static Unsigned32 catalogueRead = 0U;
static Unsigned32 catalogueKindCount = 0U;
static char catalogueKinds[CATALOGUE_KIND_LIMIT][PROPERTY_NAME_LIMIT];
static char catalogueExamples[CATALOGUE_KIND_LIMIT][PROPERTY_NAME_LIMIT];
static Unsigned32 catalogueKindTotals[CATALOGUE_KIND_LIMIT];
static Unsigned32 catalogueWithShape = 0U;
static Unsigned32 catalogueNotBinary = 0U;
static Boolean catalogueWantsKeyList = BOOLEAN_FALSE;
static Unsigned32 catalogueShapeIndex = 0U;
static Unsigned32 catalogueEntryGroup = 0U;
static Unsigned32 catalogueEntryInstance = 0U;
static Unsigned32 catalogueEntryInstanceHigh = 0U;
static char catalogueEntryName[PROPERTY_NAME_LIMIT];
static Unsigned32 catalogueResolved = 0U;
static Unsigned32 catalogueNoSidecar = 0U;
static Unsigned32 catalogueIndexPastEnd = 0U;
static Unsigned32 catalogueShapeMissing = 0U;
static Unsigned32 catalogueShown = 0U;
static Unsigned32 catalogueNotAShape = 0U;
static Unsigned32 catalogueKeysShown = 0U;
static Unsigned32 catalogueNamedMeshlessShown = 0U;

#define CATALOGUE_CATEGORY_LIMIT 24U

typedef struct SlotTally
{
    Unsigned32 values[CATALOGUE_CATEGORY_LIMIT];
    Unsigned32 totals[CATALOGUE_CATEGORY_LIMIT];
    Unsigned32 shapes[CATALOGUE_CATEGORY_LIMIT];
    Unsigned32 overlays[CATALOGUE_CATEGORY_LIMIT];
    char examples[CATALOGUE_CATEGORY_LIMIT][PROPERTY_NAME_LIMIT];
    Unsigned32 count;
    Unsigned32 beyondRoom;
} SlotTally;

static SlotTally catalogueByCategory;
static SlotTally catalogueByOutfit;
#define CATALOGUE_DUMP_LIMIT 4U
static char catalogueUncategorisedDumps[CATALOGUE_DUMP_LIMIT][512];
static Unsigned32 catalogueUncategorisedShown = 0U;
static char catalogueNamedDumps[CATALOGUE_DUMP_LIMIT][512];
static Unsigned32 catalogueNamedShown = 0U;

static Unsigned32 catalogueOverrideCount = 0U;
static char catalogueOverrideSubsets[CATALOGUE_OVERRIDE_LIMIT][PROPERTY_NAME_LIMIT];
static Unsigned32 catalogueOverrideKeyIndex[CATALOGUE_OVERRIDE_LIMIT];
static Unsigned32 catalogueOverridesBeyondRoom = 0U;
static Unsigned32 catalogueOverridesResolved = 0U;
static Unsigned32 catalogueOverridesNotAMaterial = 0U;
static Unsigned32 catalogueEntryCategorySlot = CATALOGUE_CATEGORY_LIMIT;
static Unsigned32 catalogueEntryOutfitSlot = CATALOGUE_CATEGORY_LIMIT;
static Unsigned32 catalogueEntryOutfit = 0U;
#define CATALOGUE_OUTFIT_FACE 0x02U
#define CATALOGUE_FACE_LIMIT 16U
static char catalogueFaceDumps[CATALOGUE_FACE_LIMIT][384];
static Unsigned32 catalogueFaceShown = 0U;

static const char *resourceTypeGetName(Unsigned32 typeIdentifier)
{
    switch (typeIdentifier)
    {
    case (Unsigned32)PACKAGE_TYPE_CRES:
        return "transform tree";
    case (Unsigned32)PACKAGE_TYPE_SHPE:
        return "shape";
    case (Unsigned32)PACKAGE_TYPE_GMND:
        return "geometry node";
    case (Unsigned32)PACKAGE_TYPE_GMDC:
        return "geometry container";
    case (Unsigned32)PACKAGE_TYPE_TXMT:
        return "material";
    case (Unsigned32)PACKAGE_TYPE_TXTR:
        return "texture";
    case (Unsigned32)PACKAGE_TYPE_LIFO:
        return "mip level";
    case (Unsigned32)PACKAGE_TYPE_ANIM:
        return "animation";
    case 0xEBCF3E27UL:
        return "catalogue entry";
    case (Unsigned32)0xAC506764UL:
        return "key list";
    case 0x4D51F042UL:
        return "face modifier";
    case 0xCCCEF852UL:
        return "face lighting";
    case 0x8C1580B5UL:
        return "hair tone";
    default:
        return NULL_POINTER;
    }
}

static Unsigned32 rememberSlot(SlotTally *tally, Unsigned32 value, const char *name)
{
    Unsigned32 index;

    for (index = 0U; index < tally->count; index++)
    {
        if (tally->values[index] == value)
        {
            break;
        }
    }
    if (index == tally->count)
    {
        if (tally->count >= (Unsigned32)CATALOGUE_CATEGORY_LIMIT)
        {
            tally->beyondRoom++;
            return (Unsigned32)CATALOGUE_CATEGORY_LIMIT;
        }
        tally->values[index] = value;
        tally->totals[index] = 0U;
        tally->shapes[index] = 0U;
        tally->overlays[index] = 0U;
        tally->examples[index][0] = '\0';
        tally->count++;
    }
    tally->totals[index]++;
    if (tally->examples[index][0] == '\0' && name[0] != '\0')
    {
        stringAppend(tally->examples[index], PROPERTY_NAME_LIMIT, name);
    }
    return index;
}
typedef enum CatalogueFollow
{
    CATALOGUE_FOLLOW_IDLE = 0,
    CATALOGUE_FOLLOW_SHAPE,
    CATALOGUE_FOLLOW_NODE,
    CATALOGUE_FOLLOW_CONTAINER
} CatalogueFollow;

#define CATALOGUE_FOLLOW_LIMIT 8U
static CatalogueFollow catalogueFollow = CATALOGUE_FOLLOW_IDLE;
static const ResourceIndexEntry *catalogueFollowEntry = NULL_POINTER;
static Unsigned32 catalogueFollowed = 0U;
static char catalogueFollowName[PROPERTY_NAME_LIMIT];

static void rememberCatalogueKind(const char *kind, const char *name)
{
    Unsigned32 index;

    for (index = 0U; index < catalogueKindCount; index++)
    {
        if (stringEqualsIgnoringCase(catalogueKinds[index], kind) == BOOLEAN_TRUE)
        {
            catalogueKindTotals[index]++;
            if (catalogueExamples[index][0] == '\0' && name[0] != '\0')
            {
                stringAppend(catalogueExamples[index], PROPERTY_NAME_LIMIT, name);
            }
            return;
        }
    }
    if (catalogueKindCount >= CATALOGUE_KIND_LIMIT)
    {
        return;
    }
    catalogueKinds[catalogueKindCount][0] = '\0';
    stringAppend(catalogueKinds[catalogueKindCount], PROPERTY_NAME_LIMIT, kind);
    catalogueExamples[catalogueKindCount][0] = '\0';
    stringAppend(catalogueExamples[catalogueKindCount], PROPERTY_NAME_LIMIT, name);
    catalogueKindTotals[catalogueKindCount] = 1U;
    catalogueKindCount++;
}

static void reportOneTally(const SlotTally *tally, const char *what, const char *meaning)
{
    char message[512];
    Unsigned32 index;

    if (tally->count == 0U)
    {
        return;
    }
    message[0] = '\0';
    stringAppend(message, sizeof(message), "engine: by ");
    stringAppend(message, sizeof(message), what);
    stringAppend(message, sizeof(message), " — ");
    appendCount(message, sizeof(message), tally->count);
    stringAppend(message, sizeof(message), " value(s) across the ");
    appendCount(message, sizeof(message), catalogueRead);
    stringAppend(message, sizeof(message), " entr(ies) read, a sample and not the lot; ");
    stringAppend(message, sizeof(message), meaning);
    if (tally->beyondRoom > 0U)
    {
        stringAppend(message, sizeof(message), " (");
        appendCount(message, sizeof(message), tally->beyondRoom);
        stringAppend(message, sizeof(message), " more with no room to record)");
    }
    platformLogMessage(message);

    for (index = 0U; index < tally->count; index++)
    {
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine:   ");
        appendHexadecimal(message, sizeof(message), tally->values[index]);
        stringAppend(message, sizeof(message), " — ");
        appendCount(message, sizeof(message), tally->totals[index]);
        stringAppend(message, sizeof(message), " entr(ies), ");
        appendCount(message, sizeof(message), tally->shapes[index]);
        stringAppend(message, sizeof(message), " reaching a mesh, ");
        appendCount(message, sizeof(message), tally->overlays[index]);
        stringAppend(message, sizeof(message), " painting one instead, such as ");
        stringAppend(message, sizeof(message),
                     (tally->examples[index][0] != '\0') ? tally->examples[index] : "(unnamed)");
        platformLogMessage(message);
    }
}

static void reportCatalogueSlots(Boolean withDumps)
{
    Unsigned32 which;

    reportOneTally(&catalogueByCategory, "category",
                   "which outfit categories a thing belongs to, not which part it dresses");
    reportOneTally(&catalogueByOutfit, "outfit", "which part of a Sim it dresses");

    if (!withDumps)
    {
        return;
    }
    for (which = 0U; which < catalogueUncategorisedShown; which++)
    {
        platformLogMessage(catalogueUncategorisedDumps[which]);
    }
    for (which = 0U; which < catalogueFaceShown; which++)
    {
        platformLogMessage(catalogueFaceDumps[which]);
    }
}

static void reportCatalogue(void)
{
    char message[512];
    Unsigned32 index;

    message[0] = '\0';
    stringAppend(message, sizeof(message), "engine: read ");
    appendCount(message, sizeof(message), catalogueRead);
    stringAppend(message, sizeof(message), " catalogue entr(ies), ");
    appendCount(message, sizeof(message), catalogueWithShape);
    stringAppend(message, sizeof(message), " of them naming a shape, ");
    appendCount(message, sizeof(message), catalogueNotBinary);
    stringAppend(message, sizeof(message), " spelled as XML rather than the binary form");
    platformLogMessage(message);

    {
        Unsigned32 sidecars = 0U;
        Unsigned32 entries = 0U;
        Unsigned32 at;

        for (at = 0U; at < simIndex.count; at++)
        {
            if (simIndex.entries[at].typeIdentifier == (Unsigned32)PACKAGE_TYPE_RESOURCE_KEY_LIST)
            {
                sidecars++;
            }
            else if (simIndex.entries[at].typeIdentifier == (Unsigned32)PACKAGE_TYPE_SKIN_ENTRY)
            {
                entries++;
            }
        }
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: the index holds ");
        appendCount(message, sizeof(message), entries);
        stringAppend(message, sizeof(message), " catalogue entr(ies) and ");
        appendCount(message, sizeof(message), sidecars);
        stringAppend(message, sizeof(message), " key list(s), of ");
        appendCount(message, sizeof(message), simIndex.count);
        stringAppend(message, sizeof(message), " indexed and ");
        appendCount(message, sizeof(message), simIndex.dropped);
        stringAppend(message, sizeof(message), " dropped for want of room");
        platformLogMessage(message);
    }

    message[0] = '\0';
    stringAppend(message, sizeof(message), "engine: followed ");
    appendCount(message, sizeof(message), catalogueResolved);
    stringAppend(message, sizeof(message), " of them to a shape on this disc — ");
    appendCount(message, sizeof(message), catalogueNoSidecar);
    stringAppend(message, sizeof(message), " had no key list beside them, ");
    appendCount(message, sizeof(message), catalogueIndexPastEnd);
    stringAppend(message, sizeof(message), " indexed past the end of one, ");
    appendCount(message, sizeof(message), catalogueShapeMissing);
    stringAppend(message, sizeof(message), " named a shape the index does not hold, and ");
    appendCount(message, sizeof(message), catalogueNotAShape);
    stringAppend(message, sizeof(message), " named no mesh at all — an overlay or a tone");
    platformLogMessage(message);

    reportCatalogueSlots(BOOLEAN_TRUE);

    for (index = 0U; index < catalogueKindCount; index++)
    {
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine:   ");
        appendCount(message, sizeof(message), catalogueKindTotals[index]);
        stringAppend(message, sizeof(message), " of kind ");
        stringAppend(message, sizeof(message), (catalogueKinds[index][0] != '\0')
                                                   ? catalogueKinds[index]
                                                   : "(none declared)");
        stringAppend(message, sizeof(message), ", such as ");
        stringAppend(message, sizeof(message), (catalogueExamples[index][0] != '\0')
                                                   ? catalogueExamples[index]
                                                   : "(unnamed)");
        platformLogMessage(message);
    }
}

static void reportWardrobe(void)
{
    char message[512];
    Unsigned32 part;

    message[0] = '\0';
    stringAppend(message, sizeof(message), "engine: the wardrobe was offered ");
    appendCount(message, sizeof(message), simWardrobe.offered);
    stringAppend(message, sizeof(message), " entr(ies) that reach a shape and dresses ");
    appendCount(message, sizeof(message), wardrobeGetChosenCount(&simWardrobe));
    stringAppend(message, sizeof(message), " of ");
    appendCount(message, sizeof(message), (Unsigned32)WARDROBE_PART_COUNT);
    stringAppend(message, sizeof(message), " part(s)");
    if (simWardrobe.wanted[0] != '\0')
    {
        stringAppend(message, sizeof(message), ", asked for by the name ");
        stringAppend(message, sizeof(message), simWardrobe.wanted);
    }
    if (simWardrobe.tone[0] != '\0')
    {
        stringAppend(message, sizeof(message), ", at the tone ");
        stringAppend(message, sizeof(message), simWardrobe.tone);
    }
    platformLogMessage(message);

    message[0] = '\0';
    stringAppend(message, sizeof(message), "engine:   passed over ");
    appendCount(message, sizeof(message), simWardrobe.refusedUnnamed);
    stringAppend(message, sizeof(message), " unnamed, ");
    appendCount(message, sizeof(message), simWardrobe.refusedBySlot);
    stringAppend(message, sizeof(message), " dressing a part this Sim has not got, ");
    appendCount(message, sizeof(message), simWardrobe.refusedByMark);
    stringAppend(message, sizeof(message), " authored for another age or gender, ");
    appendCount(message, sizeof(message), simWardrobe.refusedAsWorn);
    stringAppend(message, sizeof(message), " already worn, and ");
    appendCount(message, sizeof(message), simWardrobe.refusedAsSettled);
    stringAppend(message, sizeof(message), " no better than what the part had settled on");
    if (simWardrobe.wanted[0] != '\0')
    {
        stringAppend(message, sizeof(message), "; ");
        appendCount(message, sizeof(message), simWardrobe.matchedWanted);
        stringAppend(message, sizeof(message), " were named as asked for");
    }
    platformLogMessage(message);

    message[0] = '\0';
    stringAppend(message, sizeof(message), "engine:   the entries it took named ");
    appendCount(message, sizeof(message), catalogueOverridesResolved);
    stringAppend(message, sizeof(message),
                 " material(s) of their own for their subsets, which is where a garment's "
                 "colourway is and what its shape's binding is not; ");
    appendCount(message, sizeof(message), catalogueOverridesNotAMaterial);
    stringAppend(message, sizeof(message), " named something that is not a material");
    if (catalogueOverridesBeyondRoom > 0U)
    {
        stringAppend(message, sizeof(message), "; ");
        appendCount(message, sizeof(message), catalogueOverridesBeyondRoom);
        stringAppend(message, sizeof(message), " entr(ies) declared more than there is room for");
    }
    platformLogMessage(message);

    message[0] = '\0';
    stringAppend(message, sizeof(message), "engine:   ");
    switch (wardrobeGetArrangement(&simWardrobe))
    {
    case WARDROBE_ARRANGEMENT_PAIR:
        stringAppend(message, sizeof(message),
                     "a top and a bottom, which between them replace the whole body — so the "
                     "body it was assembled with is not drawn");
        break;
    case WARDROBE_ARRANGEMENT_WHOLE:
        stringAppend(message, sizeof(message), "one whole-body garment, because ");
        if (simWardrobe.nameWanted[WARDROBE_PART_BODY])
        {
            stringAppend(message, sizeof(message),
                         "it is the one asked for by name — a top and a bottom were held and "
                         "passed over");
        }
        else
        {
            stringAppend(message, sizeof(message),
                         "a top and a bottom were not both offered, and half a pair is a Sim in "
                         "a shirt and nothing else");
        }
        break;
    default:
        stringAppend(message, sizeof(message),
                     "nothing that covers a body, so it keeps the one it was assembled with");
        break;
    }
    platformLogMessage(message);

    for (part = 0U; part < (Unsigned32)WARDROBE_PART_COUNT; part++)
    {
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine:   ");
        stringAppend(message, sizeof(message), simDrawnPartNames[part]);
        stringAppend(message, sizeof(message), " — ");
        if (wardrobeIsChosen(&simWardrobe, part))
        {
            stringAppend(message, sizeof(message),
                         wardrobeIsWorn(&simWardrobe, part) ? "wearing " : "chosen but not worn: ");
            stringAppend(message, sizeof(message), wardrobeGetChosenName(&simWardrobe, part));
            if (!simWardrobe.toneMatched[part])
            {
                stringAppend(message, sizeof(message), ", which is not this Sim's tone");
            }
        }
        else
        {
            stringAppend(message, sizeof(message),
                         "nothing in the sample was named ");
            stringAppend(message, sizeof(message), wardrobeGetPartMark(&simWardrobe, part));
            stringAppend(message, sizeof(message), " in outfit slot ");
            appendHexadecimal(message, sizeof(message), wardrobeGetPartOutfit(part));
            if (wardrobeGetPartWorn(part)[0] != '\0')
            {
                stringAppend(message, sizeof(message), " and not already ");
                stringAppend(message, sizeof(message), wardrobeGetPartWorn(part));
            }
        }
        platformLogMessage(message);

        if (simWardrobe.alternativeCount[part] > 0U)
        {
            Unsigned32 which;

            message[0] = '\0';
            stringAppend(message, sizeof(message), "engine:     or any of —");
            Unsigned32 shown = simWardrobe.alternativeCount[part];

            if (shown > (Unsigned32)WARDROBE_ALTERNATIVES_WORTH_LOGGING)
            {
                shown = (Unsigned32)WARDROBE_ALTERNATIVES_WORTH_LOGGING;
            }
            for (which = 0U; which < shown; which++)
            {
                stringAppend(message, sizeof(message), " ");
                stringAppend(message, sizeof(message),
                             wardrobeGetAlternative(&simWardrobe, part, which));
                stringAppend(message, sizeof(message), ";");
            }
            if (simWardrobe.alternativeCount[part] > shown ||
                wardrobeGetAlternativesBeyondRoom(&simWardrobe, part) > 0U)
            {
                stringAppend(message, sizeof(message), " and ");
                appendCount(message, sizeof(message),
                            simWardrobe.alternativeCount[part] - shown +
                                wardrobeGetAlternativesBeyondRoom(&simWardrobe, part));
                stringAppend(message, sizeof(message), " more, which the menu lists");
            }
            platformLogMessage(message);
        }
    }
}

static const char *shortenGarment(const char *name, const char *mark)
{
    MemorySize markLength = stringLength(mark);
    MemorySize index;

    if (markLength == 0UL)
    {
        return name;
    }
    for (index = 0UL; name[index] != '\0'; index++)
    {
        MemorySize step;

        for (step = 0UL; step < markLength; step++)
        {
            if (characterToLowerCase(name[index + step]) != characterToLowerCase(mark[step]))
            {
                break;
            }
        }
        if (step == markLength && name[index + markLength] != '\0')
        {
            return &name[index + markLength];
        }
    }
    return name;
}

static void fillTheClothingPage(void)
{
    Unsigned32 part;
    Unsigned32 wasAt = debugMenuGetCursor(&debugMenu, DEBUG_MENU_PAGE_CLOTHING);

    debugMenuClearPage(&debugMenu, DEBUG_MENU_PAGE_CLOTHING);
    menuClothingCount = 0U;
    {
        Unsigned32 r;
        for (r = 0U; r < (Unsigned32)MENU_CLOTHING_CAPACITY; r++)
        {
            menuClothingMaterials[r] = NULL_POINTER;
        }
    }
    {
        Unsigned32 s;
        for (s = 0U; s < (Unsigned32)THUMBNAIL_SLOT_COUNT; s++)
        {
            thumbnailSlots[s].row = (Unsigned32)MENU_CLOTHING_CAPACITY;
            thumbnailSlots[s].ready = BOOLEAN_FALSE;
        }
        thumbnailHop = THUMBNAIL_HOP_IDLE;
        thumbnailNextRow = 0U;
        thumbnailLoadEntry = NULL_POINTER;
    }
    for (part = 0U; part < (Unsigned32)WARDROBE_PART_COUNT; part++)
    {
        Unsigned32 which;
        Unsigned32 held = wardrobeGetAlternativeCount(&simWardrobe, part);

        const char *mark = wardrobeGetPartMark(&simWardrobe, part);

        for (which = 0U; which < held; which++)
        {
            const char *name = wardrobeGetAlternative(&simWardrobe, part, which);
            Unsigned32 row;

            if (name[0] == '\0' || menuClothingCount >= (Unsigned32)MENU_CLOTHING_CAPACITY)
            {
                continue;
            }
            row = debugMenuAddRow(&debugMenu, DEBUG_MENU_PAGE_CLOTHING, shortenGarment(name, mark));
            if (row == (Unsigned32)DEBUG_MENU_NONE)
            {
                continue;
            }
            menuClothingParts[row] = (Unsigned8)part;
            menuClothingNames[row][0] = '\0';
            stringAppend(menuClothingNames[row], (MemorySize)WARDROBE_NAME_LIMIT, name);
            menuClothingMaterials[row] = simWardrobeAlternativeMaterials[(Unsigned32)part][which];
            menuClothingCount = row + 1U;
            if (wardrobeIsWorn(&simWardrobe, part) &&
                stringEqualsIgnoringCase(name, wardrobeGetChosenName(&simWardrobe, part)))
            {
                debugMenuSetInEffect(&debugMenu, DEBUG_MENU_PAGE_CLOTHING, row);
            }
        }
    }
    (void)debugMenuSetCursor(&debugMenu, DEBUG_MENU_PAGE_CLOTHING, wasAt);
    {
        char message[256];

        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: the menu offers ");
        appendCount(message, sizeof(message), menuClothingCount);
        stringAppend(message, sizeof(message), " garment(s) to choose from");
        platformLogMessage(message);
    }
}

static SimAssembly stepTheSidecar(MemorySize marker)
{
    const ResourceIndexEntry *sidecar =
        resourceIndexFindInGroup(&simIndex, (Unsigned32)PACKAGE_TYPE_RESOURCE_KEY_LIST,
                                 catalogueEntryGroup, catalogueEntryInstance,
                                 catalogueEntryInstanceHigh);
    Unsigned8 *bytes;
    MemorySize size;

    catalogueWantsKeyList = BOOLEAN_FALSE;
    if (sidecar == NULL_POINTER)
    {
        catalogueNoSidecar++;
        return SIM_ASSEMBLY_PENDING;
    }
    if (!readIndexedResource(sidecar, &bytes, &size))
    {
        catalogueWantsKeyList = BOOLEAN_TRUE;
        memoryArenaRewindToMarker(globalArena, marker);
        return SIM_ASSEMBLY_PENDING;
    }
    if (bytes != NULL_POINTER)
    {
        ResourceKeyList list;

        if (resourceKeyListRead(&list, bytes, size) == RESOURCE_KEY_LIST_OK)
        {
            const ResourceKeyEntry *key = resourceKeyListGet(&list, catalogueShapeIndex);

            if (key != NULL_POINTER && key->typeIdentifier != (Unsigned32)PACKAGE_TYPE_SHPE)
            {
                catalogueNotAShape++;
                if (catalogueEntryCategorySlot < (Unsigned32)CATALOGUE_CATEGORY_LIMIT)
                {
                    catalogueByCategory.overlays[catalogueEntryCategorySlot]++;
                }
                if (catalogueEntryOutfitSlot < (Unsigned32)CATALOGUE_CATEGORY_LIMIT)
                {
                    catalogueByOutfit.overlays[catalogueEntryOutfitSlot]++;
                }
            }

            if (key == NULL_POINTER)
            {
                catalogueIndexPastEnd++;
            }
            else
            {
                const ResourceIndexEntry *shape =
                    resourceIndexFind(&simIndex, key->typeIdentifier, key->instanceIdentifier,
                                      key->instanceIdentifierHigh);

                if (key->typeIdentifier != (Unsigned32)PACKAGE_TYPE_SHPE)
                {
                }
                else if (shape == NULL_POINTER)
                {
                    catalogueShapeMissing++;
                }
                else
                {
                    catalogueResolved++;
                    if (catalogueEntryCategorySlot < (Unsigned32)CATALOGUE_CATEGORY_LIMIT)
                    {
                        catalogueByCategory.shapes[catalogueEntryCategorySlot]++;
                    }
                    if (catalogueEntryOutfitSlot < (Unsigned32)CATALOGUE_CATEGORY_LIMIT)
                    {
                        catalogueByOutfit.shapes[catalogueEntryOutfitSlot]++;
                    }
                    const ResourceIndexEntry *thumbnailFirstMaterial = NULL_POINTER;
                    Unsigned32 thumbnailPrevCounts[WARDROBE_PART_COUNT];
                    {
                        Unsigned32 thumbP;
                        for (thumbP = 0U; thumbP < (Unsigned32)WARDROBE_PART_COUNT; thumbP++)
                        {
                            thumbnailPrevCounts[thumbP] =
                                wardrobeGetAlternativeCount(&simWardrobe, thumbP);
                        }
                    }
                    if (catalogueOverrideCount > 0U)
                    {
                        const ResourceKeyEntry *first =
                            resourceKeyListGet(&list, catalogueOverrideKeyIndex[0]);
                        if (first != NULL_POINTER &&
                            first->typeIdentifier == (Unsigned32)PACKAGE_TYPE_TXMT)
                        {
                            thumbnailFirstMaterial = resourceIndexFind(
                                &simIndex, first->typeIdentifier,
                                first->instanceIdentifier, first->instanceIdentifierHigh);
                        }
                    }
                    {
                        Unsigned32 dresses = wardrobeOffer(&simWardrobe, catalogueEntryName,
                                                           catalogueEntryOutfit);

                        if (dresses != (Unsigned32)WARDROBE_NOT_WORN)
                        {
                            char worn[512];
                            Unsigned32 each;

                            simWardrobeShapes[dresses] = shape;
                            worn[0] = '\0';
                            stringAppend(worn, sizeof(worn), "engine:   the catalogue offers ");
                            stringAppend(worn, sizeof(worn), catalogueEntryName);
                            stringAppend(worn, sizeof(worn), " for ");
                            stringAppend(worn, sizeof(worn), simDrawnPartNames[dresses]);
                            if (!simWardrobe.toneMatched[dresses])
                            {
                                stringAppend(worn, sizeof(worn),
                                             ", which is not this Sim's tone — held until "
                                             "something better turns up");
                            }
                            platformLogMessage(worn);

                            simWardrobeOverrideCount[dresses] = 0U;
                            worn[0] = '\0';
                            stringAppend(worn, sizeof(worn), "engine:     painting");
                            for (each = 0U; each < catalogueOverrideCount; each++)
                            {
                                const ResourceKeyEntry *painted =
                                    resourceKeyListGet(&list, catalogueOverrideKeyIndex[each]);
                                const ResourceIndexEntry *material = NULL_POINTER;

                                stringAppend(worn, sizeof(worn), " ");
                                stringAppend(worn, sizeof(worn),
                                             catalogueOverrideSubsets[each]);
                                stringAppend(worn, sizeof(worn), " with ");
                                if (painted == NULL_POINTER)
                                {
                                    stringAppend(worn, sizeof(worn), "a key past the end;");
                                    continue;
                                }
                                if (painted->typeIdentifier != (Unsigned32)PACKAGE_TYPE_TXMT)
                                {
                                    const char *typeName =
                                        resourceTypeGetName(painted->typeIdentifier);

                                    catalogueOverridesNotAMaterial++;
                                    stringAppend(worn, sizeof(worn), "a ");
                                    stringAppend(worn, sizeof(worn),
                                                 (typeName != NULL_POINTER) ? typeName
                                                                            : "resource of another"
                                                                              " type");
                                    stringAppend(worn, sizeof(worn), " and not a material;");
                                    continue;
                                }
                                material =
                                    resourceIndexFind(&simIndex, painted->typeIdentifier,
                                                      painted->instanceIdentifier,
                                                      painted->instanceIdentifierHigh);
                                if (material == NULL_POINTER)
                                {
                                    stringAppend(worn, sizeof(worn),
                                                 "a material the index does not hold;");
                                    continue;
                                }
                                if (simWardrobeOverrideCount[dresses] <
                                    (Unsigned32)CATALOGUE_OVERRIDE_LIMIT)
                                {
                                    Unsigned32 at = simWardrobeOverrideCount[dresses];

                                    simWardrobeOverrideSubsets[dresses][at][0] = '\0';
                                    stringAppend(simWardrobeOverrideSubsets[dresses][at],
                                                 PROPERTY_NAME_LIMIT,
                                                 catalogueOverrideSubsets[each]);
                                    simWardrobeOverrideMaterials[dresses][at] = material;
                                    simWardrobeOverrideCount[dresses]++;
                                }
                                catalogueOverridesResolved++;
                                stringAppend(worn, sizeof(worn), "the material at key ");
                                appendCount(worn, sizeof(worn),
                                            catalogueOverrideKeyIndex[each]);
                                stringAppend(worn, sizeof(worn), ";");
                            }
                            if (catalogueOverrideCount == 0U)
                            {
                                stringAppend(worn, sizeof(worn),
                                             " nothing of its own — it wears whatever its shape "
                                             "binds, which for a garment is one arbitrary "
                                             "colourway");
                            }
                            platformLogMessage(worn);
                        }
                    }
                    {
                        Unsigned32 thumbP;
                        for (thumbP = 0U; thumbP < (Unsigned32)WARDROBE_PART_COUNT; thumbP++)
                        {
                            Unsigned32 after = wardrobeGetAlternativeCount(&simWardrobe, thumbP);
                            if (after > thumbnailPrevCounts[thumbP] &&
                                after <= (Unsigned32)WARDROBE_ALTERNATIVE_LIMIT)
                            {
                                simWardrobeAlternativeMaterials[thumbP][after - 1U] =
                                    thumbnailFirstMaterial;
                                break;
                            }
                        }
                    }
                    if (catalogueFollowed < CATALOGUE_FOLLOW_LIMIT &&
                        stringContainsIgnoringCase(catalogueEntryName, "body"))
                    {
                        catalogueFollowed++;
                        catalogueFollowEntry = shape;
                        catalogueFollow = CATALOGUE_FOLLOW_SHAPE;
                        catalogueFollowName[0] = '\0';
                        stringAppend(catalogueFollowName, PROPERTY_NAME_LIMIT,
                                     catalogueEntryName);
                    }
                }
                if ((stringContainsIgnoringCase(catalogueEntryName, "body") ||
                     catalogueEntryName[0] == '\0') &&
                    catalogueKeysShown < 6U)
                {
                    char keyMessage[512];
                    Unsigned32 at;

                    catalogueKeysShown++;
                    keyMessage[0] = '\0';
                    stringAppend(keyMessage, sizeof(keyMessage), "engine:   ");
                    stringAppend(keyMessage, sizeof(keyMessage),
                                 (catalogueEntryName[0] != '\0') ? catalogueEntryName
                                                                 : "(unnamed)");
                    stringAppend(keyMessage, sizeof(keyMessage), " keys —");
                    for (at = 0U; at < list.storedKeyCount; at++)
                    {
                        const ResourceKeyEntry *each = &list.keys[at];

                        stringAppend(keyMessage, sizeof(keyMessage), " ");
                        appendCount(keyMessage, sizeof(keyMessage), at);
                        stringAppend(keyMessage, sizeof(keyMessage), ":");
                        {
                            const char *typeName = resourceTypeGetName(each->typeIdentifier);

                            if (typeName != NULL_POINTER)
                            {
                                stringAppend(keyMessage, sizeof(keyMessage), typeName);
                            }
                            else
                            {
                                appendHexadecimal(keyMessage, sizeof(keyMessage),
                                                  each->typeIdentifier);
                            }
                        }
                        stringAppend(keyMessage, sizeof(keyMessage),
                                     (resourceIndexFind(&simIndex, each->typeIdentifier,
                                                        each->instanceIdentifier,
                                                        each->instanceIdentifierHigh) !=
                                      NULL_POINTER)
                                         ? "(here)"
                                         : "(elsewhere)");
                        stringAppend(keyMessage, sizeof(keyMessage), ";");
                    }
                    platformLogMessage(keyMessage);
                }
                if (catalogueEntryOutfit == (Unsigned32)CATALOGUE_OUTFIT_FACE &&
                    catalogueEntryName[0] != '\0' &&
                    catalogueFaceShown < (Unsigned32)CATALOGUE_FACE_LIMIT)
                {
                    char *face = catalogueFaceDumps[catalogueFaceShown];
                    const char *keyTypeName = resourceTypeGetName(key->typeIdentifier);

                    catalogueFaceShown++;
                    face[0] = '\0';
                    stringAppend(face, 384UL, "engine:   face slot — ");
                    stringAppend(face, 384UL, (catalogueEntryName[0] != '\0')
                                                         ? catalogueEntryName
                                                         : "(unnamed)");
                    stringAppend(face, 384UL, ", key ");
                    appendCount(face, 384UL, catalogueShapeIndex);
                    stringAppend(face, 384UL, " of ");
                    appendCount(face, 384UL, list.storedKeyCount);
                    stringAppend(face, 384UL, " is a ");
                    if (keyTypeName != NULL_POINTER)
                    {
                        stringAppend(face, 384UL, keyTypeName);
                    }
                    else
                    {
                        appendHexadecimal(face, 384UL, key->typeIdentifier);
                    }
                    stringAppend(face, 384UL,
                                 (shape != NULL_POINTER) ? ", found on this disc"
                                                         : ", which the index does not hold");
                    platformLogMessage(face);
                }

                if (catalogueEntryName[0] != '\0' &&
                    key->typeIdentifier != (Unsigned32)PACKAGE_TYPE_SHPE &&
                    catalogueNamedMeshlessShown < 6U)
                {
                    char named[512];
                    Unsigned32 at;

                    catalogueNamedMeshlessShown++;
                    named[0] = '\0';
                    stringAppend(named, sizeof(named), "engine:   named but meshless — ");
                    stringAppend(named, sizeof(named), catalogueEntryName);
                    stringAppend(named, sizeof(named), ", key ");
                    appendCount(named, sizeof(named), catalogueShapeIndex);
                    stringAppend(named, sizeof(named), " of");
                    for (at = 0U; at < list.storedKeyCount; at++)
                    {
                        const char *typeName = resourceTypeGetName(list.keys[at].typeIdentifier);

                        stringAppend(named, sizeof(named), " ");
                        appendCount(named, sizeof(named), at);
                        stringAppend(named, sizeof(named), ":");
                        if (typeName != NULL_POINTER)
                        {
                            stringAppend(named, sizeof(named), typeName);
                        }
                        else
                        {
                            appendHexadecimal(named, sizeof(named),
                                              list.keys[at].typeIdentifier);
                        }
                        stringAppend(named, sizeof(named), ";");
                    }
                    platformLogMessage(named);
                }

                if (catalogueShown < 6U)
                {
                    char message[384];

                    catalogueShown++;
                    message[0] = '\0';
                    stringAppend(message, sizeof(message), "engine:   ");
                    stringAppend(message, sizeof(message), (catalogueEntryName[0] != '\0')
                                                               ? catalogueEntryName
                                                               : "(unnamed)");
                    stringAppend(message, sizeof(message), " — key ");
                    appendCount(message, sizeof(message), catalogueShapeIndex);
                    stringAppend(message, sizeof(message), " of ");
                    appendCount(message, sizeof(message), list.storedKeyCount);
                    stringAppend(message, sizeof(message), " in a version ");
                    appendCount(message, sizeof(message), list.version);
                    stringAppend(message, sizeof(message), " list is a ");
                    {
                        const char *typeName = resourceTypeGetName(key->typeIdentifier);

                        if (typeName != NULL_POINTER)
                        {
                            stringAppend(message, sizeof(message), typeName);
                            stringAppend(message, sizeof(message), " ");
                        }
                        appendHexadecimal(message, sizeof(message), key->typeIdentifier);
                    }
                    stringAppend(message, sizeof(message),
                                 (shape != NULL_POINTER) ? ", which is on this disc"
                                                         : ", which the index does not hold");
                    platformLogMessage(message);
                }
            }
        }
    }
    memoryArenaRewindToMarker(globalArena, marker);
    return SIM_ASSEMBLY_PENDING;
}

static SimAssembly stepTheFollow(MemorySize marker)
{
    Unsigned8 *bytes;
    MemorySize size;
    const ResourceIndexEntry *entry = catalogueFollowEntry;

    if (entry == NULL_POINTER)
    {
        catalogueFollow = CATALOGUE_FOLLOW_IDLE;
        return SIM_ASSEMBLY_PENDING;
    }
    if (!readIndexedResource(entry, &bytes, &size))
    {
        memoryArenaRewindToMarker(globalArena, marker);
        return SIM_ASSEMBLY_PENDING;
    }
    catalogueFollowEntry = NULL_POINTER;

    if (bytes == NULL_POINTER)
    {
        catalogueFollow = CATALOGUE_FOLLOW_IDLE;
        memoryArenaRewindToMarker(globalArena, marker);
        return SIM_ASSEMBLY_PENDING;
    }

    if (catalogueFollow == CATALOGUE_FOLLOW_SHAPE)
    {
        ShapeDescription shape;

        catalogueFollow = CATALOGUE_FOLLOW_IDLE;
        if (scenegraphReadShape(&shape, bytes, size) == SCENEGRAPH_READ_OK)
        {
            Unsigned32 which;
            char shapeMessage[512];

            shapeMessage[0] = '\0';
            stringAppend(shapeMessage, sizeof(shapeMessage), "engine:   ");
            stringAppend(shapeMessage, sizeof(shapeMessage), catalogueFollowName);
            stringAppend(shapeMessage, sizeof(shapeMessage), " shape names ");
            appendCount(shapeMessage, sizeof(shapeMessage), shape.meshCount);
            stringAppend(shapeMessage, sizeof(shapeMessage), " node(s) —");
            for (which = 0U; which < shape.storedMeshCount; which++)
            {
                stringAppend(shapeMessage, sizeof(shapeMessage), " ");
                stringAppend(shapeMessage, sizeof(shapeMessage), shape.meshNames[which]);
                stringAppend(shapeMessage, sizeof(shapeMessage), ";");
            }
            platformLogMessage(shapeMessage);

            for (which = 0U; which < shape.storedMeshCount && catalogueFollowEntry == NULL_POINTER;
                 which++)
            {
                if (shape.meshNames[which][0] == '\0')
                {
                    continue;
                }
                catalogueFollowEntry = resourceIndexFindNamed(
                    &simIndex, (Unsigned32)PACKAGE_TYPE_GMND, shape.meshNames[which]);
            }
            if (catalogueFollowEntry != NULL_POINTER)
            {
                catalogueFollow = CATALOGUE_FOLLOW_NODE;
            }
        }
    }
    else if (catalogueFollow == CATALOGUE_FOLLOW_NODE)
    {
        GeometryNodeDescription node;

        catalogueFollow = CATALOGUE_FOLLOW_IDLE;
        if (scenegraphReadGeometryNode(&node, bytes, size) == SCENEGRAPH_READ_OK && node.hasGeometry)
        {
            catalogueFollowEntry = resourceIndexFind(&simIndex, (Unsigned32)PACKAGE_TYPE_GMDC,
                                                     node.geometryKey.instanceIdentifier,
                                                     node.geometryKey.instanceIdentifierHigh);
            if (catalogueFollowEntry != NULL_POINTER)
            {
                catalogueFollow = CATALOGUE_FOLLOW_CONTAINER;
            }
        }
    }
    else
    {
        static GeometryMesh worn;
        char message[512];

        catalogueFollow = CATALOGUE_FOLLOW_IDLE;
        if (geometryReaderOpen(&worn, bytes, size, globalArena) == GEOMETRY_READ_OK)
        {
            Unsigned32 touched = 0U;
            Unsigned32 vertex;
            Unsigned32 which;

            for (vertex = 0U; vertex < worn.vertexCount && worn.morphSlotCount > 0U; vertex++)
            {
                Unsigned32 slot;

                for (slot = 0U; slot < worn.morphSlotCount; slot++)
                {
                    if (worn.morphSlotChannels[(MemorySize)vertex * worn.morphSlotCount + slot] !=
                        0U)
                    {
                        touched++;
                        break;
                    }
                }
            }

            {
                Real32 furthest = 0.0f;
                MemorySize slots = (MemorySize)worn.vertexCount * worn.morphSlotCount;
                MemorySize at;

                for (at = 0UL; at < slots && worn.morphSlotDeltas != NULL_POINTER; at++)
                {
                    Real32 lengthSquared =
                        (worn.morphSlotDeltas[at * 3UL] * worn.morphSlotDeltas[at * 3UL]) +
                        (worn.morphSlotDeltas[at * 3UL + 1UL] * worn.morphSlotDeltas[at * 3UL + 1UL]) +
                        (worn.morphSlotDeltas[at * 3UL + 2UL] * worn.morphSlotDeltas[at * 3UL + 2UL]);

                    if (lengthSquared > furthest)
                    {
                        furthest = lengthSquared;
                    }
                }
                message[0] = '\0';
                stringAppend(message, sizeof(message), "engine:     ");
                appendCount(message, sizeof(message), worn.morphSlotCount);
                stringAppend(message, sizeof(message), " delta set(s), furthest displacement ");
                appendThousandths(message, sizeof(message), mathSquareRoot(furthest));
                platformLogMessage(message);
            }

            message[0] = '\0';
            stringAppend(message, sizeof(message), "engine:   ");
            stringAppend(message, sizeof(message), catalogueFollowName);
            stringAppend(message, sizeof(message), " — ");
            appendCount(message, sizeof(message), worn.vertexCount);
            stringAppend(message, sizeof(message), " vertices, ");
            appendCount(message, sizeof(message), worn.morphTargetCount);
            stringAppend(message, sizeof(message), " channel(s), a map over ");
            appendCount(message, sizeof(message), worn.morphMappedVertexCount);
            stringAppend(message, sizeof(message), " reaching ");
            appendCount(message, sizeof(message), touched);
            platformLogMessage(message);

            message[0] = '\0';
            stringAppend(message, sizeof(message), "engine:     met and not used —");
            for (which = 0U; which < worn.unusedElementCount; which++)
            {
                const char *elementName = geometryElementGetName(worn.unusedElements[which]);

                stringAppend(message, sizeof(message), " ");
                stringAppend(message, sizeof(message),
                             (elementName != NULL_POINTER) ? elementName : "unnamed");
                stringAppend(message, sizeof(message), " as format ");
                appendCount(message, sizeof(message), worn.unusedElementFormats[which]);
                stringAppend(message, sizeof(message), ";");
            }
            platformLogMessage(message);

            message[0] = '\0';
            stringAppend(message, sizeof(message), "engine:     channels —");
            for (which = 0U; which < worn.morphTargetCount && which < 8U; which++)
            {
                stringAppend(message, sizeof(message), " ");
                stringAppend(message, sizeof(message), worn.morphTargets[which].groupName);
                stringAppend(message, sizeof(message), "/");
                stringAppend(message, sizeof(message), worn.morphTargets[which].channelName);
                stringAppend(message, sizeof(message), ";");
            }
            platformLogMessage(message);
        }
    }
    memoryArenaRewindToMarker(globalArena, marker);
    return SIM_ASSEMBLY_PENDING;
}

static void buildOverridePropertyName(char *destination, MemorySize capacity, Unsigned32 which,
                                      const char *wanted)
{
    char digits[16];

    destination[0] = '\0';
    stringAppend(destination, capacity, "override");
    stringWriteUnsigned(digits, sizeof(digits), which);
    stringAppend(destination, capacity, digits);
    stringAppend(destination, capacity, wanted);
}

static void rememberOverrides(const PropertySet *set)
{
    const Property *declared = propertySetFind(set, "numoverrides");
    Unsigned32 total = 0U;
    Unsigned32 which;

    catalogueOverrideCount = 0U;
    if (declared != NULL_POINTER && declared->kind == PROPERTY_VALUE_INTEGER)
    {
        total = declared->integerValue;
    }
    for (which = 0U; which < total && catalogueOverrideCount < (Unsigned32)CATALOGUE_OVERRIDE_LIMIT;
         which++)
    {
        char wanted[PROPERTY_NAME_LIMIT];
        const Property *keyIndex;
        const char *subset;

        buildOverridePropertyName(wanted, sizeof(wanted), which, "resourcekeyidx");
        keyIndex = propertySetFind(set, wanted);
        buildOverridePropertyName(wanted, sizeof(wanted), which, "subset");
        subset = propertySetGetString(set, wanted, "");
        if (keyIndex == NULL_POINTER || keyIndex->kind != PROPERTY_VALUE_INTEGER ||
            subset[0] == '\0')
        {
            continue;
        }
        catalogueOverrideSubsets[catalogueOverrideCount][0] = '\0';
        stringAppend(catalogueOverrideSubsets[catalogueOverrideCount], PROPERTY_NAME_LIMIT, subset);
        catalogueOverrideKeyIndex[catalogueOverrideCount] = keyIndex->integerValue;
        catalogueOverrideCount++;
    }
    if (total > (Unsigned32)CATALOGUE_OVERRIDE_LIMIT)
    {
        catalogueOverridesBeyondRoom++;
    }
}

static SimAssembly stepTheCatalogue(MemorySize marker)
{
    const ResourceIndexEntry *entry = NULL_POINTER;
    Unsigned8 *bytes;
    MemorySize size;

    if (catalogueFollow != CATALOGUE_FOLLOW_IDLE)
    {
        return stepTheFollow(marker);
    }
    if (catalogueWantsKeyList == BOOLEAN_TRUE)
    {
        return stepTheSidecar(marker);
    }

    if (catalogueStride == 0U)
    {
        char message[256];
        Unsigned32 which;

        catalogueTotalEntries = 0U;
        for (which = 0U; which < simIndex.count; which++)
        {
            if (simIndex.entries[which].typeIdentifier == (Unsigned32)PACKAGE_TYPE_SKIN_ENTRY)
            {
                catalogueTotalEntries++;
            }
        }
        catalogueStride = (catalogueTotalEntries > (Unsigned32)CATALOGUE_SAMPLE_LIMIT)
                              ? (catalogueTotalEntries / (Unsigned32)CATALOGUE_SAMPLE_LIMIT)
                              : 1U;

        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: ");
        appendCount(message, sizeof(message), catalogueTotalEntries);
        stringAppend(message, sizeof(message), " catalogue entr(ies) on this disc, taking every ");
        appendCount(message, sizeof(message), catalogueStride);
        stringAppend(message, sizeof(message), " of them so the sample spans the lot");
        platformLogMessage(message);
    }

    while (catalogueCursor < simIndex.count)
    {
        if (simIndex.entries[catalogueCursor].typeIdentifier !=
            (Unsigned32)PACKAGE_TYPE_SKIN_ENTRY)
        {
            catalogueCursor++;
            continue;
        }
        if ((catalogueSeen % catalogueStride) == 0U)
        {
            break;
        }
        catalogueSeen++;
        catalogueCursor++;
    }
    if (catalogueCursor >= simIndex.count || catalogueRead >= CATALOGUE_SAMPLE_LIMIT)
    {
        reportCatalogue();
        reportWardrobe();
        fillTheClothingPage();
        if (wardrobeGetChosenCount(&simWardrobe) > 0U)
        {
            simWardrobePart = 0U;
            simWardrobeStage = WARDROBE_STAGE_SHAPE;
            simWardrobeEntry = NULL_POINTER;
            simWardrobeDressed = 0U;
            simHop = SIM_HOP_WARDROBE;
            return SIM_ASSEMBLY_PENDING;
        }
        simHop = SIM_HOP_FINISHED;
        return SIM_ASSEMBLY_DONE;
    }

    entry = &simIndex.entries[catalogueCursor];
    if (!readIndexedResource(entry, &bytes, &size))
    {
        memoryArenaRewindToMarker(globalArena, marker);
        return SIM_ASSEMBLY_PENDING;
    }
    catalogueCursor++;
    catalogueSeen++;
    if (bytes != NULL_POINTER)
    {
        static Property properties[48];
        PropertySet set;

        set.properties = properties;
        set.propertyCapacity = 48U;
        if (propertySetRead(&set, bytes, size) == PROPERTY_SET_OK)
        {
            catalogueRead++;
            if (catalogueRead <= 2U)
            {
                char message[512];
                Unsigned32 which;

                message[0] = '\0';
                stringAppend(message, sizeof(message), "engine:   entry of ");
                appendCount(message, sizeof(message), set.propertyCount);
                stringAppend(message, sizeof(message), " propert(ies) —");
                for (which = 0U; which < set.storedPropertyCount; which++)
                {
                    stringAppend(message, sizeof(message), " ");
                    stringAppend(message, sizeof(message), properties[which].name);
                    if (properties[which].kind == PROPERTY_VALUE_STRING)
                    {
                        stringAppend(message, sizeof(message), "=");
                        stringAppend(message, sizeof(message), properties[which].stringValue);
                    }
                    stringAppend(message, sizeof(message), ";");
                }
                platformLogMessage(message);
            }
            if (propertySetFind(&set, "shape") != NULL_POINTER ||
                propertySetFind(&set, "shapekeyidx") != NULL_POINTER)
            {
                catalogueWithShape++;
            }
            rememberCatalogueKind(propertySetGetString(&set, "type", ""),
                                  propertySetGetString(&set, "name", ""));

            {
                const Property *category = propertySetFind(&set, "category");
                const Property *outfit = propertySetFind(&set, "outfit");
                const char *entryName = propertySetGetString(&set, "name", "");

                catalogueEntryCategorySlot = (Unsigned32)CATALOGUE_CATEGORY_LIMIT;
                catalogueEntryOutfitSlot = (Unsigned32)CATALOGUE_CATEGORY_LIMIT;
                catalogueEntryOutfit = 0U;
                if (category != NULL_POINTER && category->kind == PROPERTY_VALUE_INTEGER)
                {
                    catalogueEntryCategorySlot =
                        rememberSlot(&catalogueByCategory, category->integerValue, entryName);
                }
                if (outfit != NULL_POINTER && outfit->kind == PROPERTY_VALUE_INTEGER)
                {
                    catalogueEntryOutfitSlot =
                        rememberSlot(&catalogueByOutfit, outfit->integerValue, entryName);
                    catalogueEntryOutfit = outfit->integerValue;
                }

                if (category != NULL_POINTER && category->kind == PROPERTY_VALUE_INTEGER &&
                    category->integerValue == 0U &&
                    catalogueUncategorisedShown < (Unsigned32)CATALOGUE_DUMP_LIMIT)
                {
                    char *dump = catalogueUncategorisedDumps[catalogueUncategorisedShown];
                    Unsigned32 which;

                    catalogueUncategorisedShown++;
                    dump[0] = '\0';
                    stringAppend(dump, 512UL, "engine:   uncategorised entry —");
                    for (which = 0U; which < set.storedPropertyCount; which++)
                    {
                        stringAppend(dump, 512UL, " ");
                        stringAppend(dump, 512UL, properties[which].name);
                        stringAppend(dump, 512UL, "=");
                        if (properties[which].kind == PROPERTY_VALUE_STRING)
                        {
                            stringAppend(dump, 512UL, properties[which].stringValue);
                        }
                        else
                        {
                            appendHexadecimal(dump, 512UL, properties[which].integerValue);
                        }
                        stringAppend(dump, 512UL, ";");
                    }
                    platformLogMessage(dump);
                }

                if (entryName[0] != '\0' &&
                    catalogueNamedShown < (Unsigned32)CATALOGUE_DUMP_LIMIT)
                {
                    char *dump = catalogueNamedDumps[catalogueNamedShown];
                    Unsigned32 which;

                    catalogueNamedShown++;
                    dump[0] = '\0';
                    stringAppend(dump, 512UL, "engine:   named entry — ");
                    appendCount(dump, 512UL, set.propertyCount);
                    stringAppend(dump, 512UL, " propert(ies):");
                    for (which = 0U; which < set.storedPropertyCount; which++)
                    {
                        stringAppend(dump, 512UL, " ");
                        stringAppend(dump, 512UL, properties[which].name);
                        stringAppend(dump, 512UL, "=");
                        if (properties[which].kind == PROPERTY_VALUE_STRING)
                        {
                            stringAppend(dump, 512UL, properties[which].stringValue);
                        }
                        else
                        {
                            appendHexadecimal(dump, 512UL, properties[which].integerValue);
                        }
                        stringAppend(dump, 512UL, ";");
                    }
                    platformLogMessage(dump);
                }
            }

            if (catalogueRead % 150U == 0U)
            {
                reportCatalogueSlots(BOOLEAN_FALSE);
            }

            {
                const Property *shapeIndex = propertySetFind(&set, "shapekeyidx");

                if (shapeIndex != NULL_POINTER && shapeIndex->kind == PROPERTY_VALUE_INTEGER)
                {
                    catalogueShapeIndex = shapeIndex->integerValue;
                    catalogueEntryGroup = entry->groupIdentifier;
                    catalogueEntryInstance = entry->instanceIdentifier;
                    catalogueEntryInstanceHigh = entry->instanceIdentifierHigh;
                    catalogueEntryName[0] = '\0';
                    stringAppend(catalogueEntryName, PROPERTY_NAME_LIMIT,
                                 propertySetGetString(&set, "name", ""));
                    rememberOverrides(&set);
                    catalogueWantsKeyList = BOOLEAN_TRUE;
                }
            }
        }
        else
        {
            catalogueNotBinary++;
        }
    }
    memoryArenaRewindToMarker(globalArena, marker);
    return SIM_ASSEMBLY_PENDING;
}

static SimAssembly stepTheWardrobe(MemorySize marker)
{
    Unsigned8 *bytes;
    MemorySize size;
    char message[512];
    Unsigned32 index;

    while (simWardrobePart < (Unsigned32)WARDROBE_PART_COUNT &&
           (!wardrobeIsWorn(&simWardrobe, simWardrobePart) ||
            simWardrobeShapes[simWardrobePart] == NULL_POINTER))
    {
        simWardrobePart++;
    }

    if (simWardrobePart >= (Unsigned32)WARDROBE_PART_COUNT)
    {
        if (simWardrobeDressed == 0U)
        {
            platformLogMessage("engine: nothing the catalogue chose would load, so the Sim is left "
                               "in what it was assembled in");
            simHop = SIM_HOP_FINISHED;
            return SIM_ASSEMBLY_DONE;
        }
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: dressed ");
        appendCount(message, sizeof(message), simWardrobeDressed);
        stringAppend(message, sizeof(message),
                     " part(s) out of the catalogue — joining and painting the Sim again, because "
                     "the model on screen is the one being replaced");
        platformLogMessage(message);
        simWardrobeWorn = BOOLEAN_TRUE;
        simHopPart = 0U;
        simHop = SIM_HOP_MERGE;
        return SIM_ASSEMBLY_PENDING;
    }

    if (simWardrobeEntry == NULL_POINTER)
    {
        if (simWardrobeStage != WARDROBE_STAGE_SHAPE)
        {
            message[0] = '\0';
            stringAppend(message, sizeof(message), "engine:   ");
            stringAppend(message, sizeof(message),
                         wardrobeGetChosenName(&simWardrobe, simWardrobePart));
            stringAppend(message, sizeof(message),
                         " names a shape this disc holds, but the chain from it to a mesh does not "
                         "close — that part stays as it was");
            platformLogMessage(message);
            simWardrobePart++;
            simWardrobeStage = WARDROBE_STAGE_SHAPE;
            return SIM_ASSEMBLY_PENDING;
        }
        simWardrobeEntry = simWardrobeShapes[simWardrobePart];
    }

    if (!readIndexedResource(simWardrobeEntry, &bytes, &size))
    {
        memoryArenaRewindToMarker(globalArena, marker);
        return SIM_ASSEMBLY_PENDING;
    }
    simWardrobeEntry = NULL_POINTER;
    if (bytes == NULL_POINTER)
    {
        memoryArenaRewindToMarker(globalArena, marker);
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine:   ");
        stringAppend(message, sizeof(message),
                     wardrobeGetChosenName(&simWardrobe, simWardrobePart));
        stringAppend(message, sizeof(message),
                     " named a resource the disc would not give up, so that part stays as it was");
        platformLogMessage(message);
        simWardrobePart++;
        simWardrobeStage = WARDROBE_STAGE_SHAPE;
        return SIM_ASSEMBLY_PENDING;
    }

    if (simWardrobeStage == WARDROBE_STAGE_SHAPE)
    {
        if (scenegraphReadShape(&simWardrobeShape, bytes, size) == SCENEGRAPH_READ_OK)
        {
            for (index = 0U;
                 index < simWardrobeShape.storedMeshCount && simWardrobeEntry == NULL_POINTER;
                 index++)
            {
                if (simWardrobeShape.meshNames[index][0] == '\0')
                {
                    continue;
                }
                simWardrobeEntry = resourceIndexFindNamed(&simIndex, (Unsigned32)PACKAGE_TYPE_GMND,
                                                          simWardrobeShape.meshNames[index]);
            }
        }
        simWardrobeStage = WARDROBE_STAGE_NODE;
        memoryArenaRewindToMarker(globalArena, marker);
        return SIM_ASSEMBLY_PENDING;
    }

    if (simWardrobeStage == WARDROBE_STAGE_NODE)
    {
        GeometryNodeDescription node;

        if (scenegraphReadGeometryNode(&node, bytes, size) == SCENEGRAPH_READ_OK && node.hasGeometry)
        {
            simWardrobeEntry = resourceIndexFind(&simIndex, (Unsigned32)PACKAGE_TYPE_GMDC,
                                                 node.geometryKey.instanceIdentifier,
                                                 node.geometryKey.instanceIdentifierHigh);
        }
        simWardrobeStage = WARDROBE_STAGE_CONTAINER;
        memoryArenaRewindToMarker(globalArena, marker);
        return SIM_ASSEMBLY_PENDING;
    }

    {
        static GeometryMesh dressed;
        Unsigned32 slot = simWardrobePart;

        if (geometryReaderOpen(&dressed, bytes, size, globalArena) != GEOMETRY_READ_OK)
        {
            memoryArenaRewindToMarker(globalArena, marker);
            message[0] = '\0';
            stringAppend(message, sizeof(message), "engine:   ");
            stringAppend(message, sizeof(message),
                         wardrobeGetChosenName(&simWardrobe, simWardrobePart));
            stringAppend(message, sizeof(message),
                         " would not read as a mesh, so that part stays as it was");
            platformLogMessage(message);
            simWardrobePart++;
            simWardrobeStage = WARDROBE_STAGE_SHAPE;
            return SIM_ASSEMBLY_PENDING;
        }
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine:   ");
        if (simPartLoaded[slot])
        {
            stringAppend(message, sizeof(message), simDrawnPartNames[simWardrobePart]);
            stringAppend(message, sizeof(message), " gives way to ");
            stringAppend(message, sizeof(message),
                         wardrobeGetChosenName(&simWardrobe, simWardrobePart));
            stringAppend(message, sizeof(message), " — ");
            appendCount(message, sizeof(message), simParts[slot].vertexCount);
            stringAppend(message, sizeof(message), " vertices become ");
        }
        else
        {
            stringAppend(message, sizeof(message),
                         wardrobeGetChosenName(&simWardrobe, simWardrobePart));
            stringAppend(message, sizeof(message), " gives this Sim ");
            stringAppend(message, sizeof(message), simDrawnPartNames[simWardrobePart]);
            stringAppend(message, sizeof(message), " it did not have — ");
        }
        appendCount(message, sizeof(message), dressed.vertexCount);
        stringAppend(message, sizeof(message), " vertices, ");
        appendCount(message, sizeof(message), dressed.skinnedVertexCount);
        stringAppend(message, sizeof(message), " of them weighted");
        platformLogMessage(message);

        simParts[slot] = dressed;
        simPartLoaded[slot] = BOOLEAN_TRUE;
        bindPartMaterials(slot, &simWardrobeShape);
        for (index = 0U; index < simWardrobeOverrideCount[simWardrobePart]; index++)
        {
            Unsigned32 primitive;

            for (primitive = 0U; primitive < dressed.storedPrimitiveCount &&
                                 primitive < (Unsigned32)RENDER_PART_LIMIT;
                 primitive++)
            {
                if (dressed.primitives[primitive].name[0] == '\0')
                {
                    continue;
                }
                if (stringEqualsIgnoringCase(simWardrobeOverrideSubsets[simWardrobePart][index],
                                             dressed.primitives[primitive].name))
                {
                    simPartMaterialEntries[slot][primitive] =
                        simWardrobeOverrideMaterials[simWardrobePart][index];
                }
            }
        }
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine:     over ");
        appendCount(message, sizeof(message), dressed.storedPrimitiveCount);
        stringAppend(message, sizeof(message), " range(s), wearing —");
        for (index = 0U;
             index < dressed.storedPrimitiveCount && index < (Unsigned32)RENDER_PART_LIMIT; index++)
        {
            stringAppend(message, sizeof(message), " ");
            stringAppend(message, sizeof(message), (simPartMaterials[slot][index][0] != '\0')
                                                       ? simPartMaterials[slot][index]
                                                       : "(nothing it names)");
            stringAppend(message, sizeof(message), ";");
        }
        platformLogMessage(message);
        reportMorphTargets(&simParts[slot]);
        simWardrobeDressed++;
    }
    simWardrobePart++;
    simWardrobeStage = WARDROBE_STAGE_SHAPE;
    return SIM_ASSEMBLY_PENDING;
}

static SimAssembly stepThePaint(MemorySize marker)
{
    char wanted[RESOURCE_NAME_LIMIT];
    Unsigned8 *bytes;
    MemorySize size;

    if (simHopPart >= simRangeCount)
    {
        if (simWardrobeWorn)
        {
            simHop = SIM_HOP_FINISHED;
            return SIM_ASSEMBLY_DONE;
        }
        wardrobeBegin(&simWardrobe, simArchetype, simWardrobeWanted, simSkinTone);
        {
            Unsigned32 part;

            for (part = 0U; part < (Unsigned32)WARDROBE_PART_COUNT; part++)
            {
                wardrobeWant(&simWardrobe, part, simWardrobeWantedPart[part]);
            }
        }
        simHop = SIM_HOP_CATALOGUE;
        catalogueCursor = 0U;
        catalogueStride = 0U;
        catalogueSeen = 0U;
        catalogueRead = 0U;
        catalogueKindCount = 0U;
        catalogueByCategory.count = 0U;
        catalogueByCategory.beyondRoom = 0U;
        catalogueByOutfit.count = 0U;
        catalogueByOutfit.beyondRoom = 0U;
        catalogueUncategorisedShown = 0U;
        catalogueNamedShown = 0U;
        catalogueFaceShown = 0U;
        return SIM_ASSEMBLY_PENDING;
    }

    if (simHop == SIM_HOP_MATERIAL)
    {
        if (simRangeMaterials[simHopPart][0] == '\0' &&
            simRangeMaterialEntries[simHopPart] == NULL_POINTER)
        {
            simHopPart++;
            return SIM_ASSEMBLY_PENDING;
        }
        if (simHopEntry == NULL_POINTER)
        {
            simHopWearsItsOwn = BOOLEAN_FALSE;
            simHopEntry = simRangeMaterialEntries[simHopPart];
            if (simHopEntry != NULL_POINTER)
            {
                simHopWearsItsOwn = BOOLEAN_TRUE;
            }
            else
            {
                materialBuildResourceName(wanted, sizeof(wanted), simRangeMaterials[simHopPart],
                                          "_txmt");
                simHopEntry = resourceIndexFindNamed(&simIndex, (Unsigned32)PACKAGE_TYPE_TXMT,
                                                     wanted);
                if (simHopEntry == NULL_POINTER)
                {
                    simHopEntry = resourceIndexFindNamed(&simIndex, (Unsigned32)PACKAGE_TYPE_TXMT,
                                                         simRangeMaterials[simHopPart]);
                }
            }
            if (simHopEntry == NULL_POINTER)
            {
                simHopPart++;
                return SIM_ASSEMBLY_PENDING;
            }
        }
        if (!readIndexedResource(simHopEntry, &bytes, &size))
        {
            memoryArenaRewindToMarker(globalArena, marker);
            return SIM_ASSEMBLY_PENDING;
        }
        simHopEntry = NULL_POINTER;
        simHopTextureName[0] = '\0';
        simHopOverrode = BOOLEAN_FALSE;
        {
            MaterialDescription material;

            if (bytes != NULL_POINTER && materialRead(&material, bytes, size) == MATERIAL_READ_OK)
            {
                char message[512];
                Unsigned32 which;

                message[0] = '\0';
                stringAppend(message, sizeof(message), "engine:   material ");
                stringAppend(message, sizeof(message), material.materialName);
                stringAppend(message, sizeof(message), " is a ");
                stringAppend(message, sizeof(message), (material.definitionType[0] != '\0')
                                                           ? material.definitionType
                                                           : "(untyped)");
                stringAppend(message, sizeof(message), " painting with ");
                stringAppend(message, sizeof(message), (material.baseTextureName[0] != '\0')
                                                           ? material.baseTextureName
                                                           : "no base texture");
                stringAppend(message, sizeof(message), ", and names ");
                appendCount(message, sizeof(message), material.textureCount);
                stringAppend(message, sizeof(message), " texture(s) —");
                for (which = 0U; which < material.storedTextureCount; which++)
                {
                    stringAppend(message, sizeof(message), " ");
                    stringAppend(message, sizeof(message), material.textureNames[which]);
                    stringAppend(message, sizeof(message), ";");
                }
                platformLogMessage(message);

                if (material.baseTextureName[0] != '\0')
                {
                    stringAppend(simHopTextureName, sizeof(simHopTextureName),
                                 material.baseTextureName);
                }
            }
        }
        memoryArenaRewindToMarker(globalArena, marker);
        if (simHopTextureName[0] == '\0')
        {
            simHopPart++;
            return SIM_ASSEMBLY_PENDING;
        }
        if (!simHopWearsItsOwn && simPartTextureStems[simRangeOfPart[simHopPart]][0] != '\0' &&
            simSkinTone[0] != '\0')
        {
            char preferred[RESOURCE_NAME_LIMIT];

            preferred[0] = '\0';
            stringAppend(preferred, sizeof(preferred),
                         simPartTextureStems[simRangeOfPart[simHopPart]]);
            stringAppend(preferred, sizeof(preferred), "-");
            stringAppend(preferred, sizeof(preferred), simSkinTone);
            materialBuildResourceName(wanted, sizeof(wanted), preferred, "_txtr");
            if (resourceIndexFindNamed(&simIndex, (Unsigned32)PACKAGE_TYPE_TXTR, wanted) !=
                NULL_POINTER)
            {
                simHopTextureName[0] = '\0';
                stringAppend(simHopTextureName, sizeof(simHopTextureName), preferred);
                simHopOverrode = BOOLEAN_TRUE;
            }
        }
        materialBuildResourceName(wanted, sizeof(wanted), simHopTextureName, "_txtr");
        simHopEntry = resourceIndexFindNamed(&simIndex, (Unsigned32)PACKAGE_TYPE_TXTR, wanted);
        if (simHopEntry == NULL_POINTER)
        {
            simHopEntry = resourceIndexFindNamed(&simIndex, (Unsigned32)PACKAGE_TYPE_TXTR,
                                                 simHopTextureName);
        }
        if (simHopEntry == NULL_POINTER)
        {
            simHopPart++;
            return SIM_ASSEMBLY_PENDING;
        }
        simHop = SIM_HOP_TEXTURE;
        return SIM_ASSEMBLY_PENDING;
    }

    if (simHop == SIM_HOP_TEXTURE)
    {
        if (!readIndexedResource(simHopEntry, &bytes, &size))
        {
            memoryArenaRewindToMarker(globalArena, marker);
            return SIM_ASSEMBLY_PENDING;
        }
        simHopEntry = NULL_POINTER;
        if (bytes == NULL_POINTER ||
            textureReaderOpen(&simHopTexture, bytes, size) != TEXTURE_READ_OK)
        {
            simHopPart++;
            simHop = SIM_HOP_MATERIAL;
            return SIM_ASSEMBLY_PENDING;
        }
        if (simHopTexture.largestIsElsewhere && simHopTexture.lifoName[0] != '\0')
        {
            simHopEntry = resourceIndexFindNamed(&simIndex, (Unsigned32)PACKAGE_TYPE_LIFO,
                                                 simHopTexture.lifoName);
            if (simHopEntry == NULL_POINTER)
            {
                materialBuildResourceName(wanted, sizeof(wanted), simHopTexture.lifoName, "_lifo");
                simHopEntry = resourceIndexFindNamed(&simIndex, (Unsigned32)PACKAGE_TYPE_LIFO,
                                                     wanted);
            }
            if (simHopEntry != NULL_POINTER)
            {
                simHop = SIM_HOP_TOP_LEVEL;
                return SIM_ASSEMBLY_PENDING;
            }
        }
        return finishThePart(marker);
    }

    if (!readIndexedResource(simHopEntry, &bytes, &size))
    {
        memoryArenaRewindToMarker(globalArena, marker);
        return SIM_ASSEMBLY_PENDING;
    }
    simHopEntry = NULL_POINTER;
    if (bytes != NULL_POINTER)
    {
        TextureLevel largest;

        if (textureReaderOpenLevel(&largest, bytes, size) == TEXTURE_READ_OK &&
            largest.bytes != NULL_POINTER && largest.width > simHopTexture.levelWidth &&
            largest.byteCount == textureFormatGetLevelBytes(simHopTexture.format, largest.width,
                                                            largest.height))
        {
            simHopTexture.bytes = largest.bytes;
            simHopTexture.byteCount = largest.byteCount;
            simHopTexture.levelWidth = largest.width;
            simHopTexture.levelHeight = largest.height;
        }
    }
    return finishThePart(marker);
}

static SimAssembly stepTheSim(void)
{
    static GeometryMesh whole;
    MemorySize marker = memoryArenaGetMarker(globalArena);
    Unsigned8 *bytes;
    MemorySize size;
    char message[512];
    Unsigned32 index;

    switch (simHop)
    {
    case SIM_HOP_TREE:
        if (simHopPart >= SIM_BASE_PART_COUNT)
        {
            simHop = SIM_HOP_MERGE;
            return SIM_ASSEMBLY_PENDING;
        }
        simHopEntry = resourceIndexFindNamed(&simIndex, (Unsigned32)PACKAGE_TYPE_CRES,
                                             simDrawnPartNames[simHopPart]);
        if (simHopEntry == NULL_POINTER)
        {
            simHopPart++;
            return SIM_ASSEMBLY_PENDING;
        }
        if (!readIndexedResource(simHopEntry, &bytes, &size))
        {
            memoryArenaRewindToMarker(globalArena, marker);
            return SIM_ASSEMBLY_PENDING;
        }
        simHopEntry = NULL_POINTER;
        if (bytes != NULL_POINTER &&
            resourceNodeRead(&simHopTree, bytes, size) == RESOURCE_NODE_OK)
        {
            for (index = 0U; index < simHopTree.storedNodeCount; index++)
            {
                if (!simHopTree.nodes[index].hasShape)
                {
                    continue;
                }
                simHopEntry = resourceIndexFind(&simIndex, (Unsigned32)PACKAGE_TYPE_SHPE,
                                                simHopTree.nodes[index].shapeKey.instanceIdentifier,
                                                simHopTree.nodes[index]
                                                    .shapeKey.instanceIdentifierHigh);
                if (simHopEntry != NULL_POINTER)
                {
                    break;
                }
            }
        }
        memoryArenaRewindToMarker(globalArena, marker);
        simHop = (simHopEntry != NULL_POINTER) ? SIM_HOP_SHAPE : SIM_HOP_TREE;
        if (simHopEntry == NULL_POINTER)
        {
            reportSimPart(simHopPart, DISC_MODEL_SHAPE_NOT_IN_PACKAGE);
            simHopPart++;
        }
        return SIM_ASSEMBLY_PENDING;

    case SIM_HOP_SHAPE:
        if (!readIndexedResource(simHopEntry, &bytes, &size))
        {
            memoryArenaRewindToMarker(globalArena, marker);
            return SIM_ASSEMBLY_PENDING;
        }
        simHopEntry = NULL_POINTER;
        if (bytes != NULL_POINTER &&
            scenegraphReadShape(&simHopShape, bytes, size) == SCENEGRAPH_READ_OK)
        {
            reportWholeShape(simDrawnPartNames[simHopPart], &simHopShape);
            for (index = 0U; index < simHopShape.storedMeshCount && simHopEntry == NULL_POINTER;
                 index++)
            {
                if (simHopShape.meshNames[index][0] == '\0')
                {
                    continue;
                }
                simHopEntry = resourceIndexFindNamed(&simIndex, (Unsigned32)PACKAGE_TYPE_GMND,
                                                     simHopShape.meshNames[index]);
            }
        }
        memoryArenaRewindToMarker(globalArena, marker);
        if (simHopEntry == NULL_POINTER)
        {
            reportSimPart(simHopPart, DISC_MODEL_NO_GEOMETRY_NAMED);
            simHopPart++;
            simHop = SIM_HOP_TREE;
            return SIM_ASSEMBLY_PENDING;
        }
        simHop = SIM_HOP_NODE;
        return SIM_ASSEMBLY_PENDING;

    case SIM_HOP_NODE:
        if (!readIndexedResource(simHopEntry, &bytes, &size))
        {
            memoryArenaRewindToMarker(globalArena, marker);
            return SIM_ASSEMBLY_PENDING;
        }
        simHopEntry = NULL_POINTER;
        {
            GeometryNodeDescription node;

            if (bytes != NULL_POINTER &&
                scenegraphReadGeometryNode(&node, bytes, size) == SCENEGRAPH_READ_OK &&
                node.hasGeometry)
            {
                simHopEntry = resourceIndexFind(&simIndex, (Unsigned32)PACKAGE_TYPE_GMDC,
                                                node.geometryKey.instanceIdentifier,
                                                node.geometryKey.instanceIdentifierHigh);
            }
        }
        memoryArenaRewindToMarker(globalArena, marker);
        if (simHopEntry == NULL_POINTER)
        {
            reportSimPart(simHopPart, DISC_MODEL_GEOMETRY_UNREADABLE);
            simHopPart++;
            simHop = SIM_HOP_TREE;
            return SIM_ASSEMBLY_PENDING;
        }
        simHop = SIM_HOP_CONTAINER;
        return SIM_ASSEMBLY_PENDING;

    case SIM_HOP_CONTAINER:
        if (!readIndexedResource(simHopEntry, &bytes, &size))
        {
            memoryArenaRewindToMarker(globalArena, marker);
            return SIM_ASSEMBLY_PENDING;
        }
        simHopEntry = NULL_POINTER;
        if (bytes == NULL_POINTER ||
            geometryReaderOpen(&simParts[simHopPart], bytes, size, globalArena) !=
                GEOMETRY_READ_OK)
        {
            reportSimPart(simHopPart, DISC_MODEL_GEOMETRY_UNREADABLE);
            simHopPart++;
            simHop = SIM_HOP_TREE;
            return SIM_ASSEMBLY_PENDING;
        }
        bindPartMaterials(simHopPart, &simHopShape);
        simPartLoaded[simHopPart] = BOOLEAN_TRUE;
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine:   ");
        stringAppend(message, sizeof(message), simDrawnPartNames[simHopPart]);
        stringAppend(message, sizeof(message), " — ");
        appendCount(message, sizeof(message), simParts[simHopPart].vertexCount);
        stringAppend(message, sizeof(message), " vertices, ");
        appendCount(message, sizeof(message), simParts[simHopPart].indexCount / 3U);
        stringAppend(message, sizeof(message), " triangles, ");
        appendCount(message, sizeof(message), simParts[simHopPart].skinnedVertexCount);
        stringAppend(message, sizeof(message), " of them weighted across ");
        appendCount(message, sizeof(message), simParts[simHopPart].storedPrimitiveCount);
        stringAppend(message, sizeof(message), " range(s), wearing ");
        stringAppend(message, sizeof(message), (simPartMaterials[simHopPart][0][0] != '\0')
                                                   ? simPartMaterials[simHopPart][0]
                                                   : "no material it names");
        platformLogMessage(message);
        reportMorphTargets(&simParts[simHopPart]);
        simHopPart++;
        simHop = SIM_HOP_TREE;
        return SIM_ASSEMBLY_PENDING;

    case SIM_HOP_MERGE:
    {
        const GeometryMesh *parts[SIM_PART_COUNT_DRAWN];

        simJoinCount = 0U;
        for (index = 0U; index < (Unsigned32)SIM_PART_COUNT_DRAWN; index++)
        {
            if (!simPartLoaded[index])
            {
                continue;
            }
            if (simWardrobeWorn && (index == (Unsigned32)SIM_PART_BODY ||
                                    index == (Unsigned32)SIM_PART_TOP ||
                                    index == (Unsigned32)SIM_PART_BOTTOM) &&
                !wardrobeIsWorn(&simWardrobe, index))
            {
                continue;
            }
            parts[simJoinCount] = &simParts[index];
            simJoinParts[simJoinCount] = index;
            simJoinCount++;
        }
        if (simJoinCount == 0U)
        {
            return SIM_ASSEMBLY_FAILED;
        }
        if (geometryMeshMerge(&whole, parts, simJoinCount, globalArena) != GEOMETRY_READ_OK)
        {
            platformLogMessage("engine: a Sim's parts would not join into one model");
            return SIM_ASSEMBLY_FAILED;
        }
        message[0] = '\0';
        stringAppend(message, sizeof(message),
                     simWardrobeWorn ? "engine: a dressed Sim — " : "engine: a whole Sim — ");
        appendCount(message, sizeof(message), simJoinCount);
        stringAppend(message, sizeof(message), " part(s) joined into ");
        appendCount(message, sizeof(message), whole.vertexCount);
        stringAppend(message, sizeof(message), " vertices and ");
        appendCount(message, sizeof(message), whole.indexCount / 3U);
        stringAppend(message, sizeof(message), " triangles across ");
        appendCount(message, sizeof(message), whole.storedPrimitiveCount);
        stringAppend(message, sizeof(message), " range(s), ");
        appendCount(message, sizeof(message), whole.skinnedVertexCount);
        stringAppend(message, sizeof(message), " of them weighted");
        platformLogMessage(message);
        reportDeformationReach(&whole);

        {
            Unsigned32 channel;

            simMorphChannels = (whole.morphTargetCount < SIM_MORPH_WEIGHT_LIMIT)
                                   ? whole.morphTargetCount
                                   : SIM_MORPH_WEIGHT_LIMIT;
            for (channel = 0U; channel < SIM_MORPH_WEIGHT_LIMIT; channel++)
            {
                simMorphWeights[channel] = 0.0f;
            }
            simMorphShowing = 0xFFFFFFFFUL;
        }

        simRangeCount = 0U;
        for (index = 0U; index < simJoinCount; index++)
        {
            Unsigned32 part = simJoinParts[index];
            Unsigned32 primitive;

            for (primitive = 0U; primitive < simParts[part].storedPrimitiveCount &&
                                 primitive < (Unsigned32)RENDER_PART_LIMIT &&
                                 simRangeCount < (Unsigned32)RENDER_PART_LIMIT;
                 primitive++)
            {
                simRangeMaterials[simRangeCount][0] = '\0';
                stringAppend(simRangeMaterials[simRangeCount], RESOURCE_NAME_LIMIT,
                             simPartMaterials[part][primitive]);
                simRangeMaterialEntries[simRangeCount] = simPartMaterialEntries[part][primitive];
                simRangeOfPart[simRangeCount] = part;
                simRangeCount++;
            }
        }
        if (whole.storedPrimitiveCount > simRangeCount)
        {
            message[0] = '\0';
            stringAppend(message, sizeof(message), "engine: this Sim has ");
            appendCount(message, sizeof(message), whole.storedPrimitiveCount);
            stringAppend(message, sizeof(message), " range(s) and the backend paints ");
            appendCount(message, sizeof(message), (Unsigned32)RENDER_PART_LIMIT);
            stringAppend(message, sizeof(message),
                         " — the rest are drawn under whatever their neighbour wears");
            platformLogMessage(message);
        }

        discSearch.mesh = whole;
        renderSetMesh(&discSearch.mesh, globalArena);
        poseIsAnimated = BOOLEAN_FALSE;
        simIsAssembled = BOOLEAN_TRUE;
        simHopPart = 0U;
        simHop = SIM_HOP_SKELETON;
        return SIM_ASSEMBLY_PENDING;
    }

    case SIM_HOP_SKELETON:
        if (simHopEntry == NULL_POINTER)
        {
            simHopEntry = resourceIndexFindNamed(&simIndex, (Unsigned32)PACKAGE_TYPE_CRES,
                                                 simPartNames[0]);
        }
        discSearch.modelHasTree = BOOLEAN_FALSE;
        if (simHopEntry != NULL_POINTER)
        {
            if (!readIndexedResource(simHopEntry, &bytes, &size))
            {
                memoryArenaRewindToMarker(globalArena, marker);
                return SIM_ASSEMBLY_PENDING;
            }
            if (bytes != NULL_POINTER &&
                resourceNodeRead(&discSearch.modelTree, bytes, size) == RESOURCE_NODE_OK)
            {
                discSearch.modelHasTree = BOOLEAN_TRUE;
            }
            memoryArenaRewindToMarker(globalArena, marker);
        }
        simHopEntry = NULL_POINTER;
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: ");
        if (discSearch.modelHasTree)
        {
            stringAppend(message, sizeof(message), "hung on ");
            stringAppend(message, sizeof(message), simPartNames[0]);
            stringAppend(message, sizeof(message), " — ");
            appendCount(message, sizeof(message), discSearch.modelTree.storedNodeCount);
            stringAppend(message, sizeof(message),
                         " node(s), which is the skeleton every part is weighted to and every "
                         "animation targets");
            discContentKeepBindPose(&discSearch, globalArena);
        }
        else
        {
            stringAppend(message, sizeof(message),
                         "the skeleton these parts hang on would not read, so they stay in their "
                         "bind pose rather than being posed by somebody else's bones");
        }
        platformLogMessage(message);
        simHopPart = 0U;
        simHop = SIM_HOP_MATERIAL;
        return SIM_ASSEMBLY_PENDING;

    case SIM_HOP_MATERIAL:
    case SIM_HOP_TEXTURE:
    case SIM_HOP_TOP_LEVEL:
        return stepThePaint(marker);

    case SIM_HOP_CATALOGUE:
        return stepTheCatalogue(marker);

    case SIM_HOP_WARDROBE:
        return stepTheWardrobe(marker);

    default:
        break;
    }
    return SIM_ASSEMBLY_DONE;
}

static EngineDiscLoadStatus finishOrSeekSkin(void)
{
    if (discSearch.mesh.boneAssignments == NULL_POINTER && !skinIndexBegun)
    {
        static const Unsigned32 wantedTypes[1] = { (Unsigned32)PACKAGE_TYPE_GMDC };

        skinIndexBegun = BOOLEAN_TRUE;
        if (resourceIndexBegin(&skinIndex, discFileSystem, globalArena, SKIN_INDEX_CAPACITY,
                               wantedTypes, 1U))
        {
            skinIndexBuilding = BOOLEAN_TRUE;
            skinCursor = 0U;
            skinScanned = 0U;
            platformLogMessage("engine: what was drawn has no skeleton — asking the index where the "
                               "disc keeps geometry that does");
            discPhase = DISC_PHASE_SEEK_SKIN;
            return ENGINE_DISC_WORKING;
        }
        platformLogMessage("engine: not enough room to index the disc for a skinned mesh");
    }

    if (discSearch.mesh.boneAssignments != NULL_POINTER && !simIndexBegun)
    {
        static const Unsigned32 wantedTypes[11] = { (Unsigned32)PACKAGE_TYPE_CRES,
                                                   (Unsigned32)PACKAGE_TYPE_SHPE,
                                                   (Unsigned32)PACKAGE_TYPE_GMND,
                                                   (Unsigned32)PACKAGE_TYPE_GMDC,
                                                   (Unsigned32)PACKAGE_TYPE_TXMT,
                                                   (Unsigned32)PACKAGE_TYPE_TXTR,
                                                   (Unsigned32)PACKAGE_TYPE_LIFO,
                                                   (Unsigned32)PACKAGE_TYPE_SKIN_ENTRY,
                                                   (Unsigned32)PACKAGE_TYPE_RESOURCE_KEY_LIST,
                                                   (Unsigned32)PACKAGE_TYPE_JPEG,
                                                   (Unsigned32)PACKAGE_TYPE_CATALOG_INDEX };

        simIndexBegun = BOOLEAN_TRUE;
        if (resourceIndexBegin(&simIndex, discFileSystem, globalArena, SIM_INDEX_CAPACITY,
                               wantedTypes, 11U))
        {
            if (simIndex.wantedTypesRefused > 0U)
            {
                char refusal[128];

                refusal[0] = '\0';
                stringAppend(refusal, sizeof(refusal), "engine: the index would not take ");
                appendCount(refusal, sizeof(refusal), simIndex.wantedTypesRefused);
                stringAppend(refusal, sizeof(refusal),
                             " of the resource type(s) asked for, so lookups of those find "
                             "nothing whatever the disc holds");
                platformLogMessage(refusal);
            }
            simIndexBuilding = BOOLEAN_TRUE;
            simPartCursor = 0U;
            simPartsFound = 0U;
            simDrawnPartsFound = 0U;
            platformLogMessage("engine: asking the index whether this disc carries the parts a "
                               "whole Sim is built from");
            discPhase = DISC_PHASE_SEEK_SIM;
            return ENGINE_DISC_WORKING;
        }
        platformLogMessage("engine: not enough room to index the disc for a Sim's parts");
    }

    if (simIsAssembled && !discSearch.modelHasTree && !animationIndexBegun)
    {
        animationIndexBegun = BOOLEAN_TRUE;
        platformLogMessage("engine: the assembled Sim has no skeleton of its own, so it is left in "
                           "its bind pose rather than posed by another model's bones");
    }

    if (discSearch.mesh.boneAssignments != NULL_POINTER && !animationIndexBegun)
    {
        static const Unsigned32 wantedTypes[1] = { (Unsigned32)PACKAGE_TYPE_ANIM };

        animationIndexBegun = BOOLEAN_TRUE;
        if (!discContentKeepBindPose(&discSearch, globalArena))
        {
            platformLogMessage("engine: no room to keep the mesh's bind pose, so it cannot be "
                               "posed without posing on top of itself — left as it is");
            discPhase = DISC_PHASE_DONE;
            discLoadStatus = ENGINE_DISC_READY;
            return discLoadStatus;
        }
        if (resourceIndexBegin(&animationIndex, discFileSystem, globalArena,
                               ANIMATION_INDEX_CAPACITY, wantedTypes, 1U))
        {
            animationIndexBuilding = BOOLEAN_TRUE;
            animationCursor = 0U;
            animationScanned = 0U;
            platformLogMessage("engine: what was drawn has a skeleton — asking the index for an "
                               "animation to pose it with");
            discPhase = DISC_PHASE_SEEK_ANIMATION;
            return ENGINE_DISC_WORKING;
        }
        platformLogMessage("engine: not enough room to index the disc for an animation");
    }

    discPhase = DISC_PHASE_DONE;
    discLoadStatus = ENGINE_DISC_READY;
    return discLoadStatus;
}

static EngineDiscLoadStatus stepTheChosenAnimation(void)
{
    Unsigned8 *bytes;
    MemorySize size;
    char message[256];

    if (simWardrobeAnimationWanted == NULL_POINTER)
    {
        discPhase = DISC_PHASE_DONE;
        discLoadStatus = ENGINE_DISC_READY;
        return discLoadStatus;
    }
    if (!readIndexedResource(simWardrobeAnimationWanted, &bytes, &size))
    {
        return ENGINE_DISC_WORKING;
    }
    if (animationArenaReady)
    {
        memoryArenaRewindToMarker(&animationArena, 0UL);
    }
    if (bytes != NULL_POINTER &&
        animationReaderOpen(&posedAnimation,
                            bytes, size,
                            animationArenaReady ? &animationArena : globalArena) ==
            ANIMATION_READ_OK)
    {
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: playing ");
        stringAppend(message, sizeof(message), posedAnimation.resourceName);
        stringAppend(message, sizeof(message), " — ");
        appendCount(message, sizeof(message), posedAnimation.durationTicks);
        stringAppend(message, sizeof(message), " tick(s) long");
        platformLogMessage(message);
        animationUsedRestPose = BOOLEAN_FALSE;
        poseTick = 0.0f;
        if (discContentPoseFromAnimation(&discSearch, &posedAnimation, ANIMATION_POSE_TICK,
                                         globalArena))
        {
            poseIsAnimated = BOOLEAN_TRUE;
            renderUpdateMeshVertices(&discSearch.mesh, globalArena);
        }
        else
        {
            platformLogMessage("engine: that one reached none of this Sim's bones, so the pose "
                               "is left as it was");
        }
    }
    else
    {
        platformLogMessage("engine: that animation would not fit the space kept for one, so the "
                           "Sim goes on doing what it was doing");
    }
    simWardrobeAnimationWanted = NULL_POINTER;
    discPhase = DISC_PHASE_DONE;
    discLoadStatus = ENGINE_DISC_READY;
    return discLoadStatus;
}

static EngineDiscLoadStatus beginTheAnimationList(void)
{
    if (animationIndex.count > 0U && menuAnimationCursor == 0U &&
        debugMenuGetCount(&debugMenu, DEBUG_MENU_PAGE_ANIMATION) == 0U)
    {
        char message[256];

        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: reading ");
        appendCount(message, sizeof(message), animationIndex.count);
        stringAppend(message, sizeof(message),
                     " animation(s) for their names, so the menu has something to offer — an "
                     "animation's name is inside the resource and the index holds only a hash");
        platformLogMessage(message);
        discPhase = DISC_PHASE_LIST_ANIMATIONS;
        discLoadStatus = ENGINE_DISC_WORKING;
        return discLoadStatus;
    }
    discPhase = DISC_PHASE_DONE;
    discLoadStatus = ENGINE_DISC_READY;
    return discLoadStatus;
}

static EngineDiscLoadStatus stepTheAnimationList(void)
{
    const ResourceIndexEntry *entry;
    MemorySize marker;
    Unsigned8 *bytes;
    MemorySize size;
    static Animation listed;

    if (menuAnimationCursor >= animationIndex.count ||
        menuAnimationCount >= (Unsigned32)MENU_ANIMATION_CAPACITY)
    {
        char message[256];

        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: the menu can offer ");
        appendCount(message, sizeof(message), menuAnimationCount);
        stringAppend(message, sizeof(message), " animation(s), out of ");
        appendCount(message, sizeof(message), menuAnimationOpened);
        stringAppend(message, sizeof(message), " opened and ");
        appendCount(message, sizeof(message), animationIndex.count);
        stringAppend(message, sizeof(message), " on this disc");
        if (menuAnimationCursor < animationIndex.count)
        {
            stringAppend(message, sizeof(message),
                         " — the list filled before the disc ran out, so there are more");
        }
        platformLogMessage(message);
        discPhase = DISC_PHASE_DONE;
        discLoadStatus = ENGINE_DISC_READY;
        return discLoadStatus;
    }

    entry = &animationIndex.entries[menuAnimationCursor];
    marker = memoryArenaGetMarker(globalArena);
    if (!readIndexedResource(entry, &bytes, &size))
    {
        memoryArenaRewindToMarker(globalArena, marker);
        return ENGINE_DISC_WORKING;
    }
    menuAnimationCursor++;
    menuAnimationOpened++;

    if (bytes != NULL_POINTER &&
        animationReaderOpen(&listed, bytes, size, globalArena) == ANIMATION_READ_OK &&
        !stringContainsIgnoringCase(listed.resourceName, "2o-") &&
        !stringContainsIgnoringCase(listed.resourceName, "2o_") &&
        listed.skeletonTag[0] != '\0' &&
        stringStartsWith(simPartNames[0], listed.skeletonTag))
    {
        if (debugMenuAddRow(&debugMenu, DEBUG_MENU_PAGE_ANIMATION, listed.resourceName) !=
            (Unsigned32)DEBUG_MENU_NONE)
        {
            menuAnimationEntries[menuAnimationCount] = entry;
            if (stringEquals(listed.resourceName, posedAnimation.resourceName))
            {
                debugMenuSetInEffect(&debugMenu, DEBUG_MENU_PAGE_ANIMATION, menuAnimationCount);
            }
            menuAnimationCount++;
        }
    }
    memoryArenaRewindToMarker(globalArena, marker);
    return ENGINE_DISC_WORKING;
}

static EngineDiscLoadStatus seekTheSim(void)
{
    char message[512];

    const ResourceIndexEntry *entry;
    MemorySize marker;
    Unsigned8 *bytes;
    MemorySize size;

    if (simIndexBuilding)
    {
        if (resourceIndexStep(&simIndex) == RESOURCE_INDEX_WORKING)
        {
            return ENGINE_DISC_WORKING;
        }
        simIndexBuilding = BOOLEAN_FALSE;
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: ");
        appendCount(message, sizeof(message), simIndex.countByType[0]);
        stringAppend(message, sizeof(message), " transform tree(s) across ");
        appendCount(message, sizeof(message), simIndex.filesIndexed);
        stringAppend(message, sizeof(message), " package(s) to look among");
        platformLogMessage(message);
        reportArchetypesOnThisDisc();
        settleTheSkeleton();
        return ENGINE_DISC_WORKING;
    }

    if (simPartCursor >= SIM_PART_COUNT)
    {
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: ");
        appendCount(message, sizeof(message), simPartsFound);
        stringAppend(message, sizeof(message), " of ");
        appendCount(message, sizeof(message), (Unsigned32)SIM_PART_COUNT);
        stringAppend(message, sizeof(message),
                     " of a whole Sim's parts are on this disc by name");
        if (!saidWhatTheDiscHas)
        {
            saidWhatTheDiscHas = BOOLEAN_TRUE;
            platformLogMessage(message);
        }
        if (simDrawnPartsFound > 0U)
        {
            SimAssembly assembly = stepTheSim();

            if (assembly == SIM_ASSEMBLY_PENDING)
            {
                return ENGINE_DISC_WORKING;
            }
        }
        discPhase = DISC_PHASE_DONE;
        return finishOrSeekSkin();
    }

    entry = resourceIndexFindNamed(&simIndex, (Unsigned32)PACKAGE_TYPE_CRES,
                                   simPartNames[simPartCursor]);
    if (entry != NULL_POINTER)
    {
        simPartFileIndex = entry->fileIndex;
    }
    if (entry == NULL_POINTER)
    {
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine:   ");
        stringAppend(message, sizeof(message), simPartNames[simPartCursor]);
        stringAppend(message, sizeof(message), " — not on this disc under that name");
        platformLogMessage(message);
        simPartCursor++;
        return ENGINE_DISC_WORKING;
    }

    marker = memoryArenaGetMarker(globalArena);
    if (!readIndexedResource(entry, &bytes, &size))
    {
        memoryArenaRewindToMarker(globalArena, marker);
        return ENGINE_DISC_WORKING;
    }

    message[0] = '\0';
    stringAppend(message, sizeof(message), "engine:   ");
    stringAppend(message, sizeof(message), simPartNames[simPartCursor]);
    if (bytes != NULL_POINTER &&
        resourceNodeRead(&simPartTree, bytes, size) == RESOURCE_NODE_OK)
    {
        const VirtualFileEntry *holder =
            virtualFileSystemGetEntry(discFileSystem, entry->fileIndex);
        Unsigned32 shapes = 0U;
        Unsigned32 bones = 0U;
        Unsigned32 index;

        for (index = 0U; index < simPartTree.storedNodeCount; index++)
        {
            if (simPartTree.nodes[index].hasShape)
            {
                shapes++;
            }
            if (simPartTree.nodes[index].boneIdentifier != 0x7FFFFFFFUL)
            {
                bones++;
            }
        }
        simPartsFound++;
        if (simPartCursor > 0U)
        {
            simDrawnPartsFound++;
        }
        stringAppend(message, sizeof(message), " — ");
        appendCount(message, sizeof(message), simPartTree.storedNodeCount);
        stringAppend(message, sizeof(message), " of ");
        appendCount(message, sizeof(message), simPartTree.nodeCount);
        stringAppend(message, sizeof(message), " node(s) kept, ");
        appendCount(message, sizeof(message), bones);
        stringAppend(message, sizeof(message), " of them bones, naming ");
        appendCount(message, sizeof(message), shapes);
        stringAppend(message, sizeof(message), " shape(s), in ");
        stringAppend(message, sizeof(message),
                     (holder != NULL_POINTER) ? holder->path : "a package it cannot name");
    }
    else
    {
        stringAppend(message, sizeof(message),
                     " — found by name, but its tree would not read");
    }
    platformLogMessage(message);
    memoryArenaRewindToMarker(globalArena, marker);
    simPartCursor++;
    return ENGINE_DISC_WORKING;
}

static EngineDiscLoadStatus seekTheAnimation(void)
{
    char message[512];

    const ResourceIndexEntry *entry;
    MemorySize marker;
    Unsigned8 *bytes;
    MemorySize size;
    AnimationReadResult animationResult;

    if (animationIndexBuilding)
    {
        if (resourceIndexStep(&animationIndex) == RESOURCE_INDEX_WORKING)
        {
            return ENGINE_DISC_WORKING;
        }
        animationIndexBuilding = BOOLEAN_FALSE;
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: ");
        appendCount(message, sizeof(message), animationIndex.countByType[0]);
        stringAppend(message, sizeof(message), " animation(s) across ");
        appendCount(message, sizeof(message), animationIndex.filesIndexed);
        stringAppend(message, sizeof(message), " package(s) to choose from");
        platformLogMessage(message);
        return ENGINE_DISC_WORKING;
    }

    if (!animationTriedNamed)
    {
        entry = resourceIndexFindNamed(&animationIndex, (Unsigned32)PACKAGE_TYPE_ANIM,
                                       ANIMATION_REST_POSE_NAME);
        if (entry == NULL_POINTER)
        {
            platformLogMessage("engine: the rest pose " ANIMATION_REST_POSE_NAME
                               " is not on this disc — falling back to the scan, whose "
                               "result nothing here can check");
            animationTriedNamed = BOOLEAN_TRUE;
            return ENGINE_DISC_WORKING;
        }
        marker = memoryArenaGetMarker(globalArena);
        if (!readIndexedResource(entry, &bytes, &size))
        {
            memoryArenaRewindToMarker(globalArena, marker);
            return ENGINE_DISC_WORKING;
        }
        animationTriedNamed = BOOLEAN_TRUE;
        animationUsedRestPose = BOOLEAN_TRUE;
    }
    else
    {
        if (animationCursor >= animationIndex.count || animationScanned >= ANIMATION_SCAN_LIMIT)
        {
            message[0] = '\0';
            stringAppend(message, sizeof(message), "engine: opened ");
            appendCount(message, sizeof(message), animationScanned);
            stringAppend(message, sizeof(message), " of ");
            appendCount(message, sizeof(message), animationIndex.count);
            stringAppend(message, sizeof(message),
                         " animation(s) and none would pose this mesh");
            platformLogMessage(message);
            return beginTheAnimationList();
        }

        entry = &animationIndex.entries[animationCursor];
        marker = memoryArenaGetMarker(globalArena);
        if (!readIndexedResource(entry, &bytes, &size))
        {
            memoryArenaRewindToMarker(globalArena, marker);
            return ENGINE_DISC_WORKING;
        }
        animationCursor++;
        animationScanned++;
        animationUsedRestPose = BOOLEAN_FALSE;
    }

    animationResult = (bytes != NULL_POINTER)
                          ? animationReaderOpen(&posedAnimation, bytes, size, globalArena)
                          : ANIMATION_READ_TRUNCATED;
    if (animationResult != ANIMATION_READ_OK)
    {
        memoryArenaRewindToMarker(globalArena, marker);
        return ENGINE_DISC_WORKING;
    }

    message[0] = '\0';
    stringAppend(message, sizeof(message), "engine: animation ");
    stringAppend(message, sizeof(message), posedAnimation.resourceName);
    stringAppend(message, sizeof(message), " — ");
    appendCount(message, sizeof(message), posedAnimation.channelCount);
    stringAppend(message, sizeof(message), " channel(s) over ");
    appendCount(message, sizeof(message), posedAnimation.targetCount);
    stringAppend(message, sizeof(message), " target(s), ");
    appendCount(message, sizeof(message), posedAnimation.durationTicks);
    stringAppend(message, sizeof(message), " tick(s) long, authored against ");
    stringAppend(message, sizeof(message), (posedAnimation.skeletonTag[0] != '\0')
                                               ? posedAnimation.skeletonTag
                                               : "a skeleton it does not name");
    {
        Real32 slopeToChange;
        Unsigned32 intervals;

        animationMeasureTangentScale(&posedAnimation, &slopeToChange, &intervals);
        if (intervals > 0U)
        {
            stringAppend(message, sizeof(message), "; its tangents account for ");
            appendThousandths(message, sizeof(message), slopeToChange);
            stringAppend(message, sizeof(message), " times the change they span over ");
            appendCount(message, sizeof(message), intervals);
            stringAppend(message, sizeof(message),
                         " interval(s) — about one means per tick, about eight hundred "
                         "means per second");
        }
    }
    if (posedAnimation.chainCount > 0U)
    {
        stringAppend(message, sizeof(message), ", and ");
        appendCount(message, sizeof(message), posedAnimation.chainCount);
        stringAppend(message, sizeof(message),
                     " inverse kinematics chain(s) this does not follow");
    }
    platformLogMessage(message);

    if (stringContainsIgnoringCase(posedAnimation.resourceName, "2o-") ||
        stringContainsIgnoringCase(posedAnimation.resourceName, "2o_"))
    {
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: ");
        stringAppend(message, sizeof(message), posedAnimation.resourceName);
        stringAppend(message, sizeof(message),
                     " is authored against an object this scene has not got, so it would "
                     "place the Sim relative to nothing — looking for one that stands on "
                     "its own");
        platformLogMessage(message);
        memoryArenaRewindToMarker(globalArena, marker);
        return ENGINE_DISC_WORKING;
    }

    if (discContentPoseFromAnimation(&discSearch, &posedAnimation, ANIMATION_POSE_TICK,
                                     globalArena))
    {
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: posed ");
        appendCount(message, sizeof(message), discSearch.verticesPosed);
        stringAppend(message, sizeof(message), " of ");
        appendCount(message, sizeof(message), discSearch.mesh.vertexCount);
        stringAppend(message, sizeof(message), " vertices over ");
        appendCount(message, sizeof(message), discSearch.bonesPosed);
        stringAppend(message, sizeof(message), " bone(s), ");
        appendCount(message, sizeof(message), discSearch.channelsApplied);
        stringAppend(message, sizeof(message), " channel(s) of the animation reaching them");
        stringAppend(message, sizeof(message), "; it moved by ");
        appendThousandths(message, sizeof(message), discSearch.poseShift);
        stringAppend(message, sizeof(message), " against a model ");
        appendThousandths(message, sizeof(message), discSearch.poseSpan);
        stringAppend(message, sizeof(message), " across");
        platformLogMessage(message);

        if (animationUsedRestPose)
        {
            message[0] = '\0';
            stringAppend(message, sizeof(message),
                         "engine: that was the rest pose, which should move the mesh almost "
                         "not at all — ");
            if (discSearch.poseShift < discSearch.poseSpan / 20.0f)
            {
                stringAppend(message, sizeof(message),
                             "and it did not, so the pose composes the way the game does");
            }
            else
            {
                stringAppend(message, sizeof(message),
                             "and it did, so the matrices are being composed in some "
                             "convention this engine has wrong");
            }
            platformLogMessage(message);
        }

        {
            Unsigned32 which;

            for (which = 0U; which < discSearch.boneReportCount; which++)
            {
                const DiscContentBoneReport *report = &discSearch.boneReports[which];

                message[0] = '\0';
                stringAppend(message, sizeof(message), "engine:   bone ");
                stringAppend(message, sizeof(message), report->nodeName);
                stringAppend(message, sizeof(message), " — chain of ");
                appendCount(message, sizeof(message), report->chainLength);
                stringAppend(message, sizeof(message), ", ");
                appendCount(message, sizeof(message), report->chainNamed);
                stringAppend(message, sizeof(message), " named by a channel, ");
                appendCount(message, sizeof(message), report->chainApplied);
                stringAppend(message, sizeof(message), " applied");
                if (report->anySkipped)
                {
                    stringAppend(message, sizeof(message), "; first skipped is ");
                    stringAppend(message, sizeof(message), report->skippedNode);
                    stringAppend(message, sizeof(message), " as ");
                    stringAppend(message, sizeof(message),
                                 animationChannelTypeGetName(report->skippedType));
                    stringAppend(message, sizeof(message), " driving its ");
                    stringAppend(message, sizeof(message),
                                 animationAttributeGetName(report->skippedAttribute));
                    stringAppend(message, sizeof(message), " over ");
                    appendCount(message, sizeof(message), report->skippedComponents);
                    stringAppend(message, sizeof(message), " component(s)");
                }
                platformLogMessage(message);
            }
        }
        renderUpdateMeshVertices(&discSearch.mesh, globalArena);
        if (posedAnimation.durationTicks > 0U)
        {
            poseIsAnimated = BOOLEAN_TRUE;
            platformLogMessage("engine: playing it from here, re-skinned each frame on the "
                               "processor from the bind pose kept aside");
            reportCatalogueSlots(BOOLEAN_TRUE);
        }
        else
        {
            platformLogMessage("engine: that one has no duration to play over — keeping its "
                               "verdict and looking for an animation with a length");
            return ENGINE_DISC_WORKING;
        }
    }
    else
    {
        message[0] = '\0';
        stringAppend(message, sizeof(message),
                     "engine: that animation moved nothing — ");
        appendCount(message, sizeof(message), discSearch.channelsApplied);
        stringAppend(message, sizeof(message), " channel(s) reached a bone of ");
        appendCount(message, sizeof(message), discSearch.bonesPosed);
        stringAppend(message, sizeof(message), " posed; looking at the next one");
        platformLogMessage(message);
        memoryArenaRewindToMarker(globalArena, marker);
        return ENGINE_DISC_WORKING;
    }

    return beginTheAnimationList();
}

static EngineDiscLoadStatus seekTheSkin(void)
{
    char message[512];

    static GeometryMesh probe;
    const ResourceIndexEntry *entry;
    MemorySize marker;
    Unsigned8 *bytes;
    MemorySize size;

    if (skinIndexBuilding)
    {
        if (resourceIndexStep(&skinIndex) == RESOURCE_INDEX_WORKING)
        {
            return ENGINE_DISC_WORKING;
        }
        skinIndexBuilding = BOOLEAN_FALSE;
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: ");
        appendCount(message, sizeof(message), skinIndex.countByType[0]);
        stringAppend(message, sizeof(message), " geometry container(s) across ");
        appendCount(message, sizeof(message), skinIndex.filesIndexed);
        stringAppend(message, sizeof(message), " package(s) to look through");
        platformLogMessage(message);
        return ENGINE_DISC_WORKING;
    }

    if (skinCursor >= skinIndex.count || skinScanned >= SKIN_SCAN_LIMIT)
    {
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: opened ");
        appendCount(message, sizeof(message), skinScanned);
        stringAppend(message, sizeof(message), " of ");
        appendCount(message, sizeof(message), skinIndex.count);
        stringAppend(message, sizeof(message),
                     " container(s) and none carried bone assignments");
        platformLogMessage(message);
        discPhase = DISC_PHASE_DONE;
        discLoadStatus = ENGINE_DISC_READY;
        return discLoadStatus;
    }

    entry = &skinIndex.entries[skinCursor];
    marker = memoryArenaGetMarker(globalArena);
    if (!readIndexedResource(entry, &bytes, &size))
    {
        memoryArenaRewindToMarker(globalArena, marker);
        return ENGINE_DISC_WORKING;
    }
    skinCursor++;
    skinScanned++;

    if (bytes != NULL_POINTER &&
        geometryReaderOpen(&probe, bytes, size, globalArena) == GEOMETRY_READ_OK &&
        probe.boneAssignments != NULL_POINTER)
    {
        const VirtualFileEntry *holder =
            virtualFileSystemGetEntry(discFileSystem, entry->fileIndex);

        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: skinned geometry is on this disc — ");
        stringAppend(message, sizeof(message), probe.resourceName);
        stringAppend(message, sizeof(message), ", ");
        appendCount(message, sizeof(message), probe.skinnedVertexCount);
        stringAppend(message, sizeof(message), " of ");
        appendCount(message, sizeof(message), probe.vertexCount);
        stringAppend(message, sizeof(message), " vertices weighted, ");
        appendCount(message, sizeof(message), probe.weightsStoredPerVertex);
        stringAppend(message, sizeof(message), " weight(s) stored per vertex, in ");
        stringAppend(message, sizeof(message),
                     (holder != NULL_POINTER) ? holder->path : "a package it cannot name");
        platformLogMessage(message);
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: found after opening ");
        appendCount(message, sizeof(message), skinScanned);
        stringAppend(message, sizeof(message), " container(s)");
        platformLogMessage(message);

        {
            Unsigned32 skinnedFile = entry->fileIndex;

            memoryArenaRewindToMarker(globalArena, marker);
            platformLogMessage("engine: reading that package instead, for a model with a "
                               "skeleton under it");
            discContentBeginInFile(&discSearch, discFileSystem, globalArena, skinnedFile);
            discPhase = DISC_PHASE_CONTENT;
            return ENGINE_DISC_WORKING;
        }
    }

    memoryArenaRewindToMarker(globalArena, marker);
    return ENGINE_DISC_WORKING;
}

static EngineDiscLoadStatus stepTheIndex(void)
{
    char message[512];

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
            stringAppend(message, sizeof(message), ", ");
            appendCount(message, sizeof(message), textureIndex.censusOverflow);
            stringAppend(message, sizeof(message), " entries of untallied types");
        }
        platformLogMessage(message);
    }

    discPhase = DISC_PHASE_FETCH_TEXTURE;
    return ENGINE_DISC_WORKING;
}

static EngineDiscLoadStatus fetchTheLevel(void)
{
    char message[512];

    MemorySize marker = memoryArenaGetMarker(globalArena);

    if (!fetchLargestLevel(message, sizeof(message)))
    {
        memoryArenaRewindToMarker(globalArena, marker);
        return ENGINE_DISC_WORKING;
    }

    renderSetMesh(&discSearch.mesh, globalArena);
    uploadFoundTexture();
    memoryArenaRewindToMarker(globalArena, textureFetchMarker);
    return finishOrSeekSkin();
}

static EngineDiscLoadStatus fetchTheTexture(void)
{
    char message[512];

    char wanted[RESOURCE_NAME_LIMIT];
    const ResourceIndexEntry *found;

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
        if (succeeded && discSearch.texture.largestIsElsewhere &&
            discSearch.texture.lifoName[0] != '\0')
        {
            discPhase = DISC_PHASE_FETCH_LEVEL;
            return ENGINE_DISC_WORKING;
        }
        renderSetMesh(&discSearch.mesh, globalArena);
        uploadFoundTexture();
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

    return finishOrSeekSkin();
}

static EngineDiscLoadStatus probeTheDisc(void)
{
    char message[512];

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
        stringAppend(message, sizeof(message), "would not read: ");
        stringAppend(message, sizeof(message), virtualReadResultGetName(read));
        platformLogMessage(message);
        return ENGINE_DISC_WORKING;
    }
    {
        const FileSignature *signature = identifySignature(head, headSize);

        stringAppend(message, sizeof(message),
                     (signature != NULL_POINTER) ? signature->name : "unrecognised");
        if (signature != NULL_POINTER && signature->worthFollowing &&
            installerFileIndex == NO_INSTALLER)
        {
            installerFileIndex = nextIndex;
        }
    }
    stringAppend(message, sizeof(message), ", starting ");
    {
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

static EngineDiscLoadStatus openTheInstaller(void)
{
    char message[512];

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

    if (installerStage == 3U)
    {
        MemorySize marker = memoryArenaGetMarker(globalArena);
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

        if (archiveEntry.nextBlockOffsetInBytes <= archiveBlockOffset)
        {
            platformLogMessage("engine: an archive block that does not advance, stopping");
            discPhase = DISC_PHASE_CONTENT;
            return ENGINE_DISC_WORKING;
        }
        archiveBlockOffset = archiveEntry.nextBlockOffsetInBytes;
        return ENGINE_DISC_WORKING;
    }

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

static EngineDiscLoadStatus walkTheDisc(void)
{
    char message[512];

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

static EngineDiscLoadStatus searchTheContent(void)
{
    char message[512];

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
        if (discSearch.largestArenaWant > 0UL)
        {
            stringAppend(message, sizeof(message), " largest allocation wanted ");
            appendCount(message, sizeof(message), (Unsigned32)discSearch.largestArenaWant);
            stringAppend(message, sizeof(message), " bytes;");
        }
        platformLogMessage(message);
    }

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
        if (discSearch.limitedToOneFile)
        {
            message[0] = '\0';
            stringAppend(message, sizeof(message),
                         "engine: that package holds skinned geometry but no model this can "
                         "follow (");
            stringAppend(message, sizeof(message), discContentStatusGetName(status));
            stringAppend(message, sizeof(message), ") — keeping what was already drawn");
            platformLogMessage(message);
            discPhase = DISC_PHASE_DONE;
            discLoadStatus = ENGINE_DISC_READY;
            return discLoadStatus;
        }
        reportDiscFailure(discContentStatusGetName(status));
        return discLoadStatus;
    }

    message[0] = '\0';
    stringAppend(message, sizeof(message), "engine: drawing ");
    stringAppend(message, sizeof(message), discSearch.mesh.name);
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
        stringAppend(message, sizeof(message),
                     discSearch.partWasMoved ? ", which places the part"
                                             : ", which leaves the part where it was");
        platformLogMessage(message);
    }

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

    if (!discSearch.foundInPreferred && discSearch.modelHasTree)
    {
        platformLogMessage("engine: this is not one of the game's own meshes, so it may hold "
                           "only a part of a model");
    }

    if (discSearch.rigidModelsPassed > 0U)
    {
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: walked past ");
        appendCount(message, sizeof(message), discSearch.rigidModelsPassed);
        stringAppend(message, sizeof(message), " rigid model(s) looking for a skinned one");
        if (discSearch.mesh.boneAssignments == NULL_POINTER)
        {
            stringAppend(message, sizeof(message),
                         ", found none, and came back for the first of them");
        }
        platformLogMessage(message);
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

    if (discSearch.materialName[0] != '\0')
    {
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: material ");
        stringAppend(message, sizeof(message), discSearch.materialName);
        if (!discSearch.materialFound)
        {
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

    message[0] = '\0';
    stringAppend(message, sizeof(message), "engine: ");
    if (discSearch.mesh.boneAssignments != NULL_POINTER)
    {
        appendCount(message, sizeof(message), discSearch.mesh.skinnedVertexCount);
        stringAppend(message, sizeof(message), " of ");
        appendCount(message, sizeof(message), discSearch.mesh.vertexCount);
        stringAppend(message, sizeof(message), " vertices are weighted to bones, ");
        appendCount(message, sizeof(message), discSearch.mesh.weightsStoredPerVertex);
        stringAppend(message, sizeof(message), " weight(s) stored per vertex and one implied");
    }
    else
    {
        stringAppend(message, sizeof(message),
                     "the mesh carries no bone assignments — it is rigid, and hangs where its "
                     "node puts it");
    }
    platformLogMessage(message);

    if (discSearch.mesh.boneAssignments != NULL_POINTER)
    {
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: weighted to ");
        appendCount(message, sizeof(message), discSearch.bonesInPalette);
        stringAppend(message, sizeof(message), " bone(s) named by its primitives, out of ");
        appendCount(message, sizeof(message), discSearch.modelTree.storedNodeCount);
        stringAppend(message, sizeof(message),
                     " node(s) in the tree, left in its bind pose because skinning it there "
                     "would move nothing");
        if (discSearch.firstBoneNameCount > 0U)
        {
            Unsigned32 which;

            stringAppend(message, sizeof(message), "; its primitives named bones");
            for (which = 0U; which < discSearch.firstBoneNameCount; which++)
            {
                stringAppend(message, sizeof(message), " ");
                appendCount(message, sizeof(message), discSearch.firstBoneNames[which]);
                stringAppend(message, sizeof(message), " (");
                stringAppend(message, sizeof(message), discSearch.firstBoneNodeNames[which]);
                stringAppend(message, sizeof(message), ")");
            }
        }
        platformLogMessage(message);

        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: ");
        appendCount(message, sizeof(message), discSearch.bonesMatchedToANode);
        stringAppend(message, sizeof(message), " bone name(s) matched a node by identifier, ");
        appendCount(message, sizeof(message), discSearch.bonesWithoutANode);
        stringAppend(message, sizeof(message), " matched none");
        if (discSearch.bonesMeasured > 0U)
        {
            stringAppend(message, sizeof(message), "; over ");
            appendCount(message, sizeof(message), discSearch.bonesMeasured);
            stringAppend(message, sizeof(message),
                         " measured, world x stored is ");
            appendThousandths(message, sizeof(message), discSearch.bindPoseFromIdentity);
            stringAppend(message, sizeof(message), " from the identity and stored is ");
            appendThousandths(message, sizeof(message), discSearch.bindPoseFromWorld);
            stringAppend(message, sizeof(message),
                         " from world — the smaller says which the file holds");
        }
        else if (discSearch.mesh.bindPoseCount == 0U)
        {
            stringAppend(message, sizeof(message), "; the container carried no bind pose");
        }
        platformLogMessage(message);
    }

    if (discSearch.mesh.unusedElementCount > 0U)
    {
        Unsigned32 unused;

        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: vertex elements met and not used —");
        for (unused = 0U; unused < discSearch.mesh.unusedElementCount; unused++)
        {
            const char *elementName =
                geometryElementGetName(discSearch.mesh.unusedElements[unused]);

            stringAppend(message, sizeof(message), " ");
            if (elementName != NULL_POINTER)
            {
                stringAppend(message, sizeof(message), elementName);
                stringAppend(message, sizeof(message), " (");
                appendHexadecimal(message, sizeof(message),
                                  discSearch.mesh.unusedElements[unused]);
                stringAppend(message, sizeof(message), ")");
            }
            else
            {
                stringAppend(message, sizeof(message), "unnamed ");
                appendHexadecimal(message, sizeof(message),
                                  discSearch.mesh.unusedElements[unused]);
            }
            stringAppend(message, sizeof(message), " as format ");
            appendCount(message, sizeof(message), discSearch.mesh.unusedElementFormats[unused]);
            stringAppend(message, sizeof(message), ";");
        }
        platformLogMessage(message);
    }

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

    if (!discSearch.textureFound && discSearch.textureName[0] != '\0' &&
        discSearch.mesh.textureCoordinates != NULL_POINTER)
    {
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

    renderSetMesh(&discSearch.mesh, globalArena);
    uploadFoundTexture();
    return finishOrSeekSkin();
}
return discLoadStatus;
}

EngineDiscLoadStatus engineStepDiscLoad(void)
{
    if (discLoadStatus != ENGINE_DISC_WORKING)
    {
        return discLoadStatus;
    }

    if (!engineTextFontIsSettled() && discCatalogueIsBuilt)
    {
        if (!engineTextStepFont(discFileSystem))
        {
            return ENGINE_DISC_WORKING;
        }
    }

    if (discPhase == DISC_PHASE_LIST_ANIMATIONS)
    {
        return stepTheAnimationList();
    }

    if (discPhase == DISC_PHASE_PLAY_CHOSEN)
    {
        return stepTheChosenAnimation();
    }

    if (discPhase == DISC_PHASE_SEEK_SIM)
    {
        return seekTheSim();
    }

    if (discPhase == DISC_PHASE_SEEK_ANIMATION)
    {
        return seekTheAnimation();
    }

    if (discPhase == DISC_PHASE_SEEK_SKIN)
    {
        return seekTheSkin();
    }

    if (discPhase == DISC_PHASE_INDEX)
    {
        return stepTheIndex();
    }

    if (discPhase == DISC_PHASE_FETCH_LEVEL)
    {
        return fetchTheLevel();
    }

    if (discPhase == DISC_PHASE_FETCH_TEXTURE)
    {
        return fetchTheTexture();
    }

    if (discPhase == DISC_PHASE_PROBE)
    {
        return probeTheDisc();
    }

    if (discPhase == DISC_PHASE_INSTALLER)
    {
        return openTheInstaller();
    }

    if (!discCatalogueIsBuilt)
    {
        return walkTheDisc();
    }

    return searchTheContent();
}

static void loadDiscContent(VirtualFileSystem *fileSystem)
{
    Unsigned32 remaining = 1000000U;

    if (fileSystem == NULL_POINTER)
    {
        return;
    }
    engineBeginDiscLoad(fileSystem);
    while (engineStepDiscLoad() == ENGINE_DISC_WORKING && remaining > 0U &&
           discPhase != DISC_PHASE_LIST_ANIMATIONS)
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

    establishGraphicsMemoryLimit(configuration->graphicsMemoryLimitBytes);

    profilerBeginFrame();
    renderIsReady = renderInitialize(globalArena, configuration->widthInPixels,
                                     configuration->heightInPixels);
    profilerEndFrame();

    if (renderIsReady == BOOLEAN_FALSE)
    {
        platformLogMessage("engine: renderer failed to initialize");
        return BOOLEAN_FALSE;
    }

    engineTextSetWindowSize(configuration->widthInPixels, configuration->heightInPixels);
    simMorphHeldChannel = configuration->heldMorphChannel;

    if (configuration->simArchetype != NULL_POINTER && configuration->simArchetype[0] != '\0')
    {
        simArchetype[0] = '\0';
        stringAppend(simArchetype, sizeof(simArchetype), configuration->simArchetype);
    }
    debugMenuInitialize(&debugMenu);
    debugMenuSetOpen(&debugMenu, configuration->menuIsOpen);
    (void)debugMenuSetPage(&debugMenu, (DebugMenuPage)configuration->menuPage);
    if (!engineTextInitialize(globalArena))
    {
        platformLogMessage("engine: nothing will be drawn in words this run");
    }
    {
        Unsigned8 *block = (Unsigned8 *)memoryArenaAllocate(globalArena, ANIMATION_ARENA_BYTES,
                                                           16UL);

        if (block != NULL_POINTER)
        {
            memoryArenaInitialize(&animationArena, block, ANIMATION_ARENA_BYTES);
            animationArenaReady = BOOLEAN_TRUE;
        }
        else
        {
            platformLogMessage("engine: no room for an animation of its own, so changing one "
                               "will grow the arena instead of replacing what is there");
        }
    }
    if (!resourceCacheBegin(&resourceCache, globalArena, 64U, 64UL * 1024UL))
    {
        platformLogMessage("engine: no room for a resource cache, so every read goes to the "
                           "disc — slower, and correct");
    }
    debugMenuBindPage(&debugMenu, DEBUG_MENU_PAGE_BODY, menuBodyRows, MENU_BODY_CAPACITY);
    {
        char (*rows)[DEBUG_MENU_NAME_LIMIT] = (char (*)[DEBUG_MENU_NAME_LIMIT])
            memoryArenaAllocate(globalArena,
                                (MemorySize)MENU_ANIMATION_CAPACITY *
                                    (MemorySize)DEBUG_MENU_NAME_LIMIT, 1UL);

        if (rows != NULL_POINTER)
        {
            debugMenuBindPage(&debugMenu, DEBUG_MENU_PAGE_ANIMATION, rows,
                              MENU_ANIMATION_CAPACITY);
        }
    }
    {
        char (*rows)[DEBUG_MENU_NAME_LIMIT] = (char (*)[DEBUG_MENU_NAME_LIMIT])
            memoryArenaAllocate(globalArena,
                                (MemorySize)MENU_CLOTHING_CAPACITY *
                                    (MemorySize)DEBUG_MENU_NAME_LIMIT, 1UL);

        if (rows != NULL_POINTER)
        {
            debugMenuBindPage(&debugMenu, DEBUG_MENU_PAGE_CLOTHING, rows,
                              MENU_CLOTHING_CAPACITY);
        }
    }
    composeTheArchetype();
    {
        char message[256];

        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: building a ");
        stringAppend(message, sizeof(message), simArchetype);
        stringAppend(message, sizeof(message), " Sim — ");
        stringAppend(message, sizeof(message), simPartNames[0]);
        stringAppend(message, sizeof(message), " and ");
        stringAppend(message, sizeof(message), simDrawnPartNames[SIM_PART_BODY]);
        stringAppend(message, sizeof(message), ", ");
        stringAppend(message, sizeof(message), simDrawnPartNames[SIM_PART_FACE]);
        stringAppend(message, sizeof(message), ", ");
        stringAppend(message, sizeof(message), simDrawnPartNames[SIM_PART_HAIR]);
        platformLogMessage(message);
    }
    simWardrobeWanted[0] = '\0';
    if (configuration->wornName != NULL_POINTER)
    {
        stringAppend(simWardrobeWanted, sizeof(simWardrobeWanted), configuration->wornName);
    }
    poseIsHeldStill = configuration->poseIsHeld;
    poseHeldTick = configuration->poseHeldTick;
    if (poseIsHeldStill == BOOLEAN_TRUE)
    {
        char message[128];

        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: the pose is held on tick ");
        appendThousandths(message, sizeof(message), poseHeldTick);
        stringAppend(message, sizeof(message), ", so only the deformation moves");
        platformLogMessage(message);
    }
    if (configuration->cameraIsStill == BOOLEAN_TRUE)
    {
        char message[128];

        renderSetCameraOrbitRate(0.0f);
        renderSetCameraAngle(configuration->cameraAngleDegrees * (VICTORIA_PI / 180.0f));
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: the camera is held still at ");
        appendThousandths(message, sizeof(message), configuration->cameraAngleDegrees);
        stringAppend(message, sizeof(message),
                     " degrees, so two frames can be compared; nought is behind a Sim");
        platformLogMessage(message);
    }

    loadDiscContent(configuration->fileSystem);

    engineIsRunning = BOOLEAN_TRUE;
    platformLogMessage("engine: initialized");
    logMemoryBudget();
    return BOOLEAN_TRUE;
}

static void applyTheChoice(void);

Boolean engineHandlePointer(EnginePointerAction action, Integer32 x, Integer32 y)
{
    InterfaceMenuHit hit;

    if (engineIsRunning == BOOLEAN_FALSE)
    {
        return BOOLEAN_FALSE;
    }
    if (action == ENGINE_POINTER_LEFT)
    {
        Boolean wasOverSomething =
            (Boolean)(engineTextGetHovered().target != INTERFACE_MENU_NOTHING);

        engineTextForgetPointer();
        return wasOverSomething;
    }

    if (action == ENGINE_POINTER_RELEASED)
    {
        return BOOLEAN_FALSE;
    }

    engineTextSetPointer(x, y);
    hit = engineTextHitTest(&debugMenu);
    if (action == ENGINE_POINTER_MOVED)
    {
        InterfaceMenuHit before = engineTextGetHovered();

        engineTextSetHovered(hit);
        return (Boolean)(hit.target != before.target || hit.value != before.value);
    }

    engineTextSetHovered(hit);
    switch (hit.target)
    {
    case INTERFACE_MENU_CLOSE:
        debugMenuSetOpen(&debugMenu, BOOLEAN_FALSE);
        return BOOLEAN_TRUE;

    case INTERFACE_MENU_PAGE:
        return debugMenuSetPage(&debugMenu, (DebugMenuPage)hit.value);

    case INTERFACE_MENU_PREVIOUS:
        return debugMenuStepPage(&debugMenu, -1);

    case INTERFACE_MENU_NEXT:
        return debugMenuStepPage(&debugMenu, 1);

    case INTERFACE_MENU_TILE:
        (void)debugMenuSetCursor(&debugMenu, debugMenuGetPage(&debugMenu), hit.value);
        applyTheChoice();
        return BOOLEAN_TRUE;

    default:
        break;
    }
    return BOOLEAN_FALSE;
}

void engineResize(Unsigned32 widthInPixels, Unsigned32 heightInPixels)
{
    engineTextSetWindowSize(widthInPixels, heightInPixels);
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

static void advanceThePose(Real32 elapsedSeconds)
{
    Real32 tick;

    if (!poseIsAnimated || posedAnimation.durationTicks == 0U)
    {
        return;
    }

    VICTORIA_PROFILE_ZONE_BEGIN("advanceThePose");
    if (poseIsHeldStill == BOOLEAN_TRUE)
    {
        tick = poseHeldTick;
        poseTick = tick;
    }
    else
    {
        Real32 duration = (Real32)posedAnimation.durationTicks;
        Real32 elapsedTicks = elapsedSeconds / ANIMATION_TICK_SECONDS;
        Real32 cycles = elapsedTicks / duration;

        tick = elapsedTicks - ((Real32)(Integer32)cycles * duration);
        if (tick < 0.0f)
        {
            tick = 0.0f;
        }
        poseTick = tick;
    }

    if (simMorphHeldChannel > 0U && simMorphHeldChannel < simMorphChannels)
    {
        Unsigned32 channel;

        for (channel = 0U; channel < SIM_MORPH_WEIGHT_LIMIT; channel++)
        {
            simMorphWeights[channel] = 0.0f;
        }
        simMorphWeights[simMorphHeldChannel] = 1.0f;
        discSearch.morphWeights = simMorphWeights;
        discSearch.morphWeightCount = simMorphChannels;

        if (simMorphShowing != simMorphHeldChannel)
        {
            char message[256];

            simMorphShowing = simMorphHeldChannel;
            message[0] = '\0';
            stringAppend(message, sizeof(message), "engine: holding channel ");
            appendCount(message, sizeof(message), simMorphHeldChannel);
            stringAppend(message, sizeof(message), " ");
            stringAppend(message, sizeof(message),
                         discSearch.mesh.morphTargets[simMorphHeldChannel].groupName);
            stringAppend(message, sizeof(message), "/");
            stringAppend(message, sizeof(message),
                         discSearch.mesh.morphTargets[simMorphHeldChannel].channelName);
            stringAppend(message, sizeof(message), " at full strength");
            platformLogMessage(message);
        }
    }
    else if (simMorphMoverCount > 0U)
    {
        Unsigned32 window = (Unsigned32)(elapsedSeconds / SIM_MORPH_SECONDS_PER_CHANNEL);
        Unsigned32 slot = window % simMorphMoverCount;
        Real32 phase = elapsedSeconds - ((Real32)window * SIM_MORPH_SECONDS_PER_CHANNEL);
        Real32 strength = mathSine(VICTORIA_PI * phase / SIM_MORPH_SECONDS_PER_CHANNEL);
        Unsigned32 channel;

        if (strength < 0.0f)
        {
            strength = 0.0f;
        }
        for (channel = 0U; channel < SIM_MORPH_WEIGHT_LIMIT; channel++)
        {
            simMorphWeights[channel] = 0.0f;
        }
        simMorphWeights[simMorphMovers[slot]] = strength;
        discSearch.morphWeights = simMorphWeights;
        discSearch.morphWeightCount = simMorphChannels;

        if (slot != simMorphShowing)
        {
            char message[256];
            Unsigned32 named = simMorphMovers[slot];

            simMorphShowing = slot;
            message[0] = '\0';
            stringAppend(message, sizeof(message), "engine: deforming by channel ");
            appendCount(message, sizeof(message), named);
            stringAppend(message, sizeof(message), " ");
            stringAppend(message, sizeof(message), discSearch.mesh.morphTargets[named].groupName);
            stringAppend(message, sizeof(message), "/");
            stringAppend(message, sizeof(message), discSearch.mesh.morphTargets[named].channelName);
            stringAppend(message, sizeof(message), " alone, for the next four seconds");
            platformLogMessage(message);
        }
    }

    if (discContentPoseFromAnimation(&discSearch, &posedAnimation, tick, globalArena))
    {

        renderUpdateMeshVertices(&discSearch.mesh, globalArena);
    }
    VICTORIA_PROFILE_ZONE_END();
}

const char *engineGetMenuText(void)
{
    debugMenuWriteText(&debugMenu, menuText, sizeof(menuText));
    return menuText;
}

Boolean engineHandleMenuKey(char key)
{
    DebugMenuResult result;

    if (engineIsRunning == BOOLEAN_FALSE)
    {
        return BOOLEAN_FALSE;
    }
    result = debugMenuHandleKey(&debugMenu, key);
    if (result == DEBUG_MENU_IGNORED)
    {
        return BOOLEAN_FALSE;
    }
    if (result == DEBUG_MENU_CHOSE)
    {
        applyTheChoice();
    }
    return BOOLEAN_TRUE;
}

static void applyTheChoice(void)
{
    {
        Unsigned32 row = debugMenuGetCursor(&debugMenu, debugMenuGetPage(&debugMenu));

        switch (debugMenuGetPage(&debugMenu))
        {
        case DEBUG_MENU_PAGE_BODY:
            if (row < menuArchetypeCount &&
                !stringEqualsIgnoringCase(menuBodyArchetypes[row], simArchetype))
            {
                char message[256];

                simArchetype[0] = '\0';
                stringAppend(simArchetype, sizeof(simArchetype), menuBodyArchetypes[row]);
                composeTheArchetype();
                debugMenuSetInEffect(&debugMenu, DEBUG_MENU_PAGE_BODY, row);
                message[0] = '\0';
                stringAppend(message, sizeof(message), "engine: building a ");
                stringAppend(message, sizeof(message), simArchetype);
                stringAppend(message, sizeof(message), " Sim instead");
                platformLogMessage(message);
                restartTheAssembly();
                settleTheSkeleton();
            }
            break;

        case DEBUG_MENU_PAGE_ANIMATION:
            if (row < menuAnimationCount && menuAnimationEntries[row] != NULL_POINTER)
            {
                simWardrobeAnimationWanted = menuAnimationEntries[row];
                debugMenuSetInEffect(&debugMenu, DEBUG_MENU_PAGE_ANIMATION, row);
                if (discLoadStatus != ENGINE_DISC_WORKING)
                {
                    discPhase = DISC_PHASE_PLAY_CHOSEN;
                    discLoadStatus = ENGINE_DISC_WORKING;
                }
            }
            break;

        case DEBUG_MENU_PAGE_CLOTHING:
            if (row < menuClothingCount)
            {
                const char *name = menuClothingNames[row];
                Unsigned32 part = menuClothingParts[row];
                char message[256];

                simWardrobeWantedPart[part][0] = '\0';
                stringAppend(simWardrobeWantedPart[part], (MemorySize)WARDROBE_NAME_LIMIT, name);
                debugMenuSetInEffect(&debugMenu, DEBUG_MENU_PAGE_CLOTHING, row);
                message[0] = '\0';
                stringAppend(message, sizeof(message), "engine: dressing the ");
                stringAppend(message, sizeof(message), wardrobeGetPartName(part));
                stringAppend(message, sizeof(message), " in ");
                stringAppend(message, sizeof(message), name);
                platformLogMessage(message);
                restartTheAssembly();
                settleTheSkeleton();
            }
            break;

        default:
            break;
        }
    }
}

static Unsigned32 catalogRead32(const Unsigned8 *p, MemorySize off)
{
    return (Unsigned32)p[off]           |
           ((Unsigned32)p[off + 1U] << 8)  |
           ((Unsigned32)p[off + 2U] << 16) |
           ((Unsigned32)p[off + 3U] << 24);
}

static Unsigned32 catalogFindThumbInst(Unsigned32 srcGrp, Unsigned32 srcInst)
{
    Unsigned32 i;
    const Unsigned8 *base;

    if (bstCatalogData == NULL_POINTER)
    {
        return 0U;
    }
    base = bstCatalogData + 8U;
    for (i = 0U; i < bstCatalogCount; i++)
    {
        const Unsigned8 *e = base + (MemorySize)i * 104U;
        if (catalogRead32(e, 8U)  == srcGrp &&
            catalogRead32(e, 12U) == srcInst)
        {
            return catalogRead32(e, 28U);
        }
    }
    return 0U;
}

static void thumbnailScaleNearest(Unsigned8 *dst, Unsigned32 dstW, Unsigned32 dstH,
                                   const Unsigned8 *src, Unsigned32 srcW, Unsigned32 srcH)
{
    Unsigned32 dy;
    Unsigned32 dx;

    for (dy = 0U; dy < dstH; dy++)
    {
        for (dx = 0U; dx < dstW; dx++)
        {
            Unsigned32 sx = (dx * srcW) / dstW;
            Unsigned32 sy = (dy * srcH) / dstH;
            const Unsigned8 *s = src + ((MemorySize)sy * srcW + sx) * 4U;
            Unsigned8 *d = dst + ((MemorySize)dy * dstW + dx) * 4U;

            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = s[3];
        }
    }
}

static Boolean stepOneThumbnail(void)
{
    if (engineIsRunning == BOOLEAN_FALSE || menuClothingCount == 0U)
    {
        return BOOLEAN_FALSE;
    }

    if (bstCatalogData == NULL_POINTER)
    {
        const ResourceIndexEntry *catEntry;
        Unsigned8 *bytes;
        MemorySize size;
        MemorySize catMarker;

        if (!bstCatalogLoading)
        {
            catEntry = resourceIndexFind(&simIndex,
                                         (Unsigned32)PACKAGE_TYPE_CATALOG_INDEX,
                                         0x00000001U, 0U);
            if (catEntry == NULL_POINTER)
            {
                bstCatalogLoading = BOOLEAN_TRUE;
                return BOOLEAN_FALSE;
            }
            thumbnailLoadEntry = catEntry;
            bstCatalogLoading  = BOOLEAN_TRUE;
        }

        if (thumbnailLoadEntry == NULL_POINTER)
        {
            return BOOLEAN_FALSE;
        }

        catMarker = memoryArenaGetMarker(globalArena);
        if (!readIndexedResource(thumbnailLoadEntry, &bytes, &size))
        {
            memoryArenaRewindToMarker(globalArena, catMarker);
            return BOOLEAN_FALSE;
        }
        if (bytes != NULL_POINTER && size > 8U)
        {
            Unsigned32 ver = catalogRead32(bytes, 0U);
            Unsigned32 cnt = catalogRead32(bytes, 4U);
            if (ver == 2U && cnt > 0U && cnt <= 65536U)
            {
                bstCatalogData  = bytes;
                bstCatalogCount = cnt;
            }
        }
        if (bstCatalogData == NULL_POINTER)
        {
            memoryArenaRewindToMarker(globalArena, catMarker);
        }
        thumbnailLoadEntry = NULL_POINTER;
        return BOOLEAN_TRUE;
    }

    if (thumbnailHop == THUMBNAIL_HOP_IDLE)
    {
        Unsigned32 row;

        for (row = thumbnailNextRow; row < menuClothingCount; row++)
        {
            Unsigned32 slot;
            Boolean found;
            const ResourceIndexEntry *skinEntry;
            Unsigned32 thumbInst;
            const ResourceIndexEntry *jpegEntry;

            if (menuClothingNames[row][0] == '\0')
            {
                continue;
            }

            found = BOOLEAN_FALSE;
            for (slot = 0U; slot < (Unsigned32)THUMBNAIL_SLOT_COUNT; slot++)
            {
                if (thumbnailSlots[slot].row == row)
                {
                    found = BOOLEAN_TRUE;
                    break;
                }
            }
            if (found)
            {
                continue;
            }

            skinEntry = resourceIndexFindNamed(&simIndex,
                                               (Unsigned32)PACKAGE_TYPE_SKIN_ENTRY,
                                               menuClothingNames[row]);
            if (skinEntry == NULL_POINTER)
            {
                continue;
            }
            thumbInst = catalogFindThumbInst(skinEntry->groupIdentifier,
                                              skinEntry->instanceIdentifier);
            if (thumbInst == 0U)
            {
                continue;
            }
            jpegEntry = resourceIndexFind(&simIndex,
                                           (Unsigned32)PACKAGE_TYPE_JPEG,
                                           thumbInst, 0U);
            if (jpegEntry == NULL_POINTER)
            {
                continue;
            }

            for (slot = 0U; slot < (Unsigned32)THUMBNAIL_SLOT_COUNT; slot++)
            {
                if (thumbnailSlots[slot].row == (Unsigned32)MENU_CLOTHING_CAPACITY)
                {
                    thumbnailActiveSlot            = slot;
                    thumbnailSlots[slot].row       = row;
                    thumbnailSlots[slot].ready     = BOOLEAN_FALSE;
                    thumbnailLoadEntry             = jpegEntry;
                    thumbnailNextRow               = row + 1U;
                    thumbnailHop                   = THUMBNAIL_HOP_JPEG;
                    return BOOLEAN_TRUE;
                }
            }
            return BOOLEAN_FALSE;
        }
        return BOOLEAN_FALSE;
    }

    {
        Unsigned8 *bytes;
        MemorySize size;
        MemorySize marker = memoryArenaGetMarker(globalArena);

        if (!readIndexedResource(thumbnailLoadEntry, &bytes, &size))
        {
            memoryArenaRewindToMarker(globalArena, marker);
            return BOOLEAN_FALSE;
        }
        thumbnailLoadEntry = NULL_POINTER;
        if (bytes != NULL_POINTER && size > 0U)
        {
            MemorySize rgbaBytes = 256U * 256U * 4U;
            Unsigned8 *rgba = (Unsigned8 *)memoryArenaAllocate(globalArena,
                                                                rgbaBytes, 4UL);
            if (rgba != NULL_POINTER)
            {
                Unsigned32 w = 0U, h = 0U;

                if (jpegReadToRgba(bytes, size, rgba, rgbaBytes, &w, &h)
                        == JPEG_READ_OK &&
                    w > 0U && h > 0U)
                {
                    thumbnailScaleNearest(thumbnailSlots[thumbnailActiveSlot].pixels,
                                          (Unsigned32)THUMBNAIL_SIZE,
                                          (Unsigned32)THUMBNAIL_SIZE,
                                          rgba, w, h);
                    thumbnailSlots[thumbnailActiveSlot].ready = BOOLEAN_TRUE;
                }
            }
        }
        memoryArenaRewindToMarker(globalArena, marker);
        if (!thumbnailSlots[thumbnailActiveSlot].ready)
        {
            thumbnailSlots[thumbnailActiveSlot].row = (Unsigned32)MENU_CLOTHING_CAPACITY;
        }
        thumbnailHop = THUMBNAIL_HOP_IDLE;
        return BOOLEAN_TRUE;
    }
}

Boolean engineGetThumbnailPixels(Unsigned32 row, const Unsigned8 **rgbaPixels,
                                  Unsigned32 *width, Unsigned32 *height)
{
    Unsigned32 i;

    for (i = 0U; i < (Unsigned32)THUMBNAIL_SLOT_COUNT; i++)
    {
        if (thumbnailSlots[i].row == row && thumbnailSlots[i].ready == BOOLEAN_TRUE)
        {
            *rgbaPixels = thumbnailSlots[i].pixels;
            *width = (Unsigned32)THUMBNAIL_SIZE;
            *height = (Unsigned32)THUMBNAIL_SIZE;
            return BOOLEAN_TRUE;
        }
    }
    return BOOLEAN_FALSE;
}

void engineStepThumbnail(void)
{
    Unsigned32 iterations;

    for (iterations = 0U; iterations < 8U; iterations++)
    {
        if (!stepOneThumbnail())
        {
            break;
        }
    }
}

static Boolean mainMenuDecodeImage(const Unsigned8 *bytes, MemorySize size, Unsigned8 **rgba,
                                   Unsigned32 *width, Unsigned32 *height)
{
    Unsigned32 w = 0U;
    Unsigned32 h = 0U;
    Unsigned32 decodedWidth = 0U;
    Unsigned32 decodedHeight = 0U;
    MemorySize scratchCapacity = 0UL;
    MemorySize marker = memoryArenaGetMarker(globalArena);
    Unsigned8 *out = NULL_POINTER;
    Unsigned8 *scratch = NULL_POINTER;

    *rgba = NULL_POINTER;
    *width = 0U;
    *height = 0U;

    if (pngPeekDimensions(bytes, size, &w, &h, &scratchCapacity) == PNG_READ_OK)
    {
        out = (Unsigned8 *)memoryArenaAllocate(globalArena, (MemorySize)w * h * 4U, 16UL);
        scratch = (Unsigned8 *)memoryArenaAllocate(globalArena, scratchCapacity, 16UL);

        if (out != NULL_POINTER && scratch != NULL_POINTER &&
            pngReadToRgba(bytes, size, out, (MemorySize)w * h * 4U, scratch, scratchCapacity,
                          &decodedWidth, &decodedHeight) == PNG_READ_OK)
        {
            *rgba = out;
            *width = decodedWidth;
            *height = decodedHeight;
            return BOOLEAN_TRUE;
        }
    }
    memoryArenaRewindToMarker(globalArena, marker);

    if (tgaPeekDimensions(bytes, size, &w, &h) == TGA_READ_OK)
    {
        out = (Unsigned8 *)memoryArenaAllocate(globalArena, (MemorySize)w * h * 4U, 16UL);

        if (out != NULL_POINTER &&
            tgaReadToRgba(bytes, size, out, (MemorySize)w * h * 4U, &decodedWidth, &decodedHeight) ==
                TGA_READ_OK)
        {
            *rgba = out;
            *width = decodedWidth;
            *height = decodedHeight;
            return BOOLEAN_TRUE;
        }
    }
    memoryArenaRewindToMarker(globalArena, marker);

    if (jpegPeekDimensions(bytes, size, &w, &h) == JPEG_READ_OK)
    {
        out = (Unsigned8 *)memoryArenaAllocate(globalArena, (MemorySize)w * h * 4U, 16UL);

        if (out != NULL_POINTER &&
            jpegReadToRgba(bytes, size, out, (MemorySize)w * h * 4U, &decodedWidth,
                           &decodedHeight) == JPEG_READ_OK)
        {
            *rgba = out;
            *width = decodedWidth;
            *height = decodedHeight;
            return BOOLEAN_TRUE;
        }
    }
    memoryArenaRewindToMarker(globalArena, marker);
    return BOOLEAN_FALSE;
}

static Boolean mainMenuStepImages(void)
{
    const UIElement *element;
    Integer32 left;
    Integer32 top;
    Integer32 right;
    Integer32 bottom;
    Unsigned32 rectWidth;
    Unsigned32 rectHeight;
    const ResourceIndexEntry *entry;
    Unsigned8 *bytes = NULL_POINTER;
    MemorySize size = 0UL;
    Unsigned8 *rgba = NULL_POINTER;
    Unsigned32 width = 0U;
    Unsigned32 height = 0U;
    Boolean decoded = BOOLEAN_FALSE;
    MemorySize marker;
    Unsigned32 imageType = UI_IMAGE_TYPE_IDENTIFIER;
    Unsigned32 imageGroup = 0U;
    Unsigned32 imageInstance = 0U;

    if (mainMenuImageCursor >= mainMenuLayout.elementCount)
    {
        return BOOLEAN_TRUE;
    }

    element = &mainMenuLayout.elements[mainMenuImageCursor];
    if (element->hasImage == BOOLEAN_FALSE || element->imageNumberCount < 2U)
    {
        mainMenuImageCursor++;
        return BOOLEAN_TRUE;
    }

    uiLayoutGetAbsoluteArea(&mainMenuLayout, mainMenuImageCursor, &left, &top, &right, &bottom);
    rectWidth = (Unsigned32)(right - left);
    rectHeight = (Unsigned32)(bottom - top);

    {
        if (element->imageNumberCount == 2U)
        {
            imageGroup = element->imageNumbers[0];
            imageInstance = element->imageNumbers[1];
        }
        else if (element->imageNumberCount == 3U)
        {
            imageType = element->imageNumbers[0];
            imageGroup = element->imageNumbers[1];
            imageInstance = element->imageNumbers[2];
        }
        else
        {
            mainMenuImageCursor++;
            return BOOLEAN_TRUE;
        }

        entry = resourceIndexFindInGroup(&mainMenuIndex, imageType, imageGroup, imageInstance, 0U);
        if (entry == NULL_POINTER)
        {
            entry = resourceIndexFind(&mainMenuIndex, imageType, imageInstance, 0U);
        }
    }

    if (entry == NULL_POINTER)
    {
        mainMenuImageCursor++;
        return BOOLEAN_TRUE;
    }

    marker = memoryArenaGetMarker(globalArena);
    if (!readIndexedResource(entry, &bytes, &size))
    {
        return BOOLEAN_FALSE;
    }

    if (bytes == NULL_POINTER || size == 0UL)
    {
        memoryArenaRewindToMarker(globalArena, marker);
        mainMenuImageCursor++;
        return BOOLEAN_TRUE;
    }

    decoded = mainMenuDecodeImage(bytes, size, &rgba, &width, &height);

    if (decoded == BOOLEAN_FALSE || rgba == NULL_POINTER)
    {
        memoryArenaRewindToMarker(globalArena, marker);
        mainMenuImageCursor++;
        return BOOLEAN_TRUE;
    }

    if (rectWidth > 0U && height == rectHeight && width > rectWidth && (width % rectWidth) == 0U)
    {
        Unsigned32 frameCount = width / rectWidth;
        const UIElement *thisElement = &mainMenuLayout.elements[mainMenuImageCursor];
        MemorySize pointerCapacity = (MemorySize)frameCount * sizeof(Unsigned8 *);
        Unsigned8 **framePointers =
            (Unsigned8 **)memoryArenaAllocate(globalArena, pointerCapacity, 16UL);

        if (framePointers != NULL_POINTER && frameCount <= MAIN_MENU_MAX_FRAMES_PER_IMAGE)
        {
            Boolean copiedAll = BOOLEAN_TRUE;
            Unsigned32 frameIndex;

            mainMenuImages[mainMenuImageCursor].frameCount = frameCount;
            mainMenuImages[mainMenuImageCursor].frameWidth = rectWidth;
            mainMenuImages[mainMenuImageCursor].frameHeight = rectHeight;
            mainMenuImages[mainMenuImageCursor].isAnimated =
                (Boolean)stringEquals(thisElement->className, "0x4d9ccdb1");

            for (frameIndex = 0U; frameIndex < frameCount; frameIndex++)
            {
                MemorySize frameCapacity = (MemorySize)rectWidth * (MemorySize)rectHeight *
                                           (MemorySize)INTERFACE_BYTES_PER_PIXEL;
                Unsigned8 *frame =
                    (Unsigned8 *)memoryArenaAllocate(globalArena, frameCapacity, 16UL);

                if (frame != NULL_POINTER)
                {
                    Unsigned32 row;

                    for (row = 0U; row < rectHeight; row++)
                    {
                        const Unsigned8 *sourceRow =
                            &rgba[(MemorySize)row * width * INTERFACE_BYTES_PER_PIXEL +
                                  (MemorySize)frameIndex * rectWidth * INTERFACE_BYTES_PER_PIXEL];
                        Unsigned8 *destRow =
                            &frame[(MemorySize)row * rectWidth * INTERFACE_BYTES_PER_PIXEL];

                        memoryCopy(destRow, sourceRow,
                                   (MemorySize)rectWidth * INTERFACE_BYTES_PER_PIXEL);
                    }
                    framePointers[frameIndex] = frame;
                }
                else
                {
                    copiedAll = BOOLEAN_FALSE;
                    framePointers[frameIndex] = NULL_POINTER;
                }
            }

            if (copiedAll == BOOLEAN_TRUE)
            {
                for (frameIndex = 0U; frameIndex < frameCount; frameIndex++)
                {
                    mainMenuImages[mainMenuImageCursor].framePixels[frameIndex] =
                        framePointers[frameIndex];
                }
                mainMenuImages[mainMenuImageCursor].pixels = framePointers[0];
                mainMenuImages[mainMenuImageCursor].width = rectWidth;
                mainMenuImages[mainMenuImageCursor].height = rectHeight;
            }
        }
    }
    else
    {
        mainMenuImages[mainMenuImageCursor].frameCount = 1U;
        mainMenuImages[mainMenuImageCursor].frameWidth = width;
        mainMenuImages[mainMenuImageCursor].frameHeight = height;
        mainMenuImages[mainMenuImageCursor].framePixels[0] = rgba;
        mainMenuImages[mainMenuImageCursor].pixels = rgba;
        mainMenuImages[mainMenuImageCursor].width = width;
        mainMenuImages[mainMenuImageCursor].height = height;
    }

    mainMenuImageCursor++;
    return BOOLEAN_TRUE;
}

static Unsigned32 mainMenuPickFrame(Unsigned32 elementIndex)
{
    const UIElement *element = &mainMenuLayout.elements[elementIndex];
    MainMenuImage *image = &mainMenuImages[elementIndex];

    if (image->frameCount <= 1U)
    {
        return 0U;
    }

    if (image->isAnimated == BOOLEAN_TRUE)
    {
        return image->currentFrame % image->frameCount;
    }

    if (stringEquals(element->className, "GZWinBtn"))
    {
        if (mainMenuPressedElementIndex == (Integer32)elementIndex)
        {
            return 2U;
        }
        if (mainMenuHoveredElementIndex == (Integer32)elementIndex)
        {
            return 3U;
        }
        return 1U;
    }

    return 0U;
}

static void mainMenuDraw(void)
{
    InterfaceColor fillColor;
    Unsigned32 elementIndex;

    if (interfaceSurfaceBegin(&mainMenuSurface, mainMenuWindowWidth, mainMenuWindowHeight) ==
        BOOLEAN_FALSE)
    {
        return;
    }

    for (elementIndex = 0U; elementIndex < mainMenuLayout.elementCount; elementIndex++)
    {
        const UIElement *element = &mainMenuLayout.elements[elementIndex];
        Integer32 left;
        Integer32 top;
        Integer32 right;
        Integer32 bottom;

        if (uiLayoutIsVisible(&mainMenuLayout, elementIndex) == BOOLEAN_FALSE)
        {
            continue;
        }

        uiLayoutGetAbsoluteArea(&mainMenuLayout, elementIndex, &left, &top, &right, &bottom);

        if (element->hasFillColor && !element->noFill &&
            stringEquals(element->className, "GZWinFlatRect"))
        {
            fillColor.red = element->fillRed;
            fillColor.green = element->fillGreen;
            fillColor.blue = element->fillBlue;
            fillColor.alpha = 255U;
            interfaceSurfaceFill(&mainMenuSurface, left, top, (Unsigned32)(right - left),
                                 (Unsigned32)(bottom - top), fillColor);
        }

        if (element->hasImage && mainMenuImages[elementIndex].frameCount > 0U)
        {
            Unsigned32 frame = mainMenuPickFrame(elementIndex);

            interfaceSurfaceImage(&mainMenuSurface, left, top, (Unsigned32)(right - left),
                                  (Unsigned32)(bottom - top),
                                  mainMenuImages[elementIndex].framePixels[frame],
                                  mainMenuImages[elementIndex].frameWidth,
                                  mainMenuImages[elementIndex].frameHeight);
        }
    }

    interfaceSurfaceEnd(&mainMenuSurface);
    renderSetOverlay(mainMenuSurface.pixels, mainMenuSurface.width, mainMenuSurface.height);
}

static Boolean mainMenuPointIsInsideElement(Integer32 x, Integer32 y, Unsigned32 elementIndex)
{
    Integer32 left;
    Integer32 top;
    Integer32 right;
    Integer32 bottom;

    uiLayoutGetAbsoluteArea(&mainMenuLayout, elementIndex, &left, &top, &right, &bottom);
    return (x >= left && x < right && y >= top && y < bottom);
}

static Integer32 mainMenuHitTest(Integer32 x, Integer32 y)
{
    Integer32 hit = -1;
    Unsigned32 elementIndex;

    for (elementIndex = 0U; elementIndex < mainMenuLayout.elementCount; elementIndex++)
    {
        if (uiLayoutIsVisible(&mainMenuLayout, elementIndex) == BOOLEAN_FALSE)
        {
            continue;
        }
        if (mainMenuPointIsInsideElement(x, y, elementIndex))
        {
            hit = (Integer32)elementIndex;
        }
    }
    return hit;
}

static void mainMenuUpdate(Real32 elapsedSeconds)
{
    Unsigned32 elementIndex;

    for (elementIndex = 0U; elementIndex < mainMenuLayout.elementCount; elementIndex++)
    {
        MainMenuImage *image = &mainMenuImages[elementIndex];

        if (image->isAnimated == BOOLEAN_TRUE && image->frameCount > 1U)
        {
            image->frameAccumulator += elapsedSeconds;
            while (image->frameAccumulator >= MAIN_MENU_SECONDS_PER_FRAME)
            {
                image->frameAccumulator -= MAIN_MENU_SECONDS_PER_FRAME;
                image->currentFrame++;
                if (image->currentFrame >= image->frameCount)
                {
                    image->currentFrame = 0U;
                }
            }
        }
    }

    if (mainMenuPointerIsInside == BOOLEAN_TRUE)
    {
        mainMenuHoveredElementIndex = mainMenuHitTest(mainMenuPointerX, mainMenuPointerY);
    }
    else
    {
        mainMenuHoveredElementIndex = -1;
    }
}

static void mainMenuStep(void)
{
    switch (mainMenuPhase)
    {
    case MAIN_MENU_PHASE_DISC_LOAD:
        {
            EngineDiscLoadStatus status = engineStepGameLoad();

            if (status == ENGINE_DISC_READY)
            {
                mainMenuPhase = MAIN_MENU_PHASE_BUILD_INDEX;
            }
            else if (status == ENGINE_DISC_FAILED)
            {
                platformLogMessage("main menu: disc load failed");
                mainMenuPhase = MAIN_MENU_PHASE_READY;
            }
            else if (discCatalogueIsBuilt == BOOLEAN_TRUE &&
                     discPhase != DISC_PHASE_PROBE && discPhase != DISC_PHASE_INSTALLER)
            {
                mainMenuPhase = MAIN_MENU_PHASE_BUILD_INDEX;
            }
        }
        break;

    case MAIN_MENU_PHASE_BUILD_INDEX:
        {
            static const Unsigned32 wantedTypes[] = {
                UI_LAYOUT_TYPE_IDENTIFIER,
                0x856DDBACUL,
                0x2C1FD321UL,
                0x3C53632CUL
            };
            static const Unsigned32 wantedTypeCount =
                (Unsigned32)(sizeof(wantedTypes) / sizeof(wantedTypes[0]));

            if (mainMenuIndex.fileSystem == NULL_POINTER)
            {
                if (!resourceIndexBegin(&mainMenuIndex, discFileSystem, globalArena, 65536U,
                                        wantedTypes, wantedTypeCount))
                {
                    platformLogMessage("main menu: could not begin resource index");
                    mainMenuPhase = MAIN_MENU_PHASE_READY;
                    break;
                }
            }

            {
                ResourceIndexStatus status = resourceIndexStep(&mainMenuIndex);

                if (status == RESOURCE_INDEX_COMPLETE)
                {
                    char report[256];

                    Unsigned32 rank;

                    report[0] = '\0';
                    stringAppend(report, sizeof(report), "main menu: indexed ");
                    appendCount(report, sizeof(report), mainMenuIndex.count);
                    stringAppend(report, sizeof(report), " resource(s) across ");
                    appendCount(report, sizeof(report), mainMenuIndex.filesIndexed);
                    stringAppend(report, sizeof(report), " package(s)");
                    platformLogMessage(report);
                    for (rank = 0U; rank < mainMenuIndex.censusCount && rank < 8U; rank++)
                    {
                        Unsigned32 typeIdentifier;
                        Unsigned32 howMany;

                        if (resourceIndexGetCensusRank(&mainMenuIndex, rank, &typeIdentifier, &howMany))
                        {
                            report[0] = '\0';
                            stringAppend(report, sizeof(report), "main menu:   type ");
                            appendHexadecimal(report, sizeof(report), typeIdentifier);
                            stringAppend(report, sizeof(report), " x");
                            appendCount(report, sizeof(report), howMany);
                            platformLogMessage(report);
                        }
                    }
                    mainMenuPhase = MAIN_MENU_PHASE_LOAD_LAYOUT;
                }
                else if (status == RESOURCE_INDEX_OUT_OF_ROOM)
                {
                    platformLogMessage("main menu: resource index out of room");
                    mainMenuPhase = MAIN_MENU_PHASE_READY;
                }
            }
        }
        break;

    case MAIN_MENU_PHASE_LOAD_LAYOUT:
        {
            const ResourceIndexEntry *entry;

            entry = resourceIndexFindInGroup(&mainMenuIndex, UI_LAYOUT_TYPE_IDENTIFIER,
                                             UI_LAYOUT_GROUP_IDENTIFIER, 0x49001017U, 0U);
            if (entry == NULL_POINTER)
            {
                entry = resourceIndexFindInGroup(&mainMenuIndex, UI_LAYOUT_TYPE_IDENTIFIER,
                                                 UI_LAYOUT_GROUP_IDENTIFIER, 0U, 0U);
            }
            if (entry == NULL_POINTER)
            {
                entry = resourceIndexFind(&mainMenuIndex, UI_LAYOUT_TYPE_IDENTIFIER, 0U, 0U);
            }
            if (entry == NULL_POINTER)
            {
                platformLogMessage("main menu: no UI layout found, continuing with full disc load");
                mainMenuPhase = MAIN_MENU_PHASE_RESUME_DISC_LOAD;
                break;
            }
            mainMenuLayoutEntry = entry;
            mainMenuPhase = MAIN_MENU_PHASE_READ_LAYOUT;
        }

    case MAIN_MENU_PHASE_READ_LAYOUT:
        {
            Unsigned8 *bytes = NULL_POINTER;
            MemorySize size = 0UL;
            MemorySize marker;
            UILayoutReadResult layoutResult;

            if (mainMenuLayoutEntry == NULL_POINTER)
            {
                mainMenuPhase = MAIN_MENU_PHASE_READY;
                break;
            }

            marker = memoryArenaGetMarker(globalArena);
            if (!readIndexedResource(mainMenuLayoutEntry, &bytes, &size))
            {
                break;
            }

            if (bytes == NULL_POINTER)
            {
                memoryArenaRewindToMarker(globalArena, marker);
                mainMenuPhase = MAIN_MENU_PHASE_READY;
                break;
            }

            layoutResult = uiLayoutRead(&mainMenuLayout, bytes, size);
            if (layoutResult != UI_LAYOUT_READ_OK)
            {
                char message[128];

                message[0] = '\0';
                stringAppend(message, sizeof(message), "main menu: could not read UI layout: ");
                stringAppend(message, sizeof(message), uiLayoutReadResultGetName(layoutResult));
                platformLogMessage(message);
                memoryArenaRewindToMarker(globalArena, marker);
                mainMenuPhase = MAIN_MENU_PHASE_READY;
                break;
            }

            memoryArenaRewindToMarker(globalArena, marker);
            mainMenuImageCursor = 0U;
            mainMenuPhase = MAIN_MENU_PHASE_LOAD_IMAGES;
        }
        break;

    case MAIN_MENU_PHASE_LOAD_IMAGES:
        if (mainMenuImageCursor >= mainMenuLayout.elementCount)
        {
            mainMenuScreenDrawn = BOOLEAN_TRUE;
            mainMenuPhase = MAIN_MENU_PHASE_READY;
            break;
        }
        (void)mainMenuStepImages();
        break;

    case MAIN_MENU_PHASE_RESUME_DISC_LOAD:
        {
            EngineDiscLoadStatus status = engineStepGameLoad();

            if (status == ENGINE_DISC_READY)
            {
                mainMenuPhase = MAIN_MENU_PHASE_READY;
            }
            else if (status == ENGINE_DISC_FAILED)
            {
                platformLogMessage("main menu: full disc load failed");
                mainMenuPhase = MAIN_MENU_PHASE_READY;
            }
        }
        break;

    case MAIN_MENU_PHASE_READY:
    default:
        break;
    }
}

Boolean engineInitializeGame(VirtualFileSystem *fileSystem, Unsigned32 widthInPixels,
                             Unsigned32 heightInPixels, MemorySize graphicsMemoryLimitBytes)
{
    Unsigned8 *menuPixels;

    if (engineIsRunning == BOOLEAN_TRUE)
    {
        return BOOLEAN_TRUE;
    }

    globalArena = memoryBudgetGetGlobalArena();

    if (profilerInitialize(globalArena) == BOOLEAN_FALSE)
    {
        platformLogMessage("engine: profiler unavailable, continuing without it");
    }

    profilerReportText = (char *)memoryArenaAllocate(globalArena, VICTORIA_PROFILER_REPORT_CAPACITY,
                                                       16UL);
    if (profilerReportText != NULL_POINTER)
    {
        profilerReportText[0] = '\0';
    }

    establishGraphicsMemoryLimit(graphicsMemoryLimitBytes);

    profilerBeginFrame();
    if (renderInitialize(globalArena, widthInPixels, heightInPixels) == BOOLEAN_FALSE)
    {
        profilerEndFrame();
        platformLogMessage("engine: renderer failed to initialize");
        return BOOLEAN_FALSE;
    }
    profilerEndFrame();

    engineTextSetWindowSize(widthInPixels, heightInPixels);
    mainMenuWindowWidth = widthInPixels;
    mainMenuWindowHeight = heightInPixels;

    if (engineTextInitialize(globalArena) == BOOLEAN_FALSE)
    {
        platformLogMessage("engine: nothing will be drawn in words this run");
    }

    menuPixels = (Unsigned8 *)memoryArenaAllocate(
        globalArena,
        (MemorySize)MAIN_MENU_SURFACE_WIDTH * (MemorySize)MAIN_MENU_SURFACE_HEIGHT *
            (MemorySize)INTERFACE_BYTES_PER_PIXEL,
        16UL);
    interfaceSurfaceBind(&mainMenuSurface, menuPixels, MAIN_MENU_SURFACE_WIDTH,
                         MAIN_MENU_SURFACE_HEIGHT);

    if (!resourceCacheBegin(&resourceCache, globalArena, 64U, 64UL * 1024UL))
    {
        platformLogMessage("engine: no room for a resource cache, so every read goes to the "
                           "disc — slower, and correct");
    }

    debugMenuInitialize(&debugMenu);
    debugMenuSetOpen(&debugMenu, BOOLEAN_FALSE);
    (void)debugMenuSetPage(&debugMenu, DEBUG_MENU_PAGE_BODY);
    {
        Unsigned8 *block = (Unsigned8 *)memoryArenaAllocate(globalArena, ANIMATION_ARENA_BYTES, 16UL);

        if (block != NULL_POINTER)
        {
            memoryArenaInitialize(&animationArena, block, ANIMATION_ARENA_BYTES);
            animationArenaReady = BOOLEAN_TRUE;
        }
        else
        {
            platformLogMessage("engine: no room for an animation of its own, so changing one "
                               "will grow the arena instead of replacing what is there");
        }
    }
    debugMenuBindPage(&debugMenu, DEBUG_MENU_PAGE_BODY, menuBodyRows, MENU_BODY_CAPACITY);
    {
        char (*rows)[DEBUG_MENU_NAME_LIMIT] = (char (*)[DEBUG_MENU_NAME_LIMIT])
            memoryArenaAllocate(globalArena,
                                (MemorySize)MENU_ANIMATION_CAPACITY *
                                    (MemorySize)DEBUG_MENU_NAME_LIMIT, 1UL);

        if (rows != NULL_POINTER)
        {
            debugMenuBindPage(&debugMenu, DEBUG_MENU_PAGE_ANIMATION, rows, MENU_ANIMATION_CAPACITY);
        }
    }
    {
        char (*rows)[DEBUG_MENU_NAME_LIMIT] = (char (*)[DEBUG_MENU_NAME_LIMIT])
            memoryArenaAllocate(globalArena,
                                (MemorySize)MENU_CLOTHING_CAPACITY *
                                    (MemorySize)DEBUG_MENU_NAME_LIMIT, 1UL);

        if (rows != NULL_POINTER)
        {
            debugMenuBindPage(&debugMenu, DEBUG_MENU_PAGE_CLOTHING, rows, MENU_CLOTHING_CAPACITY);
        }
    }
    composeTheArchetype();
    {
        char message[256];

        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: building a ");
        stringAppend(message, sizeof(message), simArchetype);
        stringAppend(message, sizeof(message), " Sim — ");
        stringAppend(message, sizeof(message), simPartNames[0]);
        stringAppend(message, sizeof(message), " and ");
        stringAppend(message, sizeof(message), simDrawnPartNames[SIM_PART_BODY]);
        stringAppend(message, sizeof(message), ", ");
        stringAppend(message, sizeof(message), simDrawnPartNames[SIM_PART_FACE]);
        stringAppend(message, sizeof(message), ", ");
        stringAppend(message, sizeof(message), simDrawnPartNames[SIM_PART_HAIR]);
        platformLogMessage(message);
    }
    simWardrobeWanted[0] = '\0';
    poseIsHeldStill = BOOLEAN_FALSE;
    poseHeldTick = 0.0f;

    if (fileSystem != NULL_POINTER)
    {
        Unsigned32 remaining = 1000000U;

        engineBeginGameLoad(fileSystem);
        while (engineStepGameLoad() == ENGINE_DISC_WORKING && remaining > 0U &&
               (discCatalogueIsBuilt == BOOLEAN_FALSE || discPhase == DISC_PHASE_PROBE ||
                discPhase == DISC_PHASE_INSTALLER))
        {
            remaining--;
        }

        while (mainMenuPhase != MAIN_MENU_PHASE_READY &&
               mainMenuPhase != MAIN_MENU_PHASE_RESUME_DISC_LOAD && remaining > 0U)
        {
            mainMenuStep();
            remaining--;
        }
    }

    gameModeIsReal = BOOLEAN_TRUE;
    engineIsRunning = BOOLEAN_TRUE;
    platformLogMessage("engine: initialized");
    logMemoryBudget();
    return BOOLEAN_TRUE;
}

void engineBeginGameLoad(VirtualFileSystem *fileSystem)
{
    engineBeginDiscLoad(fileSystem);
}

EngineDiscLoadStatus engineStepGameLoad(void)
{
    return engineStepDiscLoad();
}

Boolean engineHandleGamePointer(EnginePointerAction action, Integer32 x, Integer32 y)
{
    if (engineIsRunning == BOOLEAN_FALSE || gameModeIsReal == BOOLEAN_FALSE)
    {
        return BOOLEAN_FALSE;
    }

    if (action == ENGINE_POINTER_LEFT)
    {
        mainMenuPointerIsInside = BOOLEAN_FALSE;
        mainMenuPressedElementIndex = -1;
        return BOOLEAN_TRUE;
    }

    mainMenuPointerIsInside = BOOLEAN_TRUE;
    mainMenuPointerX = x;
    mainMenuPointerY = y;

    if (action == ENGINE_POINTER_MOVED)
    {
        return BOOLEAN_TRUE;
    }

    if (action == ENGINE_POINTER_PRESSED)
    {
        Integer32 hit = mainMenuHitTest(x, y);

        mainMenuPressedElementIndex = hit;
        return (hit >= 0) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
    }

    if (action == ENGINE_POINTER_RELEASED)
    {
        Boolean wasPressed = (Boolean)(mainMenuPressedElementIndex >= 0);

        mainMenuPressedElementIndex = -1;
        return wasPressed;
    }

    return BOOLEAN_FALSE;
}

void engineRenderFrame(Real32 elapsedSeconds)
{
    if (engineIsRunning == BOOLEAN_FALSE)
    {
        return;
    }

    VICTORIA_PROFILE_ZONE_BEGIN("engineRenderFrame");
    if (gameModeIsReal == BOOLEAN_TRUE)
    {
        mainMenuStep();
        if (mainMenuScreenDrawn == BOOLEAN_TRUE)
        {
            mainMenuUpdate(elapsedSeconds);
            mainMenuDraw();
        }
    }
    else
    {
        engineStepThumbnail();
        advanceThePose(elapsedSeconds);
        engineTextDraw(&debugMenu, engineGetMenuText());
    }
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

    nowMicroseconds = platformGetMicroseconds();
    if (profilerReportText[0] == '\0' ||
        nowMicroseconds - lastReportMicroseconds >= ENGINE_REPORT_INTERVAL_MICROSECONDS)
    {
        MemorySize reportLength =
            profilerWriteReport(profilerReportText, VICTORIA_PROFILER_REPORT_CAPACITY);

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
