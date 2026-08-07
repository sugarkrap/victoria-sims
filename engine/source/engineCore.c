#include "victoria/discContent.h"
#include "victoria/discReader.h"
#include "victoria/engineCore.h"
#include "victoria/jpegReader.h"
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

/* A small real to three decimal places, which is as much as a "how far from the
   identity is this" number needs. There is no formatter for reals here and no C
   library to borrow one from, so the fraction is scaled into an integer and
   padded by hand. Values are expected around nought to a few; a large one
   saturates rather than wrapping, because a comparison that reads as 0.001
   because it overflowed would be worse than one that reads as too big. */
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
    DISC_PHASE_SEEK_SKIN,
    DISC_PHASE_SEEK_SIM,
    DISC_PHASE_SEEK_ANIMATION,
    /* Reading every animation on the disc for its name, so the menu has
       something to offer. Runs after a Sim is drawn and posed, never before:
       it is eleven thousand reads and none of them changes what is on screen. */
    DISC_PHASE_LIST_ANIMATIONS,
    /* Reading the one animation the menu asked for. */
    DISC_PHASE_PLAY_CHOSEN,
    DISC_PHASE_DONE
} DiscPhase;

static DiscPhase discPhase = DISC_PHASE_CONTENT;
static ResourceIndex textureIndex;

/* Looking for a mesh with a skeleton, once the one drawn turns out not to have
 * one.
 *
 * Walking directories has now been asked twice and answered twice: under the
 * plain Sims3D there are three packages with geometry in them and both models
 * they yield are rigid. A Sim's body is not a model sitting in a package — it
 * is assembled at run time from resources the character file never names — so
 * looking harder in the same place will not find it.
 *
 * The index can look everywhere at once. This runs after something is already
 * on screen and changes nothing about it; it exists to answer where skinned
 * geometry actually lives, which is the question the next piece of work starts
 * from. */
static ResourceIndex skinIndex;
static Boolean skinIndexBegun = BOOLEAN_FALSE;
static Boolean skinIndexBuilding = BOOLEAN_FALSE;
static Unsigned32 skinCursor = 0U;
static Unsigned32 skinScanned = 0U;

#define SKIN_INDEX_CAPACITY 32768U

/* How many containers to open looking for one. Each is a read and on the web a
   read is a round trip, so this is a survey and not an exhaustive search: if
   two hundred and fifty six containers drawn from across the whole disc hold no
   bone data, the answer is not "look at more of them". */
#define SKIN_SCAN_LIMIT 256U

/* The search for something to pose the mesh with, which runs only once a
 * skinned mesh is what is on screen. Same shape as the skin search above and
 * for the same reasons: index the disc for one type, then open entries until
 * one reads.
 *
 * The tick posed at is nought — the first moment of the animation — because the
 * point here is to prove a pose is applied at all, and a mesh moved to the
 * opening frame of a real animation is a pose that either looks like a Sim or
 * does not. Sampling a tick chosen by anything other than a clock would be
 * making the number up. */
static ResourceIndex animationIndex;
static Animation posedAnimation;
static Boolean animationIndexBegun = BOOLEAN_FALSE;
static Boolean animationIndexBuilding = BOOLEAN_FALSE;
static Unsigned32 animationCursor = 0U;
static Unsigned32 animationScanned = 0U;

#define ANIMATION_INDEX_CAPACITY 32768U
#define ANIMATION_SCAN_LIMIT 64U
#define ANIMATION_POSE_TICK 0.0f

/* The one animation on the disc whose correct outcome is known before it runs:
   a Sim standing in very nearly the pose its mesh was authored in. Posing by it
   should move almost nothing, so it is the only available check that the pose
   pipeline composes its matrices the way the game does. */
#define ANIMATION_REST_POSE_NAME "a-pose-neutral-stand_anim"
static Boolean animationTriedNamed = BOOLEAN_FALSE;
static Boolean animationUsedRestPose = BOOLEAN_FALSE;

/* Set once an animation has actually posed the mesh, which is what lets the
   frame loop keep advancing it. The tick is kept only for reporting — it is
   recomputed from the clock every frame rather than carried forward. */
static Boolean poseIsAnimated = BOOLEAN_FALSE;
static Real32 poseTick = 0.0f;

/* The parts a Sim is assembled from, by the names the game gives them.
 *
 * Taken from openTS2's own CreateNakedBaseSim, which builds its base case from
 * exactly these four: a skeleton, and three models that skin to it. They are
 * looked for by name because that is how the game refers to them — nothing
 * about a package's contents says "this is the body", only the resource's own
 * name does.
 *
 * This phase reports what it found and draws none of it. Whether the disc
 * actually carries these, and what is in them, is the question that decides how
 * a whole Sim gets assembled, and it is not one to answer by assuming. */
#define SIM_PART_COUNT 4U
/* Who this Sim is: an age and a gender, as the catalogue spells them. "am" is
 * an adult male, "cf" a child female, "tu" a teen of neither.
 *
 * Every name below is composed from it. They were four string literals, which
 * is four places for one decision and is why this engine could draw exactly one
 * Sim — and why 1578 of 1884 catalogue entries were refused for being authored
 * for somebody else. The disc is full of other people. */
static char simArchetype[WARDROBE_ARCHETYPE_LIMIT] = "am";

/* The menu, and the rows it offers.
 *
 * Every one of these was a command-line flag, which means every question costs
 * a restart and a four-minute disc load — so in practice each was asked once
 * and the answer taken on faith. The rows live in the arena rather than in
 * statics because the animation list is eleven thousand names. */
static DebugMenu debugMenu;
#define MENU_BODY_CAPACITY 32U
static char menuBodyRows[MENU_BODY_CAPACITY][DEBUG_MENU_NAME_LIMIT];
/* What each row of the body page means, kept beside it and indexed by the same
   row number: the menu holds text and the engine holds meaning. */
static char menuBodyArchetypes[MENU_BODY_CAPACITY][WARDROBE_ARCHETYPE_LIMIT];
static char menuText[2048];

/* The clothing page, and what each of its rows means.
 *
 * The wardrobe already remembers every entry it could have worn — that is what
 * a run's "or any of —" line is made of, and what `--wear=` exists to be
 * pointed at. This is the same list, all of it, laid out as a page: five slots'
 * worth of garments one after another, each row knowing which part it dresses
 * so that choosing one asks for it in the right place.
 *
 * Rebuilt whenever the catalogue is walked, because a Sim of a different age
 * and gender is offered an entirely different wardrobe. */
#define MENU_CLOTHING_CAPACITY (WARDROBE_PART_COUNT * WARDROBE_ALTERNATIVE_LIMIT)
static Unsigned8 menuClothingParts[MENU_CLOTHING_CAPACITY];
/* The name as the catalogue writes it, kept beside the shortened one the tile
   shows. A row has to be readable and it has to be askable-for, and after the
   shared prefix comes off it can no longer be both. */
static char menuClothingNames[MENU_CLOTHING_CAPACITY][WARDROBE_NAME_LIMIT];
static Unsigned32 menuClothingCount = 0U;
/* The TXMT entry for each clothing-page row, set by fillTheClothingPage from
   simWardrobeAlternativeMaterials. NULL means no material is known for this
   row's garment. Used by thumbnail loading. */
static const ResourceIndexEntry *menuClothingMaterials[MENU_CLOTHING_CAPACITY];

/* Per-row thumbnail for the clothing page.
 *
 * 64 x 64 RGBA (decoded from the prerendered JPEG in BodyShopThumbnails),
 * up to THUMBNAIL_SLOT_COUNT garments cached at a time.  A slot is free
 * when its row equals MENU_CLOTHING_CAPACITY. */
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
    THUMBNAIL_HOP_JPEG  /* load + decode the prerendered JPEG */
} ThumbnailHop;

static ThumbnailHop thumbnailHop = THUMBNAIL_HOP_IDLE;
static Unsigned32 thumbnailNextRow = 0U;
static Unsigned32 thumbnailActiveSlot = 0U;
static const ResourceIndexEntry *thumbnailLoadEntry = NULL_POINTER;

/* BodyShopThumbnails catalog index.
 *
 * The catalog maps (groupIdentifier, instanceIdentifier) of any
 * SKIN_ENTRY to the JPEG thumbnail instance stored in the same package.
 * Loaded once from the CATALOG_INDEX resource after simIndex is ready. */
#define PACKAGE_TYPE_JPEG          0x856DDBACUL
#define PACKAGE_TYPE_CATALOG_INDEX 0x43494745UL

static const Unsigned8 *bstCatalogData  = NULL_POINTER;
static Unsigned32        bstCatalogCount = 0U;
static Boolean           bstCatalogLoading = BOOLEAN_FALSE;

/* The animations the menu can offer, and where each one lives.
 *
 * An animation's name is inside the resource — the index holds a hashed key and
 * nothing legible — so listing them means opening all eleven thousand. That is
 * seconds natively and minutes in a browser, which is why it happens after the
 * Sim is on screen rather than before: the list fills while there is something
 * to look at, and the menu shows what is known so far.
 *
 * Bounded, and what does not fit is counted rather than dropped in silence. */
#define MENU_ANIMATION_CAPACITY 512U
static const ResourceIndexEntry *menuAnimationEntries[MENU_ANIMATION_CAPACITY];
static Unsigned32 menuAnimationCount = 0U;
static Unsigned32 menuAnimationCursor = 0U;
static Unsigned32 menuAnimationOpened = 0U;
/* The animation the menu asked for, waiting to be read. */
static const ResourceIndexEntry *simWardrobeAnimationWanted = NULL_POINTER;

/* Where the animation being played lives.
 *
 * A region of its own, carved from the global arena once, and reset before each
 * animation is read into it. The global arena cannot express this lifetime: it
 * frees by rewinding, so releasing the current animation would release
 * everything allocated after it, and there is a Sim allocated after it. Loading
 * each new one on top instead is what made switching grow the arena until it
 * refused — a few hundred changes of mind and the engine stops.
 *
 * The size is a ceiling on one animation rather than on all of them, which is
 * the point: it is the same number of bytes after a thousand switches as after
 * one. An animation that will not fit is refused and the previous one keeps
 * playing, which is a menu that says no rather than an engine that dies. */
#define ANIMATION_ARENA_BYTES (8UL * 1024UL * 1024UL)
static MemoryArena animationArena;
static Boolean animationArenaReady = BOOLEAN_FALSE;

/* And the bytes resources are read from, kept so that asking for the same one
 * twice does not go back to the disc twice.
 *
 * It knows nothing about where the bytes came from — a file descriptor, or a
 * range a browser delivered three frames later. The read path admits what it
 * read and looks here first, so the same code serves both stores. */
