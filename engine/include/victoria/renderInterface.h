#ifndef VICTORIA_RENDER_INTERFACE_HEADER
#define VICTORIA_RENDER_INTERFACE_HEADER

#include "victoria/coreTypes.h"
#include "victoria/memoryArena.h"

/* Implemented once per graphics backend: OpenGL ES 2.0 for Linux and ARMv5,
   WebGPU for WebAssembly. */

Boolean renderInitialize(MemoryArena *arena, Unsigned32 widthInPixels, Unsigned32 heightInPixels);
void renderResize(Unsigned32 widthInPixels, Unsigned32 heightInPixels);
void renderDrawFrame(Real32 elapsedSeconds);
void renderShutdown(void);

#endif
