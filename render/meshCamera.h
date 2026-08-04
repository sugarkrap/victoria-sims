#ifndef VICTORIA_MESH_CAMERA_HEADER
#define VICTORIA_MESH_CAMERA_HEADER

#include "victoria/coreTypes.h"
#include "victoria/geometryReader.h"

/* Where to stand to look at a model, shared by the backends that draw one.
 *
 * The camera is derived from the model's own bounds rather than configured, so
 * a teapot and a Sim both arrive framed without anyone tuning a number for
 * either. It orbits, because a still shot of an untextured mesh tells you far
 * less than a turning one about whether the geometry is right.
 *
 * Sims models are z-up. Everything downstream wants y-up, and the swap happens
 * here so no backend has to remember it. */

typedef struct MeshCamera
{
    Real32 centre[3];
    Real32 radius;
} MeshCamera;

void meshCameraFrame(MeshCamera *camera, const GeometryMesh *mesh);

/* Writes sixteen floats, column major, ready for glUniformMatrix4fv and for
   WebGPU's uniform buffers alike. */
void meshCameraBuildMatrix(const MeshCamera *camera, Real32 angleInRadians, Real32 aspect,
                           Real32 *matrix);

/* The light, rotated into the model's own space. Doing it here means a shader
   can light a vertex from its model space normal without a normal matrix, which
   is one less thing to get wrong and one less uniform to upload. Writes three
   floats. */
void meshCameraGetLightDirection(Real32 angleInRadians, Real32 *direction);

#endif
