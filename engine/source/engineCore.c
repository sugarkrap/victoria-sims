#include "victoria/discContent.h"
#include "victoria/discReader.h"
#include "victoria/engineCore.h"
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
static const char *const simPartNames[SIM_PART_COUNT] = { "auskel_cres", "amBodyNaked_cres",
                                                          "amFace_cres", "amHairBald_cres" };
static ResourceIndex simIndex;
static ResourceNodeDescription simPartTree;
static Boolean simIndexBegun = BOOLEAN_FALSE;
static Boolean simIndexBuilding = BOOLEAN_FALSE;
static Unsigned32 simPartCursor = 0U;
static Unsigned32 simPartsFound = 0U;
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
    SIM_HOP_FINISHED
} SimHop;

static SimHop simHop = SIM_HOP_TREE;
static Unsigned32 simHopPart = 0U;
static Unsigned32 simGathered = 0U;
static ResourceNodeDescription simHopTree;
static ShapeDescription simHopShape;
static const ResourceIndexEntry *simHopEntry = NULL_POINTER;
static TextureDescription simHopTexture;
static char simHopTextureName[RESOURCE_NAME_LIMIT];
static Boolean simHopOverrode = BOOLEAN_FALSE;

/* The three that carry geometry. The skeleton names no shape at all — it is
   what the others hang on — so it is probed for and then not drawn. */
#define SIM_DRAWN_PART_COUNT 3U
static const char *const simDrawnPartNames[SIM_DRAWN_PART_COUNT] = { "amBodyNaked_cres",
                                                                     "amFace_cres",
                                                                     "amHairBald_cres" };
static GeometryMesh simParts[SIM_DRAWN_PART_COUNT];

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
static char simPartMaterials[SIM_DRAWN_PART_COUNT][RESOURCE_NAME_LIMIT];

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
static const char *const simPartTextureStems[SIM_DRAWN_PART_COUNT] = { "", "amface", "" };
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
        stringAppend(message, sizeof(message), " delta set(s) over a map read for ");
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
    stringAppend(message, sizeof(message), "engine:   part ");
    appendCount(message, sizeof(message), simHopPart);
    stringAppend(message, sizeof(message), " painted with ");
    stringAppend(message, sizeof(message), simHopTextureName);
    if (simHopOverrode)
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
#define CATALOGUE_SAMPLE_LIMIT 600U
#define CATALOGUE_KIND_LIMIT 16U
static Unsigned32 catalogueCursor = 0U;
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

/* The sidecar read: an entry's shape index against the key list beside it.
 *
 * A step of its own because it is a second read, and one read per step is the
 * rule a browser's store enforces. The entry that wanted it is gone by now, so
 * what it needed — its instance words, its shape index, its name — was kept.
 *
 * The sidecar is matched on instance alone, the index not carrying groups. A
 * catalogue entry and its key list share both, so this is right wherever two
 * types do not collide on an instance, which is everywhere seen so far. Said
 * out loud because it is an assumption and not a guarantee. */
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
                    stringAppend(message, sizeof(message),
                                 (key->typeIdentifier == (Unsigned32)PACKAGE_TYPE_SHPE)
                                     ? "shape"
                                     : "resource of another type");
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

