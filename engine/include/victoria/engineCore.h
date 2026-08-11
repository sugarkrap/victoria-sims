#ifndef VICTORIA_ENGINE_CORE_HEADER
#define VICTORIA_ENGINE_CORE_HEADER

#include "victoria/coreTypes.h"
#include "victoria/memoryArena.h"
#include "victoria/virtualFileSystem.h"

typedef struct EngineConfiguration
{
    Unsigned32 widthInPixels;
    Unsigned32 heightInPixels;
    MemorySize graphicsMemoryLimitBytes;

    VirtualFileSystem *fileSystem;

    Boolean cameraIsStill;
    Real32 cameraAngleDegrees;

    Boolean poseIsHeld;
    Real32 poseHeldTick;

    Unsigned32 heldMorphChannel;

    const char *wornName;

    const char *simArchetype;

    Boolean menuIsOpen;
    Unsigned32 menuPage;
} EngineConfiguration;

Boolean engineInitialize(const EngineConfiguration *configuration);
void engineResize(Unsigned32 widthInPixels, Unsigned32 heightInPixels);
void engineShutdown(void);

void engineBeginFrame(void);
void engineRenderFrame(Real32 elapsedSeconds);
void engineEndFrame(void);

const char *engineGetProfilerReportText(void);

typedef enum EngineDiscLoadStatus
{
    ENGINE_DISC_IDLE = 0,
    ENGINE_DISC_WORKING,
    ENGINE_DISC_READY,
    ENGINE_DISC_FAILED
} EngineDiscLoadStatus;

void engineBeginDiscLoad(VirtualFileSystem *fileSystem);

EngineDiscLoadStatus engineStepDiscLoad(void);

void engineReportDiscCatalogue(const VirtualFileSystem *fileSystem);

const char *engineGetMenuText(void);

Boolean engineHandleMenuKey(char key);

typedef enum EnginePointerAction
{
    ENGINE_POINTER_MOVED = 0,
    ENGINE_POINTER_PRESSED,
    ENGINE_POINTER_LEFT
} EnginePointerAction;

Boolean engineHandlePointer(EnginePointerAction action, Integer32 x, Integer32 y);

MemoryArena *engineGetGlobalArena(void);

Boolean engineGetThumbnailPixels(Unsigned32 row, const Unsigned8 **rgbaPixels,
                                  Unsigned32 *width, Unsigned32 *height);
void engineStepThumbnail(void);

#endif
