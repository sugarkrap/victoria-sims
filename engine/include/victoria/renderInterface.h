#ifndef VICTORIA_RENDER_INTERFACE_HEADER
#define VICTORIA_RENDER_INTERFACE_HEADER

#include "victoria/coreTypes.h"
#include "victoria/geometryReader.h"
#include "victoria/memoryArena.h"

/* Implemented once per graphics backend: OpenGL ES 2.0 for Linux and ARMv5,
   WebGPU for WebAssembly. */

/* Best-effort total graphics memory, in bytes, or zero when the backend cannot
   tell us — which is the common case, since neither OpenGL ES 2.0 nor WebGPU
   has a portable way to ask. Called before renderInitialize. */
MemorySize renderQueryGraphicsMemoryBytes(void);

Boolean renderInitialize(MemoryArena *arena, Unsigned32 widthInPixels, Unsigned32 heightInPixels);
void renderResize(Unsigned32 widthInPixels, Unsigned32 heightInPixels);
/* Hands the backend a mesh to draw instead of the placeholder triangle. Null
   goes back to the triangle, which is what a build with no disc still shows.
   The mesh and its arrays must outlive the backend; they come from the arena,
   so they do.

   Called after renderInitialize, because a backend may want to upload it. */
void renderSetMesh(const GeometryMesh *mesh, MemoryArena *arena);

void renderDrawFrame(Real32 elapsedSeconds);
void renderShutdown(void);

/* Number of shader programs built during initialisation. Reported so a stall
   at startup can be attributed. */
Unsigned32 renderGetShaderProgramCount(void);

#endif