static ResourceCache resourceCache;
/* The four names, built rather than written down.
 *
 * Case does not matter to any of them: resourceIndexFindNamed hashes a name
 * through characterToLowerCase, so amBodyNaked_cres and ambodynaked_cres are
 * the same lookup. They are spelled the way the game spells them anyway,
 * because a log line is read by people. */
static char simPartNames[SIM_PART_COUNT][RESOURCE_NAME_LIMIT];
static ResourceIndex simIndex;
static ResourceNodeDescription simPartTree;
static Boolean simIndexBegun = BOOLEAN_FALSE;
static Boolean simIndexBuilding = BOOLEAN_FALSE;
static Unsigned32 simPartCursor = 0U;
static Unsigned32 simPartsFound = 0U;
/* Of those, how many carry geometry. The skeleton is not one of them, and a Sim
   with a skeleton and nothing hung on it is nothing to draw. */
static Unsigned32 simDrawnPartsFound = 0U;
static Unsigned32 simPartFileIndex = 0U;
static Boolean saidWhatTheDiscHas = BOOLEAN_FALSE;

/* Where the assembly has got to.
 *
 * It has to be resumable, one read at a time, because the browser's disc store
 * holds exactly one delivered range and consuming it clears the hold. An
 * assembly that re-read its whole chain on every attempt could never converge:
 * it would read the tree, pend on the shape, come back, find the tree no longer
 * held, pend on that instead, and alternate forever. That is the lesson the
 * skin search already had written on it — exactly one read per step — and this
 * was built doing thirteen.
 *
 * So each hop below performs at most one read and records what it learned. The
 * structures it parses into outlive the bytes they came from, which is what
 * lets the bytes go back to the arena between hops. */
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
    /* Reading the catalogue, which is where everything a Sim wears and every
       part beyond the four hardcoded names is described. */
    SIM_HOP_CATALOGUE,
    /* Putting on what the catalogue chose. Shape, geometry node, container —
       the same three hops a hardcoded part takes, entered one step further
       along because a catalogue entry names a shape outright and has no
       transform tree to be found through. */
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
/* Whether this range is being painted with the material its own catalogue entry
   named, rather than with the one its shape bound. When it is, the skin-tone
   stand-in stands aside: it exists to guess at exactly this, and a guess must
   not overrule the thing it was standing in for. */
static Boolean simHopWearsItsOwn = BOOLEAN_FALSE;

/* Every part a Sim can be drawn in, whether or not this one has it.
 *
 * The first three are the hardcoded base: the skeleton names no shape at all —
 * it is what the others hang on — so it is probed for and then not drawn. A top
 * and a bottom have no base name because there is no undressed top; they exist
 * only when the catalogue puts one there.
 *
 * Indexed by IDENTITY, not by the order they happened to load. A part whose
 * shape is not on the disc leaves its slot empty rather than letting the ones
 * after it move down, so the texture override, the wardrobe and the range map
 * all mean the same thing whether three parts loaded or one. Packing them was a
 * bug waiting to be tripped over and one indirection to keep straight. */
#define SIM_PART_COUNT_DRAWN 5U
#define SIM_PART_BODY 0U
#define SIM_PART_FACE 1U
#define SIM_PART_HAIR 2U
#define SIM_PART_TOP 3U
#define SIM_PART_BOTTOM 4U
/* How many of them the base assembly looks up by name. The rest are the
   catalogue's to fill. */
#define SIM_BASE_PART_COUNT 3U
static char simDrawnPartNames[SIM_PART_COUNT_DRAWN][RESOURCE_NAME_LIMIT];
static GeometryMesh simParts[SIM_PART_COUNT_DRAWN];
static Boolean simPartLoaded[SIM_PART_COUNT_DRAWN];
/* The material each of a part's primitives should be painted with, taken from
   the catalogue entry that put the part there rather than from the shape. Null
   where the entry said nothing, which is every part of the base Sim. */
static const ResourceIndexEntry *simPartMaterialEntries[SIM_PART_COUNT_DRAWN][RENDER_PART_LIMIT];
/* Which parts the joined model was actually built from, in the order it joined
   them. Not every part that loaded: a Sim wearing a top and a bottom leaves its
   whole body out, and the two are the same volume of Sim. */
static Unsigned32 simJoinParts[SIM_PART_COUNT_DRAWN];
static Unsigned32 simJoinCount = 0U;

/* One weight per deformation channel the joined model declares.
 *
 * Fixed rather than sized to the model, because the engine allocates nothing at
 * run time. Sixty-four covers a Sim's thirty with room over; a model declaring
 * more has its later channels left at rest, which is a shape slightly less
 * deformed than the file asked for rather than a refusal.
 *
 * Driven on the clock the same way the pose is, and for the same reason: a
 * still deformation cannot be told apart from geometry that was always that
 * shape. */
#define SIM_MORPH_WEIGHT_LIMIT 64U
static Real32 simMorphWeights[SIM_MORPH_WEIGHT_LIMIT];

/* Whether the animation plays, and where it stops if it does not. */
static Boolean poseIsHeldStill = BOOLEAN_FALSE;
static Real32 poseHeldTick = 0.0f;
static Unsigned32 simMorphChannels = 0U;

/* The channels worth showing, and which of them is showing now.
 *
 * Not all of them. Twelve of a face's twenty-six move their vertices by under a
 * thousandth of a unit against a model 1.879 across, which is nothing anyone
 * can see — cycling through those would be twelve windows of a still face and
 * would read as the deformation having stopped working.
 *
 * One at a time, because all of them together is not an instrument. The mouth
 * alone is driven by nine channels, so at full strength they fight over the
 * same vertices and the result says nothing about whether any one of them is
 * right. A single named channel swung up and back can be checked against its
 * name: l_cheekpuff had better puff the left cheek. */
#define SIM_MORPH_MOVER_LIMIT 32U
/* Below this a channel is not worth a window. A thousandth of a unit on a model
   1.879 across is half a pixel at any sane distance. */
#define SIM_MORPH_VISIBLE_SHIFT 0.002f
#define SIM_MORPH_SECONDS_PER_CHANNEL 4.0f
static Unsigned32 simMorphMovers[SIM_MORPH_MOVER_LIMIT];
static Unsigned32 simMorphMoverCount = 0U;
static Unsigned32 simMorphShowing = 0xFFFFFFFFUL;
/* One channel, held. Nought sweeps. */
static Unsigned32 simMorphHeldChannel = 0U;
/* The material each primitive of each part binds.
 *
 * A primitive and not a part, because a garment is not one piece: a firefighter
 * carries his suit and the skin at his wrists as two primitives with two
 * materials, and the hardcoded base Sim carries exactly one apiece. So long as
 * every part had one, a part index and a primitive index were the same number
 * and the difference could not show — and the moment a Sim wore anything it did,
 * as a green face. renderSetPartTexture has always been indexed by primitive;
 * it was being handed a part. */
static char simPartMaterials[SIM_PART_COUNT_DRAWN][RENDER_PART_LIMIT][RESOURCE_NAME_LIMIT];

/* The joined model's ranges, flattened in the order the merge concatenates
   them: every primitive of part nought, then every primitive of part one. */
static Unsigned32 simRangeCount = 0U;
static char simRangeMaterials[RENDER_PART_LIMIT][RESOURCE_NAME_LIMIT];
/* The material a range's catalogue entry named for it, which beats the name its
   shape bound. Null on every range of a Sim nothing has dressed. */
static const ResourceIndexEntry *simRangeMaterialEntries[RENDER_PART_LIMIT];
/* Which of the three names each range came from, for the texture override,
   which belongs to a part rather than to a range. */
static Unsigned32 simRangeOfPart[RENDER_PART_LIMIT];

/* What the catalogue chose for this Sim to wear, and the shapes it chose.
 *
 * The wardrobe decides on a name and a slot; these are the resources those
 * decisions landed on. Kept as index entries rather than as bytes because the
 * decision is made during the catalogue walk and acted on after it, and every
 * byte read in between has gone back to the arena by then. Index entries do not
 * move: they live in simIndex, which is never rewound.
 *
 * The whole thing runs after the hardcoded Sim is assembled, painted and on
 * screen, rather than before. Two reasons. The tone a face has to match is not
 * known until a body has been painted with one — it is read off the texture the
 * body ended up wearing, not written down anywhere — and there is nothing to
 * choose a face for until then. And a Sim that appears and then dresses is a
 * Sim whose two states can be told apart in one run; one that is dressed before
 * it is ever drawn cannot be checked against anything. */
/* How many subsets of one garment can be painted separately. Declared here
   because the wardrobe holds them; what they are is written where they are
   read, on catalogueOverrideCount below. */
#define CATALOGUE_OVERRIDE_LIMIT 8U

static Wardrobe simWardrobe;
static const ResourceIndexEntry *simWardrobeShapes[WARDROBE_PART_COUNT];
/* And the materials each chosen entry named for its own subsets, which is where
   a garment's colourway lives. Resolved when the key list is open and applied
   when the mesh is, which are two different steps and two different reads. */
static char simWardrobeOverrideSubsets[WARDROBE_PART_COUNT][CATALOGUE_OVERRIDE_LIMIT]
                                      [PROPERTY_NAME_LIMIT];
static const ResourceIndexEntry
    *simWardrobeOverrideMaterials[WARDROBE_PART_COUNT][CATALOGUE_OVERRIDE_LIMIT];
static Unsigned32 simWardrobeOverrideCount[WARDROBE_PART_COUNT];
/* One TXMT entry per wardrobe alternative, indexed [part][which], parallel to
   wardrobe.alternatives[][]. Set during the catalogue walk; used by thumbnail
   loading to find a garment's texture without re-reading the catalogue entry.
   NULL means no override material was found for this alternative. */
static const ResourceIndexEntry
    *simWardrobeAlternativeMaterials[WARDROBE_PART_COUNT][WARDROBE_ALTERNATIVE_LIMIT];
/* What to dress it in, as asked for on the command line. Empty takes whatever
   the walk meets first, which is one garment out of hundreds — the flag is how
   a run looks at any of the others. */
static char simWardrobeWanted[WARDROBE_NAME_LIMIT];
/* One garment asked for per part, chosen from the menu.
 *
 * Separate from the single `simWardrobeWanted` above, which is the command
 * line's substring and applies to every part at once. A menu chooses a top and
 * then a bottom, and both have to stick — one preference between them would
 * make the second choice forget the first. */
static char simWardrobeWantedPart[WARDROBE_PART_COUNT][WARDROBE_NAME_LIMIT];
/* Set once the wardrobe has been put on, which is what stops the assembly
   walking back into the catalogue and round again. */
