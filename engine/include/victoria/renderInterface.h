#ifndef VICTORIA_RENDER_INTERFACE_HEADER
#define VICTORIA_RENDER_INTERFACE_HEADER

#include "victoria/coreTypes.h"
#include "victoria/geometryReader.h"
#include "victoria/memoryArena.h"

MemorySize renderQueryGraphicsMemoryBytes(void);

Boolean renderInitialize(MemoryArena *arena, Unsigned32 widthInPixels, Unsigned32 heightInPixels);
void renderResize(Unsigned32 widthInPixels, Unsigned32 heightInPixels);
void renderSetMesh(const GeometryMesh *mesh, MemoryArena *arena);

void renderUpdateMeshVertices(const GeometryMesh *mesh, MemoryArena *arena);

#define RENDER_PART_LIMIT 16U

void renderSetPartTexture(Unsigned32 partIndex, const Unsigned8 *rgbaPixels,
                          Unsigned32 widthInPixels, Unsigned32 heightInPixels);

void renderSetTexture(const Unsigned8 *rgbaPixels, Unsigned32 widthInPixels,
                      Unsigned32 heightInPixels);

#define RENDER_CAMERA_ORBIT_DEFAULT 0.6f

void renderSetCameraOrbitRate(Real32 radiansPerSecond);

#define RENDER_CAMERA_FRONT (3.14159265358979323846f)
void renderSetCameraAngle(Real32 radians);

void renderSetOverlay(const Unsigned8 *pixels, Unsigned32 widthInPixels,
                      Unsigned32 heightInPixels);

void renderDrawFrame(Real32 elapsedSeconds);
void renderShutdown(void);

Unsigned32 renderGetShaderProgramCount(void);

#endif
