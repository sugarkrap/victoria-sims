#ifndef VICTORIA_ENGINE_CORE_HEADER
#define VICTORIA_ENGINE_CORE_HEADER

#include "victoria/coreTypes.h"
#include "victoria/memoryArena.h"

Boolean engineInitialize(Unsigned32 widthInPixels, Unsigned32 heightInPixels);
void engineResize(Unsigned32 widthInPixels, Unsigned32 heightInPixels);
void engineRenderFrame(Real32 elapsedSeconds);
void engineShutdown(void);

MemoryArena *engineGetGlobalArena(void);

#endif
