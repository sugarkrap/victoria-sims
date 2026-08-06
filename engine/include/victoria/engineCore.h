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

    /* Opens the debug menu straight away instead of waiting for somebody to
     * press m.
     *
     * Which is a smaller thing than it sounds: a menu is the one part of an
     * engine that cannot be judged from a log, and a machine with no way to
     * synthesise a keystroke — a headless capture, a continuous integration
     * runner, this one — otherwise has no way to see it at all. `--menu` is how
     * to ask from the command line. */
    Boolean menuIsOpen;
    /* Which page it opens on: 0 body, 1 clothing, 2 animation. Only read when
       menuIsOpen, and `--menu=clothing` is how to ask. */
    Unsigned32 menuPage;
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

/* The debug menu, displayed the way the profiler report is: the engine formats
 * plain text and the platform shows it — printed on Linux, put in an element on
 * the web. Nothing is drawn on screen, which is what makes it work on the
 * backend at the floor of the device ladder that has no shaders at all.
 *
 * Never null. Says how to open the menu even while it is shut, because a debug
 * feature nobody can discover is a debug feature nobody has. */
const char *engineGetMenuText(void);

/* One keystroke. Returns whether anything changed, so a platform knows whether
 * to redraw rather than reprinting a menu on every key that did nothing.
 *
 * Choosing a different Sim restarts the assembly, which the platform drives by
 * going on calling engineStepDiscLoad — the index is kept, so it costs a second
 * rather than another walk of the disc. */
Boolean engineHandleMenuKey(char key);

/* What a pointer did. Three actions rather than a position and a button flag:
 * a click is not a move that happens to have a button held, and a pointer that
 * has left the window is not a pointer at (0, 0) — which is a corner of the
 * menu, and would light a button up every time somebody moved the mouse off the
 * window. */
typedef enum EnginePointerAction
{
    ENGINE_POINTER_MOVED = 0,
    ENGINE_POINTER_PRESSED,
    ENGINE_POINTER_LEFT
} EnginePointerAction;

/* One pointer event, in window pixels from the top left. Returns whether
 * anything changed, so a platform knows whether to redraw.
 *
 * The engine decides whether the point is over anything, so a platform never
 * has to know where the menu is — which is what keeps the same interface
 * working on a backend with shaders and one without. A press that lands on
 * nothing is answered as nothing rather than passed on to the world behind,
 * because there is nothing behind it to pass to yet. */
Boolean engineHandlePointer(EnginePointerAction action, Integer32 x, Integer32 y);

MemoryArena *engineGetGlobalArena(void);

#endif
