#include "render/meshCamera.h"

#include "victoria/freestandingRuntime.h"

/* How far back to stand, as a multiple of the model's radius. Chosen so a
   sphere of that radius fills most of the frame without touching the edges as
   it turns. */
#define VIEW_DISTANCE_IN_RADII 3.2f

/* Cotangent of half the vertical field of view. 1.6 is a little under fifty
   degrees, which is close enough to a natural viewing angle that a model does
   not look like it is being examined through a lens. */
#define FOCAL_LENGTH 1.6f

#define NEAR_PLANE_IN_RADII 0.05f
#define FAR_PLANE_IN_RADII 12.0f

/* Over the viewer's shoulder and slightly to one side, in world space. */
static const Real32 worldLightDirection[3] = { -0.34f, 0.47f, 0.81f };

void meshCameraFrame(MeshCamera *camera, const GeometryMesh *mesh)
{
    Real32 minimum[3];
    Real32 maximum[3];
    Unsigned32 axis;

    camera->radius = 0.001f;
    for (axis = 0U; axis < 3U; axis++)
    {
        camera->centre[axis] = 0.0f;
    }
    if (mesh == NULL_POINTER)
    {
        return;
    }

    geometryMeshGetBounds(mesh, minimum, maximum);
    for (axis = 0U; axis < 3U; axis++)
    {
        Real32 extent = (maximum[axis] - minimum[axis]) * 0.5f;

        camera->centre[axis] = (minimum[axis] + maximum[axis]) * 0.5f;
        if (extent > camera->radius)
        {
            camera->radius = extent;
        }
    }
}

void meshCameraBuildMatrix(const MeshCamera *camera, Real32 angleInRadians, Real32 aspect,
                           Real32 *matrix)
{
    Real32 sine = mathSine(angleInRadians);
    Real32 cosine = mathCosine(angleInRadians);
    Real32 distance = camera->radius * VIEW_DISTANCE_IN_RADII;
    Real32 nearPlane = camera->radius * NEAR_PLANE_IN_RADII;
    Real32 farPlane = camera->radius * FAR_PLANE_IN_RADII;
    Real32 horizontal = FOCAL_LENGTH / ((aspect > 0.0f) ? aspect : 1.0f);
    /* The two terms of the standard perspective depth mapping. */
    Real32 depthScale = (farPlane + nearPlane) / (nearPlane - farPlane);
    Real32 depthOffset = (2.0f * farPlane * nearPlane) / (nearPlane - farPlane);
    /* Where the centre lands after the spin, so the translation can be folded
       in rather than applied as a separate matrix. */
    Real32 acrossAtCentre = (-camera->centre[0] * cosine) + (camera->centre[1] * sine);
    Real32 awayAtCentre = (-camera->centre[0] * sine) - (camera->centre[1] * cosine);
    Real32 depthAtCentre = awayAtCentre + distance;
    Unsigned32 index;

    for (index = 0U; index < 16U; index++)
    {
        matrix[index] = 0.0f;
    }

    /* One matrix rather than four multiplied together: spin about z, swap the
       model's z-up for the viewer's y-up, push back, then project. Written out
       because the product is short enough to read, and a general multiply would
       need a general matrix type nothing else here wants.

       Column major, so matrix[column * 4 + row]. A model point is transformed
       as clip = matrix * (x, y, z, 1), with the model's y running away from the
       viewer and becoming the depth the perspective divide uses. */

    matrix[0] = horizontal * cosine;
    matrix[2] = -depthScale * sine;
    matrix[3] = sine;

    matrix[4] = -horizontal * sine;
    matrix[6] = -depthScale * cosine;
    matrix[7] = cosine;

    matrix[9] = FOCAL_LENGTH;

    matrix[12] = horizontal * acrossAtCentre;
    matrix[13] = FOCAL_LENGTH * -camera->centre[2];
    matrix[14] = (-depthScale * depthAtCentre) + depthOffset;
    matrix[15] = depthAtCentre;
}

void meshCameraGetLightDirection(Real32 angleInRadians, Real32 *direction)
{
    Real32 sine = mathSine(angleInRadians);
    Real32 cosine = mathCosine(angleInRadians);

    /* The inverse rotation: turning the light the other way is the same as
       turning the model, and costs one vector instead of a matrix per normal. */
    direction[0] = (worldLightDirection[0] * cosine) + (worldLightDirection[1] * sine);
    direction[1] = (worldLightDirection[0] * -sine) + (worldLightDirection[1] * cosine);
    direction[2] = worldLightDirection[2];
}
