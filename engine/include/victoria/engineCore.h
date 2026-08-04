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
