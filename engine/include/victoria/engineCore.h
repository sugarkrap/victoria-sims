#ifndef VICTORIA_ENGINE_CORE_HEADER
#define VICTORIA_ENGINE_CORE_HEADER

#include "victoria/coreTypes.h"
#include "victoria/memoryArena.h"
#include "victoria/virtualFileSystem.h"

typedef struct EngineConfiguration
{
    Unsigned32 widthInPixels;
    Unsigned32 heightInPixels;
    /* Zero asks the backend what it has and falls back to the conservative
       default; anything else overrides both. */
    MemorySize graphicsMemoryLimitBytes;

    /* Where the game's data is, or null when none was given — in which case the
       engine starts and draws its placeholder, which is what every build did
       before there was a disc to read. The platform opens the store; the engine
       does not know whether it is an image, a folder, or a browser's File.

       An empty catalogue means "walk it as a disc image"; a catalogue that is
       already filled is taken as it is, which is how a folder arrives. */
    VirtualFileSystem *fileSystem;

    /* Holds the camera still instead of orbiting the model.
     *
     * For comparing frames by eye, which is the one thing an orbiting camera
     * makes impossible: two captures a moment apart are two different views,
     * and the difference gets attributed to whatever changed in the code. That
     * has already cost a round of work here once. The orbit stays the default
     * because a still model hides its own silhouette. */
    Boolean cameraIsStill;
    /* Where to hold it, in degrees. Only read when cameraIsStill. Nought is
       behind a Sim, so this defaults to half a turn at the caller. */
    Real32 cameraAngleDegrees;

    /* Holds the animation on one frame instead of playing it.
     *
     * The companion to cameraIsStill, and for the same reason: with the camera
     * fixed and the pose fixed, anything still moving on screen is the one
     * thing being looked at. Judging a deformation against a model that is also
     * walking is judging two motions at once. */
    Boolean poseIsHeld;
    /* Which tick to hold it on. Only read when poseIsHeld. */
    Real32 poseHeldTick;

    /* Hold one deformation channel at full strength instead of sweeping.
     *
     * Nought sweeps, which is the default and is right for finding out what a
     * model can do. A number is right for judging one shape against another:
     * with the camera and the pose held too, three runs at nought, one and two
     * give three frames that differ in exactly one thing. */
    Unsigned32 heldMorphChannel;

    /* What to dress the Sim in, matched against any part of a catalogue entry's
     * name, or null for whatever the catalogue walk offers first.
     *
     * The walk samples two thousand entries out of thousands and takes the
     * first of each slot that fits, which is one garment out of hundreds. This
     * is how a run looks at any of the others — the log names every entry the
     * wardrobe was offered, so a name to try comes out of the previous run.
     *
     * It does not override the rule that a mesh must be authored for the age
     * and gender the skeleton is: asking for a child's garment on an adult
     * still refuses, and says which rule refused it. */
    const char *wornName;

    /* Which Sim to build: an age and a gender, as the catalogue spells them.
     * "am" is an adult male, "af" an adult female, "cm" a child male, and so
     * on through t for teen, e for elder, p for toddler and b for baby.
     *
     * Null keeps the adult male the engine has always built. It is not a
     * cosmetic choice: every one of the four names a Sim is assembled from is
     * composed from this, the skeleton is chosen by the age half of it, and the
     * wardrobe refuses anything the disc authored for somebody else — so this
     * is the difference between reaching a fifth of the catalogue and reaching
     * the rest of it. */
    const char *simArchetype;
} EngineConfiguration;

Boolean engineInitialize(const EngineConfiguration *configuration);
void engineResize(Unsigned32 widthInPixels, Unsigned32 heightInPixels);
void engineShutdown(void);

/* The platform layer owns the frame boundary, so work it does outside the
   engine — pumping events, presenting — still lands inside the profiled frame.
   Every frame must be bracketed by exactly one begin and one end. */
void engineBeginFrame(void);
void engineRenderFrame(Real32 elapsedSeconds);
void engineEndFrame(void);

/* Latest profiler report, regenerated a few times a second. Never null;
   returns an empty string when profiling is unavailable. */
const char *engineGetProfilerReportText(void);

/* Where a disc load has got to. */
typedef enum EngineDiscLoadStatus
{
    ENGINE_DISC_IDLE = 0,
    /* Still walking or still waiting on the host. Step again next frame. */
    ENGINE_DISC_WORKING,
    ENGINE_DISC_READY,
    ENGINE_DISC_FAILED
} EngineDiscLoadStatus;

/* Starts reading whatever the platform has opened. Safe to call after
   engineInitialize, and the only way in on a platform whose reads cannot be
   answered on the spot. */
void engineBeginDiscLoad(VirtualFileSystem *fileSystem);

/* Does one unit of work: one directory, or one package. Returns WORKING for as
   long as it wants calling again. A platform that can answer a read
   immediately may spin on this; a browser must call it once a frame. */
EngineDiscLoadStatus engineStepDiscLoad(void);

/* Logs what a catalogue holds: packages against everything else, and the
   largest of everything else by name. Called for itself during a disc load, and
   exposed because a platform may want to look at a disc without starting a
   renderer — and because "what is on this disc" is a question worth being able
   to ask on its own. */
void engineReportDiscCatalogue(const VirtualFileSystem *fileSystem);

MemoryArena *engineGetGlobalArena(void);

#endif