static Boolean simWardrobeWorn = BOOLEAN_FALSE;
static Unsigned32 simWardrobePart = 0U;
static const ResourceIndexEntry *simWardrobeEntry = NULL_POINTER;
static ShapeDescription simWardrobeShape;
static Unsigned32 simWardrobeDressed = 0U;

/* Where a wardrobe part has got to. The same chain stepTheFollow walks, kept
   apart from it because that one reads a container to measure it and this one
   reads a container to wear it. */
typedef enum WardrobeStage
{
    WARDROBE_STAGE_SHAPE = 0,
    WARDROBE_STAGE_NODE,
    WARDROBE_STAGE_CONTAINER
} WardrobeStage;

static WardrobeStage simWardrobeStage = WARDROBE_STAGE_SHAPE;

/* The stem of the texture a part should wear INSTEAD of the one its shape
 * binds, or empty to wear what it binds.
 *
 * The base face resource binds its face primitive to a brow material, because
 * it cannot know which face a Sim has — the game overrides it per subset, and
 * openTS2 takes a materialOverridesBySubset for the same reason. This is that
 * override, standing in for the skin-tone resolution that would choose it
 * properly: the tone is taken from whatever the body ended up wearing rather
 * than written down here, so a disc whose bodies are toned differently follows
 * its own naming rather than this one's guess.
 *
 * A stem that resolves to nothing on the disc leaves the part wearing what its
 * shape bound, and says so. */
static char simPartTextureStems[SIM_PART_COUNT_DRAWN][RESOURCE_NAME_LIMIT];
/* The tone the body turned out to wear — "s1" out of "ambodynaked-nude-s1". */
static char simSkinTone[RESOURCE_NAME_LIMIT];
/* Set once the parts are joined and drawn, which stops the animation search
   from posing them. The palette is built against discSearch.modelTree, and that
   tree still belongs to the model the search found — not to these parts. Posing
   a Sim by another model's skeleton resolves its bones to the wrong joints and
   deforms it differently on every frame, which does not read as a wrong pose so
   much as a body coming apart. Giving these parts their own skeleton is the
   next piece of work; until then they are drawn in the bind pose. */
static Boolean simIsAssembled = BOOLEAN_FALSE;

/* Seven types across fourteen hundred packages. The textures alone were twenty
   two thousand on the tested disc, so this is not the geometry search's cap
   with more types hung off it. */
#define SIM_INDEX_CAPACITY 131072U

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

/* Reads the three parts a Sim is drawn from and joins them into one model.
 *
 * All three are in one package, which the probe above established rather than
 * assumed, so this is a single file read followed by three walks of the same
 * chain: a name to a tree, a tree to a shape, a shape to a container.
 *
 * Nothing is skinned here and nothing is painted per part yet. What this
 * answers is whether the three fit together into something person-shaped — and
 * if they do not, everything built on top of them would have been built on a
 * mistake. */
typedef enum SimAssembly
{
    SIM_ASSEMBLY_PENDING = 0,
    SIM_ASSEMBLY_FAILED,
    SIM_ASSEMBLY_DONE
} SimAssembly;

/* Says why a part did not resolve, in the caller's voice rather than a code. */
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

/* Everything a shape names, not only the one thing that gets followed.
 *
 * A shape lists several geometry nodes and several material bindings, and the
 * assembly takes the first node that resolves and stops. That is fine for
 * drawing and useless for finding out what is being left behind — and two
 * things are plainly being left behind: a Sim has no brows, eyes or lips, and
 * the body's fat and pregnancy channels declare themselves and carry no
 * per-vertex data. Both are resources named somewhere and followed by nobody.
 *
 * So this prints the whole list, with whether each name resolves in the index.
 * A name that resolves and is not followed is a lead; a name that does not is
 * either not on this disc or not spelled the way it is being looked up, and the
 * two want telling apart. */
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

/* What a part can be deformed into, as the container itself names it.
 *
 * A probe, not a feature: nothing applies these yet. It is here because the
 * question "what does a Sim's body carry besides its bind pose" is one the disc
 * can answer directly, and every previous guess at that class of question has
 * been wrong in a way only the disc could settle.
 *
 * Both names are printed. Several groups reuse the same channel name, so a
 * channel column alone would read as a list of duplicates. */
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
        /* Bounded because a face carries dozens and the line is for reading.
           What is left out is counted rather than dropped silently. */
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

    /* And whether anything came with them. A part can declare channels and
       carry nothing to move by them — the declaration is in one section of the
       container and the per-vertex data is in the elements — so the two are
       counted separately rather than one being read as evidence of the other. */
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

/* Which declared channels the vertices actually refer to, and how far each one
 * would move the model at full strength.
 *
 * This is the measurement that says the deformation data was read the right way
 * round, and it is worth having for one reason: the per-vertex map packs its
 * four slots into a word MOST significant byte first, while the bone assignment
 * word beside it packs its four slots LEAST significant byte first. Reading
 * either in the other's direction produces channel numbers that are perfectly
 * plausible — small, in range, differing per vertex — and simply wrong.
 *
 * What tells them apart is this: read correctly, the numbers land on channels
 * the container declared and cluster on the two or three a part really uses.
 * Read backwards, they scatter across numbers the declaration has no names for.
 * So the count of vertices per channel is printed against the channel's name,
 * and a name that no vertex reaches is printed too — a declared channel nothing
 * refers to is not an error, but a run of them is the signature of the wrong
 * byte order. */
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

    /* Channel nought is skipped rather than counted as unreached: it is the
       file's "no channel here" and is meant to have no vertices. */
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
        /* Kept for the sweep below. The measurement is already being done here,
           and doing it twice would be two walks of the mesh to reach the same
           list. */
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

/* Decodes whatever level is in hand and gives it to the part, then moves on. */
/* Everything the assembly decided, forgotten, so it can decide it again.
 *
 * The load was a one-shot: it ran to DONE during initialisation and nothing
 * ever asked it a second question. Choosing a different Sim means asking it
 * again, and the only honest way to do that is to put every piece of state the
 * assembly wrote back where it started rather than to patch the parts that
 * seemed to matter — a half-reset assembly draws the last Sim's arms.
 *
 * The index is NOT rebuilt. It is the expensive thing, it is a property of the
 * disc rather than of the Sim, and it is what makes changing your mind cost a
 * second instead of four minutes. */
static void restartTheAssembly(void)
{
    Unsigned32 index;

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
    /* Re-derived rather than carried over: the tone is read off the texture the
       body ends up wearing, and this is a different body. */
    simSkinTone[0] = '\0';
    poseIsAnimated = BOOLEAN_FALSE;

    discPhase = DISC_PHASE_SEEK_SIM;
    discLoadStatus = ENGINE_DISC_WORKING;
}

/* Which Sims this disc can build, asked of the index rather than assumed.
 *
 * Costs no reads at all — the index is already in memory and a name is a hash —
 * so there is no reason to guess. Which ages and genders a given install
 * carries is a question about THAT install: a base game, an expansion and a
 * stuff pack do not agree, and a list taken from a reference is a list about
 * somebody else's disc.
 *
 * Both halves are reported, because they fail differently: a body that is not
 * there means this age and gender is not on the disc, and a skeleton that is
 * not there means nothing of that age can be posed at all. */
static Unsigned32 menuArchetypeCount = 0U;

static void reportArchetypesOnThisDisc(void)
{
    /* Every age the game has, against every gender. Unisex is included because
       the youngest ages are only spelled that way — a baby is `bu` and there is
       no `bm`. Meeting none of a row is an answer too. */
    static const char *const ages[] = { "b", "p", "c", "t", "y", "a", "e" };
    static const char *const genders[] = { "m", "f", "u" };
    char message[512];
    Unsigned32 age;

    /* Rebuilt rather than appended to, because this runs again whenever the
       index does and a page that grew every time would list every Sim twice. */
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
            /* All three parts, not only the body. They are not all there for
               everybody — which part is missing decides whether an archetype is
               unbuildable or merely bald. */
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
                /* A row per archetype the disc can actually build, filled from
                   the same lookups that report them — so the menu cannot offer
                   a Sim the log says is not there. */
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

/* An age with no skeleton of its own borrows the adult's, and says so.
 *
 * `<age>uskel` held for every age the disc was asked about except one: it
 * carries emBodyNaked_cres and efBodyNaked_cres and no euskel_cres at all, so
 * an elder is an adult skeletally and only differs in the meshes hung on it.
 * That is the disc's answer and not a rule anybody could have derived — which
 * is why this substitutes on what the index holds rather than on a list of ages
 * known to be special, and says out loud when it does.
 *
 * A Sim posed by the wrong skeleton does not read as a wrong pose so much as a
 * body coming apart, so a silent fallback here would be worse than none. */
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

/* Builds the four names, and the labels and stems that go with them, out of
 * whoever this Sim is.
 *
 * The naming is the game's and is not guessed at anywhere else: a part is the
 * age and gender followed by what it is, and the skeleton is the age followed
 * by `uskel` — one skeleton per age, shared between genders, which is what
 * `auskel` being unisex means. Whether the disc actually carries a given one is
 * a question for the index, and the load asks it by name and says what it
 * found.
 *
 * Called once, before anything looks anything up. */
static void composeTheArchetype(void)
{
    static const char *const partSuffixes[SIM_PART_COUNT_DRAWN] = { "BodyNaked_cres",
                                                                    "Face_cres",
                                                                    "HairBald_cres", "", "" };
    Unsigned32 part;

    /* The skeleton is per AGE and not per gender, so it takes the first
       character and a `u` for unisex rather than the whole archetype. */
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
            /* A top and a bottom have no name to be looked up by — there is no
               undressed top — so they carry a label for the log and nothing
               else. */
            stringAppend(simDrawnPartNames[part], RESOURCE_NAME_LIMIT,
                         (part == (Unsigned32)SIM_PART_TOP) ? "a top" : "a bottom");
            continue;
        }
        stringAppend(simDrawnPartNames[part], RESOURCE_NAME_LIMIT, simArchetype);
        stringAppend(simDrawnPartNames[part], RESOURCE_NAME_LIMIT, partSuffixes[part]);
        /* simPartNames keeps the skeleton at nought and the drawn parts after
           it, which is the order the load probes them in. */
        simPartNames[part + 1U][0] = '\0';
        stringAppend(simPartNames[part + 1U], RESOURCE_NAME_LIMIT, simDrawnPartNames[part]);
    }

    /* The face's texture stem, which stands in for skin-tone resolution where a
       catalogue entry has not named its own material. */
    stringAppend(simPartTextureStems[SIM_PART_FACE], RESOURCE_NAME_LIMIT, simArchetype);
    stringAppend(simPartTextureStems[SIM_PART_FACE], RESOURCE_NAME_LIMIT, "face");
}

