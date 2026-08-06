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

/* Re-sends the vertices of a mesh already set, for a model whose vertices moved
 * but whose shape did not — one skinned on the processor each frame.
 *
 * Separate from renderSetMesh because that one rebuilds everything: it charges
 * the graphics ledger for a new buffer, compiles a program, and re-frames the
 * camera. Called every frame it leaks the ledger and recompiles a shader per
 * frame, which is what an animated mesh made it do.
 *
 * A backend that reads the mesh's arrays directly rather than copying them has
 * nothing to do here, and does nothing. Doing nothing is also the right answer
 * when no mesh is set or the vertex count has changed — that is a different
 * mesh, and renderSetMesh is what it wants. */
void renderUpdateMeshVertices(const GeometryMesh *mesh, MemoryArena *arena);

/* How many of a model's parts a backend will paint separately.
 *
 * The cap exists because each part costs a texture name held for the life of
 * the mesh, and a model that exceeds it draws its remaining parts with the last
 * texture rather than not at all — a part painted wrongly is visible and
 * diagnosable, a part missing looks like a hole in the model.
 *
 * Was eight, which was three times what an undressed Sim needed and not quite
 * enough for a dressed one. A part is a PRIMITIVE and not a body part: a
 * firefighter's suit is two, a helmet is three, and a Sim wearing a top, a
 * bottom, a face and a hair reaches eight on the garments alone. Sixteen has
 * headroom for that and costs four words a slot in each backend. */
#define RENDER_PART_LIMIT 16U

/* Gives one of the mesh's parts its own texture.
 *
 * partIndex is a position in the mesh's primitives array, which is what says
 * which range of indices the part owns. Called after renderSetMesh, once per
 * part that has a texture of its own; a part never given one is drawn with
 * whatever renderSetTexture last supplied, so a model with one texture behaves
 * exactly as it did before any of this existed.
 *
 * Passing null pixels releases that part's texture. */
void renderSetPartTexture(Unsigned32 partIndex, const Unsigned8 *rgbaPixels,
                          Unsigned32 widthInPixels, Unsigned32 heightInPixels);

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

/* How fast the camera goes round the model, in radians a second. */
#define RENDER_CAMERA_ORBIT_DEFAULT 0.6f

/* Sets that rate. Nought holds the camera still.
 *
 * This exists because of a specific and repeated mistake. Two screenshots of an
 * orbiting model are two different views, and comparing them attributes the
 * camera's motion to whatever changed in the code — a correctly posed head seen
 * from the side has no skull behind it and looks exactly like a torn one, which
 * cost a full round of misdirected work once already. Anything being judged by
 * eye across frames wants this at nought, and `--still-camera` is the way to
 * ask for it from the command line.
 *
 * The orbit is otherwise worth keeping: a still model hides everything about
 * its silhouette, and three of this engine's bugs were visible only in motion. */
void renderSetCameraOrbitRate(Real32 radiansPerSecond);

/* Where the camera starts, in radians. The orbit is added to this, so with the
 * rate at nought this is simply where the camera stays.
 *
 * Nought is behind a Sim — which is not a fact anyone could have predicted from
 * the code, and was found by holding the camera there and getting a back. Half
 * a turn is the front. */
#define RENDER_CAMERA_FRONT (3.14159265358979323846f)
void renderSetCameraAngle(Real32 radians);

void renderDrawFrame(Real32 elapsedSeconds);
void renderShutdown(void);

/* Number of shader programs built during initialisation. Reported so a stall
   at startup can be attributed. */
Unsigned32 renderGetShaderProgramCount(void);

#endif