static SimAssembly stepTheCatalogue(MemorySize marker)
{
    const ResourceIndexEntry *entry = NULL_POINTER;
    Unsigned8 *bytes;
    MemorySize size;

    if (catalogueWantsKeyList == BOOLEAN_TRUE)
    {
        return stepTheSidecar(marker);
    }

    while (catalogueCursor < simIndex.count &&
           simIndex.entries[catalogueCursor].typeIdentifier !=
               (Unsigned32)PACKAGE_TYPE_SKIN_ENTRY)
    {
        catalogueCursor++;
    }
    if (catalogueCursor >= simIndex.count || catalogueRead >= CATALOGUE_SAMPLE_LIMIT)
    {
        reportCatalogue();
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

/* The painting hops: a material, then its texture, then the top level that
   texture kept somewhere else. One read each, like everything else here. */
static SimAssembly stepThePaint(MemorySize marker)
{
    char wanted[RESOURCE_NAME_LIMIT];
    Unsigned8 *bytes;
    MemorySize size;

    if (simHopPart >= simGathered)
    {
        simHop = SIM_HOP_CATALOGUE;
        catalogueCursor = 0U;
        catalogueRead = 0U;
        catalogueKindCount = 0U;
        return SIM_ASSEMBLY_PENDING;
    }

    if (simHop == SIM_HOP_MATERIAL)
    {
        if (simPartMaterials[simHopPart][0] == '\0')
        {
            simHopPart++;
            return SIM_ASSEMBLY_PENDING;
        }
        if (simHopEntry == NULL_POINTER)
        {
            materialBuildResourceName(wanted, sizeof(wanted), simPartMaterials[simHopPart],
                                      "_txmt");
            simHopEntry = resourceIndexFindNamed(&simIndex, (Unsigned32)PACKAGE_TYPE_TXMT, wanted);
            if (simHopEntry == NULL_POINTER)
            {
                simHopEntry = resourceIndexFindNamed(&simIndex, (Unsigned32)PACKAGE_TYPE_TXMT,
                                                     simPartMaterials[simHopPart]);
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

            if (bytes != NULL_POINTER && materialRead(&material, bytes, size) == MATERIAL_READ_OK &&
                material.baseTextureName[0] != '\0')
            {
                stringAppend(simHopTextureName, sizeof(simHopTextureName),
                             material.baseTextureName);
            }
        }
        memoryArenaRewindToMarker(globalArena, marker);
        if (simHopTextureName[0] == '\0')
        {
            simHopPart++;
            return SIM_ASSEMBLY_PENDING;
        }
        /* The override, when this part has one and the disc carries it. */
        if (simPartTextureStems[simHopPart][0] != '\0' && simSkinTone[0] != '\0')
        {
            char preferred[RESOURCE_NAME_LIMIT];

            preferred[0] = '\0';
            stringAppend(preferred, sizeof(preferred), simPartTextureStems[simHopPart]);
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
        if (simHopPart >= SIM_DRAWN_PART_COUNT)
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
            geometryReaderOpen(&simParts[simGathered], bytes, size, globalArena) !=
                GEOMETRY_READ_OK)
        {
            reportSimPart(simHopPart, DISC_MODEL_GEOMETRY_UNREADABLE);
            simHopPart++;
            simHop = SIM_HOP_TREE;
            return SIM_ASSEMBLY_PENDING;
        }
        /* Which material this part wears, matched by the primitive's own name.
           A shape binds by name and lists more materials than the part has
           parts — a face shape carries the face, the brows, the eyes and the
           lips — so taking the first painted a Sim's face with an eyebrow. Both
           names must be real: two blanks compare equal and that is not a
           match. */
        simPartMaterials[simGathered][0] = '\0';
        if (simParts[simGathered].storedPrimitiveCount > 0U)
        {
            for (index = 0U; index < simHopShape.storedMaterialCount; index++)
            {
                if (simParts[simGathered].primitives[0].name[0] == '\0' ||
                    simHopShape.materials[index].primitiveName[0] == '\0')
                {
                    continue;
                }
                if (stringEqualsIgnoringCase(simHopShape.materials[index].primitiveName,
                                             simParts[simGathered].primitives[0].name))
                {
                    stringAppend(simPartMaterials[simGathered], RESOURCE_NAME_LIMIT,
                                 simHopShape.materials[index].materialName);
                    break;
                }
            }
        }
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine:   ");
        stringAppend(message, sizeof(message), simDrawnPartNames[simHopPart]);
        stringAppend(message, sizeof(message), " — ");
        appendCount(message, sizeof(message), simParts[simGathered].vertexCount);
        stringAppend(message, sizeof(message), " vertices, ");
        appendCount(message, sizeof(message), simParts[simGathered].indexCount / 3U);
        stringAppend(message, sizeof(message), " triangles, ");
        appendCount(message, sizeof(message), simParts[simGathered].skinnedVertexCount);
        stringAppend(message, sizeof(message), " of them weighted, wearing ");
        stringAppend(message, sizeof(message), (simPartMaterials[simGathered][0] != '\0')
                                                   ? simPartMaterials[simGathered]
                                                   : "no material it names");
        platformLogMessage(message);
        reportMorphTargets(&simParts[simGathered]);
        simGathered++;
        simHopPart++;
        simHop = SIM_HOP_TREE;
        return SIM_ASSEMBLY_PENDING;

    case SIM_HOP_MERGE:
    {
        const GeometryMesh *parts[SIM_DRAWN_PART_COUNT];

        if (simGathered == 0U)
        {
            return SIM_ASSEMBLY_FAILED;
        }
        for (index = 0U; index < simGathered; index++)
        {
            parts[index] = &simParts[index];
        }
        if (geometryMeshMerge(&whole, parts, simGathered, globalArena) != GEOMETRY_READ_OK)
        {
            platformLogMessage("engine: a Sim's parts would not join into one model");
            return SIM_ASSEMBLY_FAILED;
        }
        message[0] = '\0';
        stringAppend(message, sizeof(message), "engine: a whole Sim — ");
        appendCount(message, sizeof(message), simGathered);
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
        static const Unsigned32 wantedTypes[9] = { (Unsigned32)PACKAGE_TYPE_CRES,
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
                                                   (Unsigned32)PACKAGE_TYPE_RESOURCE_KEY_LIST };

        simIndexBegun = BOOLEAN_TRUE;
        if (resourceIndexBegin(&simIndex, discFileSystem, globalArena, SIM_INDEX_CAPACITY,
                               wantedTypes, 9U))
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

EngineDiscLoadStatus engineStepDiscLoad(void)
{
    /* Wide enough for every refusal reason at once. A truncated diagnostic is
       worse than none: it looks complete. */
    char message[512];

    if (discLoadStatus != ENGINE_DISC_WORKING)
    {
        return discLoadStatus;
    }

    if (discPhase == DISC_PHASE_SEEK_SIM)
    {
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
            /* Every part named a package, and on this disc it was the same one
               each time, so the whole Sim is one file read. */
            if (simPartsFound == SIM_PART_COUNT)
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

    if (discPhase == DISC_PHASE_SEEK_ANIMATION)
    {
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
                discPhase = DISC_PHASE_DONE;
                discLoadStatus = ENGINE_DISC_READY;
                return discLoadStatus;
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

        discPhase = DISC_PHASE_DONE;
        discLoadStatus = ENGINE_DISC_READY;
        return discLoadStatus;
    }

    if (discPhase == DISC_PHASE_SEEK_SKIN)
    {
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
        return finishOrSeekSkin();
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

        return finishOrSeekSkin();
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
    if (simMorphMoverCount > 0U)
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

void engineRenderFrame(Real32 elapsedSeconds)
{
    if (engineIsRunning == BOOLEAN_FALSE)
    {
        return;
    }

    VICTORIA_PROFILE_ZONE_BEGIN("engineRenderFrame");
    advanceThePose(elapsedSeconds);
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