/* Which material a gathered part wears, taken from the shape that named it.
 *
 * A material binds to a primitive BY NAME, not by position. Both names have to
 * be real, too: an unnamed primitive and an unnamed binding compare equal, which
 * is two blanks agreeing rather than a match. That warning was already written
 * in discContent.c before any of this existed, and the rule in it got broken
 * anyway — a Sim's face came out painted with an eyebrow.
 *
 * Shared between the parts the assembly hardcodes and the ones the catalogue
 * chooses, because a garment binds its material exactly the way a base part
 * does and two copies of this would only differ once. */
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
        /* The tone comes from a part wearing what it was bound, never from one
           already overridden, or the override would derive its own input. */
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

/* What the catalogue says a Sim can be made of.
 *
 * The four names a whole Sim is built from here are hardcoded, and that is why
 * it has no brows, eyes or lips and wears nothing: those are not scenegraph
 * resources to be walked to, they are catalogue entries — property sets naming
 * a shape — and until now nothing read a property set at all.
 *
 * A probe, not the feature. It reads entries one at a time, tallies them by the
 * kind each declares, and keeps the first name of each kind so the log can say
 * what is actually on the disc rather than what the reference says should be.
 * Bounded, because the catalogue runs to thousands and the point is to learn
 * what kinds exist, not to hold them.
 *
 * One read per step, like every other hop, because a browser's store answers
 * exactly one. */
/* Six hundred was a sample of one kind of thing. The catalogue is not sorted by
   what an entry is, but it is grouped: the first hundred and fifty read came
   back identical — unnamed, uncategorised, one outfit value between them — and
   a sample that homogeneous says more about where it started than about the
   disc. Two thousand costs two reads apiece and reaches past the first
   neighbourhood of it. */
#define CATALOGUE_SAMPLE_LIMIT 2000U
#define CATALOGUE_KIND_LIMIT 16U
static Unsigned32 catalogueCursor = 0U;
/* The catalogue is walked with a stride rather than end to end.
 *
 * It is not sorted by what an entry is, but it is heavily clustered: the first
 * hundred and fifty entries came back identical — unnamed, uncategorised, one
 * outfit value between them — and that sample was read as evidence about the
 * disc twice, wrongly, before a wider one contradicted it. Marching from the
 * front of a clustered list samples the front of the list.
 *
 * Striding costs exactly the same number of reads and covers the whole of it.
 * Zero means the stride has not been worked out yet. */
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
/* The second half of the hop: an entry names its shape by an index into a
   sidecar, so resolving one takes a read of its own. Held between steps because
   a browser's store answers exactly one read per step. */
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
/* Entries whose key at the wanted index is not a shape at all. Not a failure —
   an overlay or a skin tone has no mesh — so it is counted apart from the ones
   that named a shape and could not find it. */
static Unsigned32 catalogueNotAShape = 0U;
static Unsigned32 catalogueKeysShown = 0U;
/* Named entries that reach no mesh — the candidates for a brow, an eye or a
   lip, which are named things without geometry of their own. */
static Unsigned32 catalogueNamedMeshlessShown = 0U;

/* The catalogue tallied by the slot each entry declares.
 *
 * Everything a Sim can wear or be is in here — outfits, hair, brows, eyes,
 * lips — and they are told apart by `category`, which is the body slot. Which
 * slots exist is a question about THIS disc: a list taken from a reference is a
 * list about somebody else's install, and the base game, an expansion and a
 * stuff pack do not agree about what a Sim has.
 *
 * So the slots are counted rather than named, and each is followed to see
 * whether its entries reach geometry or stop at a texture. That distinction is
 * the whole question for a face: brows and lips are painted onto the face that
 * is already drawn, while eyes may be a mesh of their own, and the two want
 * completely different work. A slot whose entries all end in an overlay needs
 * no mesh loading at all. */
#define CATALOGUE_CATEGORY_LIMIT 24U

/* One tally: a value, how many entries carried it, and what those entries
   reached. Two of these are kept, because the first cut of this counted only
   `category` and `category` turned out not to be the body part at all. */
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

/* What outfit categories an entry belongs to. NOT the body part: hair comes
   through as 0x37F, which is nine bits at once, because hair is worn with every
   outfit. The single-bit values are garments available in one category each. */
static SlotTally catalogueByCategory;
/* Which part of a Sim an entry dresses. This is the one that tells a hair from
   a top from a pair of shoes, and reading `category` for it was a mistake the
   disc corrected: an earlier note here called category the body slot. */
static SlotTally catalogueByOutfit;
/* The uncategorised entries, in full, kept as well as printed. Four is enough
   to see what they are and few enough not to drown the log — and they are kept
   because they are printed early in the longest phase of the load and would
   otherwise be gone from a console by the time it finishes. */
#define CATALOGUE_DUMP_LIMIT 4U
static char catalogueUncategorisedDumps[CATALOGUE_DUMP_LIMIT][512];
static Unsigned32 catalogueUncategorisedShown = 0U;
/* And the named ones, which are the garments. Every entry dumped before these
   was a grouping, so every property list this engine had ever seen in full was
   a grouping's — and the three properties that say what colour a garment is
   are exactly the three a grouping does not carry. */
static char catalogueNamedDumps[CATALOGUE_DUMP_LIMIT][512];
static Unsigned32 catalogueNamedShown = 0U;

/* What an entry says its mesh should be painted with, subset by subset.
 *
 * A shape binds a material, and for a garment that material is one arbitrary
 * colourway: one mesh serves every colour of a cowboy shirt, and the shape has
 * to name something. Which colour THIS entry is, is here —
 *
 *   numoverrides=1; override0subset=body; override0resourcekeyidx=2
 *
 * — an index into the same key list the shape came out of. Taking the shape's
 * binding instead drew brownstriped as decogold and blueplaidtannavy as
 * navywhiteblack, and on a pair of shorts that is the difference between legs
 * and no legs.
 *
 * `subset` is a primitive's name, which is how a material has always bound
 * here. So this is the same rule as the shape's bindings with a better source,
 * not a second mechanism. */
static Unsigned32 catalogueOverrideCount = 0U;
static char catalogueOverrideSubsets[CATALOGUE_OVERRIDE_LIMIT][PROPERTY_NAME_LIMIT];
static Unsigned32 catalogueOverrideKeyIndex[CATALOGUE_OVERRIDE_LIMIT];
/* Entries declaring more than there is room for. Counted and reported: a limit
   that cannot announce itself is a lie told once per run. */
static Unsigned32 catalogueOverridesBeyondRoom = 0U;
/* And what became of them: how many resolved to a material on this disc, and
   how many named a key that was not one. */
static Unsigned32 catalogueOverridesResolved = 0U;
static Unsigned32 catalogueOverridesNotAMaterial = 0U;
/* Where in each tally the entry waiting on its key list sits, so that when the
   list resolves the answer lands against the right row. CATALOGUE_CATEGORY_LIMIT
   means the entry declared no such property. */
static Unsigned32 catalogueEntryCategorySlot = CATALOGUE_CATEGORY_LIMIT;
static Unsigned32 catalogueEntryOutfitSlot = CATALOGUE_CATEGORY_LIMIT;
/* The outfit value itself, not its row. 0x02 is the face, and what is in that
   slot is the question this was all built to answer. */
static Unsigned32 catalogueEntryOutfit = 0U;
#define CATALOGUE_OUTFIT_FACE 0x02U
/* Kept as well as printed, like the uncategorised dumps beside them. These are
   written during the catalogue walk, and the animation search that follows
   prints a line per candidate out of eleven thousand — so anything not repeated
   at the end has scrolled out of a console long before the run finishes. That
   was fixed once for the uncategorised entries and not for these, which is why
   two runs came back without them. */
#define CATALOGUE_FACE_LIMIT 16U
static char catalogueFaceDumps[CATALOGUE_FACE_LIMIT][384];
static Unsigned32 catalogueFaceShown = 0U;

/* Records one entry against a value, and answers which row that was. */
/* What a resource type identifier is called.
 *
 * For saying what a catalogue key points at. "A resource of another type" is
 * true and useless — a material and a mesh overlay want completely different
 * work, and the number alone sends the next reader to a table. */
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
    /* The first NAMED entry, not the first: plenty carry no name, and an
       example of nothing says nothing about what the row holds. */
    if (tally->examples[index][0] == '\0' && name[0] != '\0')
    {
        stringAppend(tally->examples[index], PROPERTY_NAME_LIMIT, name);
    }
    return index;
}
/* Following a resolved shape the rest of the way, for a handful of entries.
 *
 * The nude body declares fatbot and pregbot and carries an empty map, so the
 * fat and pregnancy shapes are not in it. The obvious question is whether a
 * body that is not nude carries them, and the chain to find out is the one just
 * built — shape, geometry node, container — so this walks it for the first few
 * and reports what their containers actually hold.
 *
 * A read each, hence the states. */
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
            /* The first entry of a kind often has no name, so the example is
               the first named one rather than simply the first. An example of
               "(unnamed)" says nothing about what the kind holds. */
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

/* The slots met so far, with what their entries reach.
 *
 * Called both partway through the walk and at the end of it. Partway matters:
 * the walk is six hundred entries at two reads apiece, and a browser console
 * that fills up, or a page closed early, otherwise leaves nothing at all — the
 * first run of this reported the slots only on completion and the log arrived
 * cut off before them. */
/* One tally, said plainly. */
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
    /* Said every time, because a value absent from a sample and a value absent
       from the disc are not the same thing and the difference is invisible once
       this line scrolls away. */
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

/* What the catalogue says a Sim can be made of, counted two ways.
 *
 * Called partway through the walk as well as at the end: the walk is six
 * hundred entries at two reads apiece, and a console that fills up or a page
 * closed early otherwise leaves nothing at all.
 *
 * Two tallies because the first cut of this had one and it was the wrong one.
 * `category` looked like the body slot until hair arrived carrying 0x37F — nine
 * bits at once, because hair is worn with every outfit. It is the set of outfit
 * categories a thing belongs to. `outfit` is the one that should say which part
 * of a Sim it dresses, and it is counted here rather than assumed. */
