#ifndef VICTORIA_MESH_CAMERA_HEADER
#define VICTORIA_MESH_CAMERA_HEADER

#include "victoria/coreTypes.h"
#include "victoria/geometryReader.h"

typedef struct MeshCamera
{
    Real32 centre[3];
    Real32 radius;
} MeshCamera;

void meshCameraFrame(MeshCamera *camera, const GeometryMesh *mesh);

void meshCameraBuildMatrix(const MeshCamera *camera, Real32 angleInRadians, Real32 aspect,
                           Real32 *matrix);

void meshCameraGetLightDirection(Real32 angleInRadians, Real32 *direction);

#endif
