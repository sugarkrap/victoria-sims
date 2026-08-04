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

/* Hands the backend the image the mesh is painted with: eight bit RGBA, red
   first, top row first, width * height * 4 bytes.

   Call it before renderSetMesh. A backend needs to know the vertex layout and
   the bindings before it builds a pipeline, and rebuilding one afterwards to
   add a texture is work nobody needs.

   Never calling it is not an error. A backend that has no image paints with
   white, so an untextured mesh comes out exactly as it did before there was
   any of this — which is also what a mesh with no texture coordinates gets,
   since sampling one without them would be worse than not sampling. */
void renderSetTexture(const Unsigned8 *rgbaPixels, Unsigned32 widthInPixels,
                      Unsigned32 heightInPixels);

void renderDrawFrame(Real32 elapsedSeconds);
void renderShutdown(void);

/* Number of shader programs built during initialisation. Reported so a stall
   at startup can be attributed. */
Unsigned32 renderGetShaderProgramCount(void);

#endif