static void reportCatalogueSlots(Boolean withDumps)
{
    Unsigned32 which;

    reportOneTally(&catalogueByCategory, "category",
                   "which outfit categories a thing belongs to, not which part it dresses");
    reportOneTally(&catalogueByOutfit, "outfit", "which part of a Sim it dresses");

    /* Last, and after the tallies on purpose.
     *
       The tallies run to twenty-odd lines, and anything printed before them is
       off the top of a copied tail. That has now happened to these dumps three
       times: first because they were not repeated at all, then because they
       were repeated in the wrong order. What is most specific goes last. */
    if (!withDumps)
    {
        /* A checkpoint says how the counts are growing. Repeating every stored
           dump alongside them prints the same sixteen lines a dozen times over
           and buries the counts it was called to show. */
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
        /* How many of each are in the index at all. A hop that resolves nothing
           has two very different causes — the sidecars are not there, or they
           are there and are not being matched — and only a count separates
           them. */
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

/* What the walk chose to dress this Sim in, and what it turned down.
 *
 * The refusals are the half worth printing. A wardrobe that dresses nothing
 * looks exactly like a disc carrying nothing to wear, and this catalogue has
 * already been misread twice on precisely that kind of silence — so every entry
 * offered is accounted for by name of reason, and a part left undressed says so
 * against the rule that would have dressed it. */
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

    /* Which of the two ways a Sim can be covered this one ended up in. Named
       before the parts, because it is what decides whether the whole body or
       the top and bottom below are the ones that reach the model. */
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
        /* Which of the two reasons, and not a form of words that covers both.
           They are different states of the disc and the run said the wrong one
           of the two out loud before this told them apart. */
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
            /* Against the rule, so an empty slot can be told from a rule that
               could never have matched anything. */
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

        /* What it could have worn instead, by name. The counts on their own say
           a hundred and fifty others fitted and name none of them, which leaves
           --wear with nothing to be pointed at — the whole reason that flag
           exists is to look at one of these. */
        if (simWardrobe.alternativeCount[part] > 0U)
        {
            Unsigned32 which;

            message[0] = '\0';
            stringAppend(message, sizeof(message), "engine:     or any of —");
            Unsigned32 shown = simWardrobe.alternativeCount[part];

            /* Eight, however many are kept. The store behind this is what the
               menu's clothing page offers and runs to hundreds; a log line
               that long buries everything else the walk says, and the point of
               naming any of them here is to give --wear something to be
               pointed at. */
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

/* A garment's name without the part every tile on the page already says.
 *
 * Every entry for a slot begins with the same thing — ambody, amtop, ambottom —
 * because that is how the catalogue says which Sim and which slot it is for.
 * On a page where all of that is already known it is eight characters of a
 * thirty-character name spent saying nothing, and a tile fits about fifteen.
 * Cutting it is the difference between twenty-four tiles reading
 * "hoodedsweatshirtpants_green" and twenty-four reading "amb..een".
 *
 * Found rather than assumed to be at the front: a good few entries are prefixed
 * CASIE_ by whoever authored them, and the part is in the middle of those. */
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

/* Everything this Sim could wear, as a page.
 *
 * Built from what the wardrobe passed over rather than from a second walk of
 * the catalogue: the walk already made this decision for every entry — right
 * age, right gender, right slot, not the thing the part already wears — and
 * throwing that away to make it again would be two answers to one question.
 *
 * The parts run in the order the wardrobe names them, so a page reads body,
 * face, hair, top, bottom, which is roughly how somebody dresses. The row in
 * effect for each part is marked, and there are five of those on one page
 * against the menu's one marker — so the marker goes on whichever the cursor is
 * nearest, which is the one being looked at. */
static void fillTheClothingPage(void)
{
    Unsigned32 part;
    /* Where the reader was. Choosing a garment rebuilds this page from a fresh
       walk, and a cursor thrown back to the top every time would make browsing
       a list of several hundred impossible — you would lose your place on the
       very action that comes of finding it. */
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
    /* Reset thumbnail loading so it starts over from the new clothing list. */
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
            /* What the part is actually wearing, so the page opens showing the
               Sim on screen rather than showing a list with nothing marked. */
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

/* The sidecar read: an entry's shape index against the key list beside it.
 *
 * A step of its own because it is a second read, and one read per step is the
 * rule a browser's store enforces. The entry that wanted it is gone by now, so
 * what it needed — its instance words, its shape index, its name — was kept.
 *
 * How the sidecar is matched, and why, is on the lookup below. */
static SimAssembly stepTheSidecar(MemorySize marker)
{
    /* Matched on group and instance both, which is what a sidecar shares.
     *
     * Measured rather than taken on faith. Matching instance alone found a list
     * for 334 of 600 entries and every key at the wanted index was a shape;
     * matching both found one for 485, of which 334 were shapes and 151 were
     * resources of some other type. More lists found, and the extra ones are
     * not wrong — a catalogue entry of kind skin covers overlays and tones as
     * well as clothing, and those have no mesh to name. So the count below
     * reports what a non-shape key actually was rather than calling it a
     * failure to resolve. */
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
        /* Not consumed: the read pended, so this must be asked for again. */
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
                /* Against its slot as well as the total. A slot where every
                   entry ends here is painted onto a mesh that is already
                   drawn, and wants no mesh loading at all. */
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
                    /* Already counted above. Nothing more to say about it: the
                       entry names no mesh, which is what an overlay looks
                       like. */
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
                    /* The material for thumbnail loading, resolved here while the
                       key list is still open. The offer below decides winner or
                       alternative after the fact; the material is stored against
                       whichever alternative it adds, so both outcomes get it. */
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
                    /* Offered to the wardrobe, which is the only place in the
                       walk where an entry is looked at as something to wear
                       rather than something to count. Nothing is read here: the
                       decision is made on the name and the slot, both already
                       in hand, and the shape is only remembered. */
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

                            /* What this entry says to paint it with, resolved
                               against the list already open. A whole colourway
                               of a garment lives in these and nowhere else. */
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
                                    /* Said, not assumed. A subset painted with
                                       something that is not a material is a
                                       different discovery from one that could
                                       not be found. */
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
                    /* Record the material for whichever alternative the offer
                       just added, so thumbnail loading can reach the garment's
                       texture without re-reading the catalogue entry. */
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
                    /* Only bodies. A hair or a pair of shoes has nothing to say
                       about where a fat morph lives, and eight reads three deep
                       is enough to answer the question. */
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
                /* Every key of a body's list, with its type. Only key 1 has
                 * been followed so far and a list holds three to five — and
                 * every body container on this disc declares fatbot over an
                 * all-zero map, so the deltas are somewhere this has not
                 * looked. The rest of the list is the nearest place. */
                /* Bodies, and the unnamed entries beside them. The unnamed
                   ones are the question now: two in three of the catalogue is
                   unnamed, meshless and uncategorised, and every one of them
                   carries a key list of ten to sixteen keys. What is in those
                   lists is what a face is made of, or is not. */
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
                /* Everything in the face slot, named and said plainly.
                 *
                   Two hundred and eighty seven entries dress the face; a
                   hundred and fifty one of them paint it and twenty one carry
                   geometry. Which is which is the whole question — a brow is
                   paint on a face already drawn, an eye may be a mesh of its
                   own, and the two want completely different work. The counts
                   say the split exists; only the names say where it falls. */
                /* Named ones only. The unnamed entries in this slot are the
                   groupings, they are dumped in full elsewhere, and there are
                   enough of them at the front of the walk to eat this whole
                   budget before a named one is ever met — which is exactly what
                   happened on the first run of it. */
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

                /* A NAMED entry whose key is not a shape, in full with every
                   key it holds.
                 *
                   That is the shape a brow, an eye or a lip should have: it is
                   a thing somebody chose and named, and it has no mesh of its
                   own because it is painted onto a face that is already drawn.
                   The unnamed ones turned out to be groupings — a tree and a
                   dozen other catalogue entries — so they are not it. */
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

                /* A few in full, because a count of successes says the chain
                   closes and nothing about what it closes onto. */
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
                        /* By name where there is one. Saying only "a resource
                           of another type" is true and useless: a material and
                           a mesh overlay want completely different work, and
                           this line is the one that decides which. */
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

/* Shape to geometry node to container, one read a step, for an entry whose
   shape already resolved. The last hop is the one worth having: what a clothed
   body's container declares and how much of it the map actually reaches. */
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

            /* Every node it names, not only the first that resolves. A morph
             * mesh would be a second node here, and the follow below takes one
             * and stops — so a list of one settles that the fat shape is not
             * reached this way, and a list of several says exactly where it
             * is. */
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

            /* Whether the deltas hold anything, which the map's being empty
             * does not answer.
             *
             * A face vertex can belong to four of twenty-seven channels, so it
             * needs a word per vertex saying which. A body declares one real
             * channel. If its deltas carry real displacements while its map is
             * all noughts, then the map is not how a body's deltas are
             * addressed — the slot is — and the fat shape has been in the
             * container all along, unreachable only through the face's rule. */
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

            /* What the container carried and this reader passed over. The map
             * is present and empty on every body met, so the assignment has to
             * be somewhere else in the container — and the likeliest somewhere
             * is a sparse form: index elements naming which vertices a delta
             * set applies to, instead of a word per vertex. Those are read by
             * nobody here and are listed among the unused. */
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

/* An override's property name: "override", the number, then what is wanted.
   They are numbered from nought and there is one set per override, so the name
   has to be built rather than looked up from a list. */
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

/* What this entry says its subsets should be painted with, before its bytes go
   back to the arena. Only the indices: what they point at needs the key list,
   which is a read of its own and so a step of its own. */
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

    /* How far apart to take them, worked out once. Counting costs no reads —
       the index is already in memory — and it is the difference between a
       sample of the disc and a sample of wherever the index happens to start. */
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

    /* Forward to the next one the stride wants. */
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
            /* The first two entries in full. Which keys a catalogue entry
               actually carries is not something to take from a reference and
               assume — the reference was written against a different disc, and
               the one key looked up so far came back empty. */
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

            /* The slot, before the bytes go back. An entry with no category is
               kept apart from one whose category is nought — the first is a
               property that is not there, the second is a slot numbered
               nought, and reading them as the same would invent a slot. */
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

                /* In full, for the entries that belong to no outfit category at
                   all. Two hundred and thirty eight of six hundred are like
                   that, none of them reaches a mesh, and none carries a name —
                   which is what a brow, an eye or a skin tone would look like
                   from here, and equally what a misread property would. Only
                   the whole record can tell those apart. */
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

                /* A NAMED entry in full — a real garment rather than a grouping.
                 *
                 * Every entry dumped so far has been an unnamed one, and those
                 * are the groupings: eighteen properties, none of them about a
                 * garment. A Sim wearing amtopcowboyshirt_brownstriped is
                 * painted amtopcowboyshirt_decogold, so the colourway is named
                 * somewhere this has never looked, and a garment's own record is
                 * where to look first. `numoverrides` is the reason to expect
                 * more properties than the eighteen that keep turning up. */
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

            /* Partway as well as at the end. The walk is six hundred entries at
               two reads apiece and everything else on the disc has already been
               read by then, so a console that fills or a page closed early
               otherwise carries away nothing — which is exactly what happened
               the first time this ran. */
            if (catalogueRead % 150U == 0U)
            {
                reportCatalogueSlots(BOOLEAN_FALSE);
            }

            /* What the sidecar step will need once this entry's bytes are
               given back. An index of its own is not enough — the list it
               indexes is found by this entry's instance words. */
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
                    /* Read here and resolved there, for the same reason as the
                       shape index: they index the same list, and the list is a
                       read this step has already spent. */
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

/* Putting on what the catalogue chose: shape, geometry node, container, one
 * read a step and one part at a time.
 *
 * The same three hops the assembly walks for a hardcoded part, entered one
 * further along — a catalogue entry names a shape outright, so there is no
 * transform tree to be found through. That is the whole reason a garment costs
 * three reads and the base Sim's parts cost four.
 *
 * The container goes into a mesh of its own and is copied over the part only
 * once it has read cleanly. A garment that will not open then leaves the Sim in
 * what it was already wearing, rather than halfway out of it — which is a
 * distinction that matters here, because the mesh being overwritten is on
 * screen at the time. */
static SimAssembly stepTheWardrobe(MemorySize marker)
{
    Unsigned8 *bytes;
    MemorySize size;
    char message[512];
    Unsigned32 index;

    /* Forward to the next part with something to put on that the arrangement
       will actually draw. A top chosen with no bottom beside it is a decision
       that was made and is not worn, and loading it would cost two reads for a
       mesh nothing joins. */
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
        /* Before the merge, not after: the merge is what sends the paint round
           again, and the paint is what would otherwise walk back into the
           catalogue and dress the same Sim for ever. */
        simWardrobeWorn = BOOLEAN_TRUE;
        simHopPart = 0U;
        simHop = SIM_HOP_MERGE;
        return SIM_ASSEMBLY_PENDING;
    }

    if (simWardrobeEntry == NULL_POINTER)
    {
        if (simWardrobeStage != WARDROBE_STAGE_SHAPE)
        {
            /* The chain broke a hop short of a mesh. Said out loud, because a
               garment that resolves to nothing looks exactly like one that was
               never chosen. */
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
        /* Answered, and empty. This part is given up on rather than asked
           again: the entry is where it was, so a second read returns the same
           nothing and the third does too. A hop that retries what the store has
           already answered does not pend, it spins. */
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
            /* A shape does not name a container. It names a geometry node, and
               that node references the container. */
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

    /* WARDROBE_STAGE_CONTAINER */
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
        /* Not rewound past here: the mesh lives in the arena above the bytes it
           was read from, so giving them back would take it too. */
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine:   ");
        if (simPartLoaded[slot])
        {
            /* A part with a base mesh, replaced. Both counts, because "becomes"
               is the only line that says the swap happened to the model rather
               than only to the wardrobe's mind. */
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
            /* A top and a bottom have no undressed form to give way from: the
               assembly never had one, and the catalogue is the only thing that
               can put one there. */
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
        /* The entry's own materials over the shape's, by the subset each names.
           A subset is a primitive's name, so this is the same binding rule with
           a better source — the shape says what the mesh can be painted, the
           entry says what THIS one is. */
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

/* The painting hops: a material, then its texture, then the top level that
   texture kept somewhere else. One read each, like everything else here. */
static SimAssembly stepThePaint(MemorySize marker)
{
    char wanted[RESOURCE_NAME_LIMIT];
    Unsigned8 *bytes;
    MemorySize size;

    /* simHopPart counts RANGES here, not parts: the assembly reuses it as its
       cursor and the paint has one step per range of the joined model. */
    if (simHopPart >= simRangeCount)
    {
        /* Second time round. The paint is what sends the assembly into the
           catalogue, and the catalogue is what sends it back here — so without
           this the Sim would be dressed, painted, and dressed again for as long
           as the disc stayed open. */
        if (simWardrobeWorn)
        {
            simHop = SIM_HOP_FINISHED;
            return SIM_ASSEMBLY_DONE;
        }
        /* The tone is known now and not before: it is read off the texture the
           body ended up wearing, so a face cannot be chosen for it until a body
           has been painted. */
        wardrobeBegin(&simWardrobe, simArchetype, simWardrobeWanted, simSkinTone);
        {
            /* After begin, which clears them. Whatever the menu has been asked
               for, restated for the walk that is about to happen. */
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
            /* What the catalogue entry named for this subset, where it named
               one. A shape's binding is one arbitrary colourway of a garment
               that has dozens; the entry is the one that says which. */
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
                /* Everything the material names, not only the one texture that
                   gets used.
                 *
                   A face's material is the place a layered face would show
                   itself: the reader keeps a list because a material sometimes
                   holds more than its base texture, and nothing has ever looked
                   at the list. The base face binds `uuface_browbushy_brown`,
                   which is named for a brow and painted on a whole face, and
                   the Sim read out of the tutorial neighbourhood wears one
                   called `..._cmpm`. If a face texture is composited from
                   layers, they are named here or nowhere this engine has
                   looked. */
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
        /* The override, when the part this range belongs to has one and the
           disc carries it. It belongs to a part and not a range — a face is a
           face however many pieces it is drawn in — so the range has to say
           which part it came from. */
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
        /* Not rewound: the description points into these bytes until the level
           is decoded out of them. */
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

    /* SIM_HOP_TOP_LEVEL */
    if (!readIndexedResource(simHopEntry, &bytes, &size))
    {
        memoryArenaRewindToMarker(globalArena, marker);
        return SIM_ASSEMBLY_PENDING;
    }
    simHopEntry = NULL_POINTER;
    if (bytes != NULL_POINTER)
    {
        TextureLevel largest;

        /* A level carries no format of its own, so the only check available is
           whether its length is what those dimensions cost in the format the
           texture that named it owns. One that fails decodes into noise. */
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

/* One hop of the assembly, and at most one read.
 *
 * Returns PENDING whenever a read has not been answered, having given the arena
 * back to where that hop began — so an attempt costs the same whether it is the
 * first or the fiftieth. Nothing is re-read, because each hop records what it
 * learned before the next one starts. */
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
                /* By instance alone: a key's group says which collection the
                   shape was filed under, and the instance words are its name
                   hashed, which identify it wherever it was filed. */
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
        /* The tree is a structure of its own now, so its bytes can go. */
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
            /* A shape does not name a container. It names a geometry node, and
               that node references the container. */
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
        /* Not rewound: the mesh this builds lives in the arena above these
           bytes, so giving them back would take it too. */
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

        /* Which parts this Sim is actually drawn in.
         *
         * Not every part that loaded. A Sim wears either a whole body or a top
         * and a bottom, and the two describe the same volume: joining both puts
         * a pair of trousers through a pair of legs that are already there, and
         * what shows through is decided by whichever triangle the rasterizer
         * reaches last. The wardrobe settles the arrangement; this obeys it. */
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

        /* The parts flattened into the ranges the merge made of them, in the
         * order it concatenated them: every primitive of part nought, then
         * every primitive of part one.
         *
         * This is what the paint walks. It used to walk parts, which was the
         * same list only while every part had exactly one primitive — true of
         * the four hardcoded names and false of the first garment put on one. */
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
        /* Said out loud, because a range the backend cannot hold a texture for
           is drawn under whatever the last one set — which reads as a garment
           bleeding onto a face, not as a limit being reached. */
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

/* Ends the load, or starts the search for a skinned mesh first.
 *
 * Called from every place the load would otherwise be finished, so the search
 * happens once whichever way the texture went. Everything drawable is already
 * uploaded by this point — this only delays the disc reporting itself loaded,
 * and only when what it drew has no skeleton. */
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

    /* What a whole Sim is made of, before anything is done with it. Runs ahead
       of the animation search because it is the cheaper question and the one
       the next piece of work turns on. */
    if (discSearch.mesh.boneAssignments != NULL_POINTER && !simIndexBegun)
    {
        /* The whole chain, because a Sim's parts do not keep it in one place:
           the tree is in Sims06 and the shape it names is not, so resolving a
           reference means asking the disc rather than asking the package. That
           is how the game's own content manager resolves them too — a
           scenegraph reference is global, and treating it as package-local
           found three trees and not one shape. */
        /* Every hop a part needs, so a Sim is one index and not a dependency
           on whichever earlier phase happened to have built one. */
        static const Unsigned32 wantedTypes[11] = { (Unsigned32)PACKAGE_TYPE_CRES,
                                                   (Unsigned32)PACKAGE_TYPE_SHPE,
                                                   (Unsigned32)PACKAGE_TYPE_GMND,
                                                   (Unsigned32)PACKAGE_TYPE_GMDC,
                                                   (Unsigned32)PACKAGE_TYPE_TXMT,
                                                   (Unsigned32)PACKAGE_TYPE_TXTR,
                                                   (Unsigned32)PACKAGE_TYPE_LIFO,
                                                   /* The catalogue, which names everything a Sim
                                                      wears and everything it is built from beyond
                                                      the four names hardcoded here. */
                                                   (Unsigned32)PACKAGE_TYPE_SKIN_ENTRY,
                                                   /* The sidecar an entry's shape index points
                                                      into, without which the index is a number
                                                      and nothing else. */
                                                   (Unsigned32)PACKAGE_TYPE_RESOURCE_KEY_LIST,
                                                   /* Prerendered clothing thumbnails and the
                                                      catalogue that maps SKIN_ENTRY keys to them,
                                                      both in BodyShopThumbnails.package. */
                                                   (Unsigned32)PACKAGE_TYPE_JPEG,
                                                   (Unsigned32)PACKAGE_TYPE_CATALOG_INDEX };

        simIndexBegun = BOOLEAN_TRUE;
        if (resourceIndexBegin(&simIndex, discFileSystem, globalArena, SIM_INDEX_CAPACITY,
                               wantedTypes, 11U))
        {
            /* Said out loud, because a type asked for and not taken looks
               exactly like a disc that holds none of it — which cost a run of
               the disc to work out once. */
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

    /* A skinned mesh is on screen, so there is finally something an animation
       can be applied to. Until now every mesh drawn was rigid and a pose would
       have had nothing to move. */
    /* An assembled Sim with no skeleton of its own must not be posed by the one
       the search found — that is what took the body apart. Without a tree it
       stays in its bind pose and says so; with one it goes on to the animation
       search like anything else. */
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
        /* The vertices as they are now, before anything has posed them, and
           before the first per-attempt marker exists to rewind past. */
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

/* Reads the animation the menu asked for and poses by it.
 *
 * The animation's own data stays in the arena — the pose reads it every frame —
 * so choosing repeatedly costs arena each time. That is the honest trade for a
 * debug menu: the alternative is a second arena to rewind, and a Sim that
 * cannot be asked a second question is what this whole page exists to fix. */
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
    /* Reset, not appended to. This is the whole reason the region exists. */
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
        /* Most likely too big for the region. Refused rather than loaded on top
           of the arena, which is what this was built to stop. */
        platformLogMessage("engine: that animation would not fit the space kept for one, so the "
                           "Sim goes on doing what it was doing");
    }
    simWardrobeAnimationWanted = NULL_POINTER;
    discPhase = DISC_PHASE_DONE;
    discLoadStatus = ENGINE_DISC_READY;
    return discLoadStatus;
}

/* Starts filling the menu's animation page, once there is a Sim to pose.
 *
 * After the search and not instead of it: the search stops at the first
 * animation that works, which is what puts something on screen quickly, and
 * this then walks the rest for their names. Nothing here changes the pose. */
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

/* One animation read, named, and offered to the menu if it could pose this Sim.
 *
 * The test is the naming convention and the skeleton tag, not a trial pose:
 * posing to find out would move the model, and a list must not change what it
 * is a list of. So it is the same two rules the search itself applies — an
 * animation authored to an object cannot be placed without the object, and one
 * authored against another skeleton reaches none of these bones. */
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
    /* One read per step, like everything else that has to survive a store which
       answers PENDING. */
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
    /* Given straight back: the list keeps a name and a place, not an animation.
       Holding eleven thousand of them would be the arena and then some. */
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
        /* Once, not once per attempt. Assembling a Sim takes many steps
           against a store that answers one read at a time, and a line
           logged on each of them buried the console under fifteen thousand
           copies of itself. */
        if (!saidWhatTheDiscHas)
        {
            saidWhatTheDiscHas = BOOLEAN_TRUE;
            platformLogMessage(message);
        }
        /* At least one thing to draw, rather than all four.
         *
         * Demanding all four meant an elder could not be built at all: this
         * disc carries emBodyNaked_cres and efBodyNaked_cres and no elder
         * bald scalp, because an elder is an adult with different meshes and
         * the scalp is not one of them. Refusing over that costs a whole age
         * to save a bald head — on a Sim the catalogue is about to put hair
         * on anyway.
         *
         * Each hop already skips a part whose name is not on the disc, and
         * the skeleton hop already reports its own absence and leaves the
         * Sim in its bind pose. So the only thing worth refusing over is
         * having nothing to draw. */
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
            /* The sentinel a node carries when it is not a joint, so this
               counts the ones that are. */
            if (simPartTree.nodes[index].boneIdentifier != 0x7FFFFFFFUL)
            {
                bones++;
            }
        }
        simPartsFound++;
        /* The skeleton is simPartNames[0] and carries no geometry. */
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

    /* The rest pose first, by name, before falling back to whatever the
     * scan reaches.
     *
     * This animation is very nearly the pose the mesh was authored in, so
     * posing by it should move the model almost not at all. That makes it
     * the one animation on the disc whose correct result is known in
     * advance, and therefore the only one that can tell a working pose
     * pipeline from a broken one — every other animation produces a shape
     * nobody here can check. openTS2's own SimAnimationTest opens with it
     * for the same reason. */
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
        /* One read per step, for the reason the skin search records: two
           would alternate forever against a store that answers PENDING. */
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
        /* What unit its tangents are in, measured off the animation in
           hand. The curve was followed once on the assumption of per tick
           and threw the Sim about; this is the number that says whether
           that was the shape or the scale. */
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

    /* An animation authored against an object cannot be placed without it.
     *
     * The mark is "2o", as in to-object, and the letter before it is who: a2o
     * is adult, t2o teen, c2o child. Matching only "a2o" caught a bench
     * press and then settled on a teen applying zit cream to a mirror that
     * is equally not there — the convention is a family, not a prefix.
     *
     * A bench press start is authored in the exercise machine's space —
     * played with no machine in the scene it
     * drives the Sim's root to where it would be relative to a bench that
     * is not there, and the Sim tumbles through empty air with every limb
     * perfectly sensible. That is not a pose gone wrong; it is a pose
     * missing its other half.
     *
     * This is the naming convention and not the format talking, so it is a
     * rule about which animation to choose rather than a claim about what
     * one contains — and it is skipped out loud. */
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
        /* Against the model's own size, because a displacement means
           nothing on its own. This is the number that says whether what is
           on screen is a pose or the spike. */
        stringAppend(message, sizeof(message), "; it moved by ");
        appendThousandths(message, sizeof(message), discSearch.poseShift);
        stringAppend(message, sizeof(message), " against a model ");
        appendThousandths(message, sizeof(message), discSearch.poseSpan);
        stringAppend(message, sizeof(message), " across");
        platformLogMessage(message);

        /* The verdict, but only for the rest pose — it is the only
           animation whose right answer is known ahead of time. For any
           other the displacement above is a number with nothing to compare
           it against, and saying more than that would be inventing a
           standard. */
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

        /* Per bone the mesh actually draws with, how much of its chain the
           animation reached against how much was applied. A chain named far
           more than it is applied is posed in part, and a mesh posed in part
           is dragged against itself — which is what a torn face looks like,
           and what a displacement measured on the bounds cannot see. */
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
        /* The vertices have moved; the mesh is otherwise the one already
           uploaded, so only they are sent.
           
           renderSetMesh here would start by releasing everything the last
           mesh took — which includes every per-part texture. The Sim was
           correctly painted right up until the first pose landed, and then
           reverted to wearing one texture over all three parts, because
           this line threw them away. The body went back to wearing a face:
           a band across the arm and a patch at the waist, the same
           artefacts per-part texturing had just removed. */
        renderUpdateMeshVertices(&discSearch.mesh, globalArena);
        /* And from here the frame loop keeps it moving. Only now, because
           until a pose has succeeded once there is nothing to advance and
           a frame that tried would pay for a palette every frame to
           discover it. */
        if (posedAnimation.durationTicks > 0U)
        {
            poseIsAnimated = BOOLEAN_TRUE;
            platformLogMessage("engine: playing it from here, re-skinned each frame on the "
                               "processor from the bind pose kept aside");
            /* And last of all, what the catalogue said a Sim can be made
               of.
             *
               It is read long before this, but the animation search prints
               a line per candidate and there are eleven thousand of them to
               pick through — so by the time a load finishes, the catalogue
               has scrolled out of any console anyone would copy. Three logs
               in a row came back without it. Repeating it here costs
               nothing and puts the answer where the log ends. */
            reportCatalogueSlots(BOOLEAN_TRUE);
        }
        else
        {
            /* The rest pose has no duration — it is one pose, which is
               exactly why it can be checked. Having checked with it, the
               search carries on for something with a length to play, or
               the clock work would never have anything to move. */
            platformLogMessage("engine: that one has no duration to play over — keeping its "
                               "verdict and looking for an animation with a length");
            return ENGINE_DISC_WORKING;
        }
    }
    else
    {
        /* Read but did not move anything. Almost always the animation and
           the model naming their bones differently, which is a miss and not
           a failure — said plainly so it is not read as one. */
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
    /* Exactly one read per step. Two would alternate forever on a store that
       answers PENDING, which is a lesson this file has already paid for. */
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
        /* Found after scanning this many, which says whether skinned meshes
           are everywhere or rare enough that the next search needs to know
           exactly where to look. */
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: found after opening ");
        appendCount(message, sizeof(message), skinScanned);
        stringAppend(message, sizeof(message), " container(s)");
        platformLogMessage(message);

        /* Read that package properly rather than stopping at having found
           it. The probe opened a container on its own, which is enough to
           answer where skinning lives and not enough to draw: a model needs
           the shape that names its materials and the tree that places it,
           and those come from the same walk everything else goes through.
         *
           Its file index is kept before the arena is given back — the entry
           it came from sits below this marker and survives, but reading a
           pointer after rewinding it is a habit worth not having. */
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

    return finishOrSeekSkin();
}

/* Says what the disc's large non-package files actually are, before any of
   the work that assumes the answer is "nothing that matters". Sixteen bytes
   apiece; the phase exists at all because on the web even sixteen bytes have
   to go back to the event loop. */
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
        if (discSearch.largestArenaWant > 0UL)
        {
            stringAppend(message, sizeof(message), " largest allocation wanted ");
            appendCount(message, sizeof(message), (Unsigned32)discSearch.largestArenaWant);
            stringAppend(message, sizeof(message), " bytes;");
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
        /* A search that was pointed at one package and came away with
           nothing is not a failed load. Something is already drawn and
           uploaded — this was an attempt to draw something better — so the
           engine keeps what it has and says why, rather than reporting a
           disc it read perfectly well as unreadable. */
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

    /* What the search turned down before it settled. A run that took the
       first model it met and a run that walked past forty rigid ones to get
       here look identical otherwise. */
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

    /* What the mesh is weighted to, or that it is weighted to nothing.
       A face is rigid — it hangs off one joint whole — and prints the
       second, which is not a failure to read and should not look like
       one. */
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

    /* What the mesh is weighted to, and that nothing was moved by it.
       Skinning a mesh at rest is the identity — the mesh is already in the
       pose its bones were measured in — so this says what is ready rather
       than claiming a pose was applied. */
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

            /* The bones a primitive named. These are identifiers its nodes
               carry, not positions in the node list, so the line below says
               how many of them a node actually answered to. */
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

        /* How the bone numbers resolved, and what the container's own bind
           pose turned out to be. Both directions are printed because the
           number that matters is whichever one is near nought, and a reader
           who is told only the winner has to take the comparison on trust. */
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

    /* Element kinds the reader met and did not use, by name where the
       format's own table has one. A number alone sends the next reader
       looking it up; the name says at once whether what was passed over
       was a morph target or a second colour set. */
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
    return finishOrSeekSkin();
}
return discLoadStatus;
}

/* Which phase of the load gets this step.
 *
 * This was two thousand lines with every phase written out inside it, which is
 * how it got there: each capability added since the bootstrap needed a turn of
 * this loop, and another branch here was always the cheapest way to get one.
 * The phases are unchanged and still share their state; what has moved is only
 * where each one is written down, so that reading any of them no longer means
 * scrolling past the other nine. */
EngineDiscLoadStatus engineStepDiscLoad(void)
{
    if (discLoadStatus != ENGINE_DISC_WORKING)
    {
        return discLoadStatus;
    }

    /* Before any of the phases, and outside them.
     *
     * A font is not part of assembling a Sim and does not belong in the machine
     * that does it — but it does have to be found by reading a disc that may
     * answer later, so it needs a turn of the same loop. Taking one step here
     * and returning gives it exactly that, once, without any phase having to
     * know it exists. */
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
    /* Stops at the animation listing rather than running through it.
     *
     * That phase is thousands of reads and none of them changes what is on
     * screen — it is names for a menu. Spinning through it here would hold the
     * window shut while a Sim that is already assembled and posed waits behind
     * it. The platform goes on stepping the load every frame, so the list fills
     * with something to look at. */
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
    /* Early, and before anything that might fail: the font the engine carries
       with it is what lets a later failure be reported in words on the screen
       rather than only to a terminal nobody is looking at. */
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
    /* Sized for the resources a menu asks for over and over — an entry, its key
       list, a shape — rather than for a texture. What does not fit is read the
       way it always was, and the count of those is reported. */
    if (!resourceCacheBegin(&resourceCache, globalArena, 64U, 64UL * 1024UL))
    {
        platformLogMessage("engine: no room for a resource cache, so every read goes to the "
                           "disc — slower, and correct");
    }
    debugMenuBindPage(&debugMenu, DEBUG_MENU_PAGE_BODY, menuBodyRows, MENU_BODY_CAPACITY);
    /* From the arena rather than a static: five hundred names is thirty
       kilobytes, and this engine counts them. */
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

    /* After the renderer, because the backend may want to upload what it is
       given, and before the first frame so nothing is drawn twice. */
    loadDiscContent(configuration->fileSystem);

    engineIsRunning = BOOLEAN_TRUE;
    platformLogMessage("engine: initialized");
    logMemoryBudget();
    return BOOLEAN_TRUE;
}

/* Declared before both of its callers: a keystroke chooses and so does a click,
   and the two live either side of it. */
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
        /* Moving the cursor and choosing, in that order and both at once. A
           click that only moved the cursor would need a second click to act,
           which is not what a tile in a grid means anywhere else. */
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
    /* The menu sizes itself against the window: a fraction of it, capped, so a
       debug menu never fills the screen it is describing. */
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

/* Moves the model to where the animation has it now.
 *
 * Skinned on the processor rather than blended on the graphics one, which the
 * note on geometryMeshApplySkin would rather it were. The device ladder decides
 * it: the floor has no programmable shading at all, and the software rasterizer
 * is the expected backend down there, so a processor path is needed whatever
 * the top of the ladder eventually does.
 *
 * Every frame rebuilds the palette and re-skins from the bind pose kept aside,
 * so nothing here accumulates. It also re-sends the vertices, which is the real
 * cost of doing it this way and is why this is measured as its own zone rather
 * than hidden inside the frame.
 *
 * Silent when no animation was found, which is every disc that got this far
 * without a skinned mesh. */
static void advanceThePose(Real32 elapsedSeconds)
{
    Real32 tick;

    if (!poseIsAnimated || posedAnimation.durationTicks == 0U)
    {
        return;
    }

    VICTORIA_PROFILE_ZONE_BEGIN("advanceThePose");
    /* The clock handed in is time since the run began, not time since the last
     * frame, so the tick is computed from it rather than accumulated into it.
     * That also means a dropped frame skips ahead instead of slowing the
     * animation down, and a long run cannot drift.
     *
     * Seconds become ticks through the format's own constant. The wrap is a
     * truncated division rather than repeated subtraction, which at eight
     * hundred ticks a second would be thousands of iterations an hour into a
     * run; there is no floating point modulus to call freestanding. */
    if (poseIsHeldStill == BOOLEAN_TRUE)
    {
        /* Held, but still rebuilt every frame. Skipping the pose entirely would
           leave the mesh wherever the deformation last put it, since the two
           share the one restore — so the animation is re-applied to the same
           tick rather than not applied. */
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

    /* One channel at a time, swung from rest to full and back over four
     * seconds, then on to the next.
     *
     * This began as every channel at once, which was a mistake worth recording:
     * nine of a face's channels drive the mouth, so at full strength they fight
     * over the same vertices and what arrives is a mangled mouth that says
     * nothing about whether any single channel is right. A caricature is not an
     * instrument. One named channel at a time can be checked against its own
     * name, which is the only thing here that can be checked at all.
     *
     * Only the channels that move a vertex far enough to see, because a window
     * showing a face that does not change reads as the deformation having
     * broken rather than as a channel with nothing in it. */
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

        /* The vertices moved; the mesh is otherwise the one already uploaded.
           renderSetMesh here would charge the graphics ledger for a second
           buffer every frame and compile the program again with it. */
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

/* Acts on the row the cursor is on.
 *
 * Its own function because two things choose now — a keystroke and a click on a
 * tile — and a menu where clicking and pressing enter do subtly different
 * things is a menu nobody can describe. */
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
                /* The skeleton name is composed and may not be on this disc —
                   an elder has none — so it is settled again against the index
                   that is already built rather than left to fail silently. */
                settleTheSkeleton();
            }
            break;

        case DEBUG_MENU_PAGE_ANIMATION:
            if (row < menuAnimationCount && menuAnimationEntries[row] != NULL_POINTER)
            {
                simWardrobeAnimationWanted = menuAnimationEntries[row];
                debugMenuSetInEffect(&debugMenu, DEBUG_MENU_PAGE_ANIMATION, row);
                /* Applied by the step machine rather than here: reading it is a
                   read, and a read is something a browser answers later. */
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
                /* The whole assembly again, because the wardrobe is decided
                   during the catalogue walk and nowhere else. The index is
                   kept, so this costs a second rather than another walk of the
                   disc. */
                restartTheAssembly();
                settleTheSkeleton();
            }
            break;

        default:
            break;
        }
    }
}

/* Read a little-endian 32-bit value from a byte buffer. */
static Unsigned32 catalogRead32(const Unsigned8 *p, MemorySize off)
{
    return (Unsigned32)p[off]           |
           ((Unsigned32)p[off + 1U] << 8)  |
           ((Unsigned32)p[off + 2U] << 16) |
           ((Unsigned32)p[off + 3U] << 24);
}

/* Look up a prerendered thumbnail instance for a SKIN_ENTRY key.
 *
 * Each catalog entry is 104 bytes (plus 8-byte header):
 *   bytes  8-11  src_grp
 *   bytes 12-15  src_inst
 *   bytes 28-31  thumb_inst  (the JPEG instance to load)
 *
 * Returns the JPEG instance ID, or 0 if not found. */
static Unsigned32 catalogFindThumbInst(Unsigned32 srcGrp, Unsigned32 srcInst)
{
    Unsigned32 i;
    const Unsigned8 *base;

    if (bstCatalogData == NULL_POINTER)
    {
        return 0U;
    }
    base = bstCatalogData + 8U; /* skip 8-byte header */
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

/* Nearest-neighbour downscale from an arbitrary RGBA source to the thumbnail
 * pixel array. Used only during thumbnail decode; not worth a general routine. */
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

/* One hop of thumbnail loading.
 *
 * Returns TRUE when the hop completed and the caller may try again immediately
 * (native, where reads never pend). Returns FALSE when either there is nothing
 * more to load, or a VFS read is pending and the caller must wait for delivery
 * before calling again (web). */
static Boolean stepOneThumbnail(void)
{
    if (engineIsRunning == BOOLEAN_FALSE || menuClothingCount == 0U)
    {
        return BOOLEAN_FALSE;
    }

    /* One-time: load the BodyShopThumbnails catalog index.
     *
     * The allocation is intentionally NOT rewound on success — the catalog
     * lives permanently in the arena below any per-frame markers. */
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
                /* No catalog on this disc — thumbnail loading is unavailable. */
                bstCatalogLoading = BOOLEAN_TRUE; /* suppress repeated searches */
                return BOOLEAN_FALSE;
            }
            thumbnailLoadEntry = catEntry;
            bstCatalogLoading  = BOOLEAN_TRUE;
        }

        if (thumbnailLoadEntry == NULL_POINTER)
        {
            return BOOLEAN_FALSE; /* gave up above */
        }

        catMarker = memoryArenaGetMarker(globalArena);
        if (!readIndexedResource(thumbnailLoadEntry, &bytes, &size))
        {
            /* Pending — rewind the scratch allocation and retry next frame. */
            memoryArenaRewindToMarker(globalArena, catMarker);
            return BOOLEAN_FALSE;
        }
        if (bytes != NULL_POINTER && size > 8U)
        {
            Unsigned32 ver = catalogRead32(bytes, 0U);
            Unsigned32 cnt = catalogRead32(bytes, 4U);
            if (ver == 2U && cnt > 0U && cnt <= 65536U)
            {
                /* Keep the decompressed catalog in the arena permanently. */
                bstCatalogData  = bytes;
                bstCatalogCount = cnt;
            }
        }
        if (bstCatalogData == NULL_POINTER)
        {
            /* Parse failed — rewind so the wasted space is reclaimed. */
            memoryArenaRewindToMarker(globalArena, catMarker);
        }
        /* Either way, don't search for the catalog entry again. */
        thumbnailLoadEntry = NULL_POINTER;
        return BOOLEAN_TRUE;
    }

    if (thumbnailHop == THUMBNAIL_HOP_IDLE)
    {
        /* Find the next row that has a name and no cache slot yet. */
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

            /* Check whether this row already has a slot. */
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

            /* Resolve: name → SKIN_ENTRY → catalog → JPEG entry. */
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

            /* Assign a free slot and start the load. */
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
            return BOOLEAN_FALSE; /* All slots occupied. */
        }
        return BOOLEAN_FALSE; /* All loadable rows covered. */
    }

    /* THUMBNAIL_HOP_JPEG: read the JPEG, decode, scale to slot pixels. */
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
            /* 256×256×4 = 262 144 bytes for the decoded RGBA. */
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
            /* Decode failed — free the slot so another row can use it. */
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
    /* On native, reads never pend, so loop to complete multiple hops per call.
       Cap at 8 to bound the worst-case stall per frame. */
    Unsigned32 iterations;

    for (iterations = 0U; iterations < 8U; iterations++)
    {
        if (!stepOneThumbnail())
        {
            break;
        }
    }
}

void engineRenderFrame(Real32 elapsedSeconds)
{
    if (engineIsRunning == BOOLEAN_FALSE)
    {
        return;
    }

    VICTORIA_PROFILE_ZONE_BEGIN("engineRenderFrame");
    engineStepThumbnail();
    advanceThePose(elapsedSeconds);
    /* Before the frame rather than after: what the menu says can change on any
       step of the disc load, and composing it afterwards would put it on screen
       one frame late — which on the frame somebody presses a key is the frame
       they are looking at. */
    engineTextDraw(&debugMenu, engineGetMenuText());
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
