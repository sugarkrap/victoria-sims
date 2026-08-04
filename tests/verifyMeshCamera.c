/* Checks the hand-written camera matrix against the same projection done
   long-hand, step by step.

   The matrix in meshCamera.c is four transforms multiplied out by hand — spin,
   axis swap, push back, project — which is the sort of thing that is either
   exactly right or silently wrong in a way that looks almost plausible on
   screen. This does each step separately and compares.

   It uses the engine's own mathSine and mathCosine rather than the C library's.
   The first version of this test used libm and reported mismatches of three
   parts in ten thousand at angles past a radian, which was the engine's Taylor
   series being compared against a better one — not the algebra being wrong.
   Testing the matrix means holding the trigonometry the same on both sides. */

#include <stdio.h>

#include "render/meshCamera.h"
#include "utils/assert.h"
#include "victoria/freestandingRuntime.h"

/* Must match meshCamera.c. Duplicated deliberately: if someone changes the
   framing there, this test should fail and make them say so here too. */
#define VIEW_DISTANCE_IN_RADII 3.2f
#define FOCAL_LENGTH 1.6f
#define NEAR_PLANE_IN_RADII 0.05f
#define FAR_PLANE_IN_RADII 12.0f

static Integer32 failureCount = 0;

static Real32 absolute(Real32 value)
{
    return (value < 0.0f) ? -value : value;
}

/* Projects one point the long way, then through the matrix, and compares. */
static Boolean agreesAt(const MeshCamera *camera, Real32 angle, Real32 aspect, Real32 pointX,
                        Real32 pointY, Real32 pointZ)
{
    Real32 matrix[16];
    Real32 sine = mathSine(angle);
    Real32 cosine = mathCosine(angle);
    Real32 distance = camera->radius * VIEW_DISTANCE_IN_RADII;
    Real32 nearPlane = camera->radius * NEAR_PLANE_IN_RADII;
    Real32 farPlane = camera->radius * FAR_PLANE_IN_RADII;

    Real32 centredX = pointX - camera->centre[0];
    Real32 centredY = pointY - camera->centre[1];
    Real32 centredZ = pointZ - camera->centre[2];
    Real32 spunX = (centredX * cosine) - (centredY * sine);
    Real32 spunY = (centredX * sine) + (centredY * cosine);
    /* The viewer looks down negative z, so what is in front is negative. */
    Real32 viewZ = -(spunY + distance);

    Real32 wantX = (FOCAL_LENGTH / aspect) * spunX;
    Real32 wantY = FOCAL_LENGTH * centredZ;
    Real32 wantZ = (((farPlane + nearPlane) / (nearPlane - farPlane)) * viewZ) +
                   ((2.0f * farPlane * nearPlane) / (nearPlane - farPlane));
    Real32 wantW = -viewZ;

    Real32 gotX;
    Real32 gotY;
    Real32 gotZ;
    Real32 gotW;
    Real32 tolerance;

    meshCameraBuildMatrix(camera, angle, aspect, matrix);
    gotX = (matrix[0] * pointX) + (matrix[4] * pointY) + (matrix[8] * pointZ) + matrix[12];
    gotY = (matrix[1] * pointX) + (matrix[5] * pointY) + (matrix[9] * pointZ) + matrix[13];
    gotZ = (matrix[2] * pointX) + (matrix[6] * pointY) + (matrix[10] * pointZ) + matrix[14];
    gotW = (matrix[3] * pointX) + (matrix[7] * pointY) + (matrix[11] * pointZ) + matrix[15];

    /* Relative, because the depth term is two orders of magnitude larger than
       the screen ones and a fixed tolerance would be meaningless for both. */
    tolerance = 0.0005f * (camera->radius + absolute(pointX) + absolute(pointY) + absolute(pointZ));
    if (absolute(gotX - wantX) > tolerance || absolute(gotY - wantY) > tolerance ||
        absolute(gotZ - wantZ) > tolerance * 10.0f || absolute(gotW - wantW) > tolerance)
    {
        printf("  at (%.1f %.1f %.1f) angle %.2f\n", (double)pointX, (double)pointY, (double)pointZ,
               (double)angle);
        printf("    want %.4f %.4f %.4f %.4f\n", (double)wantX, (double)wantY, (double)wantZ,
               (double)wantW);
        printf("    got  %.4f %.4f %.4f %.4f\n", (double)gotX, (double)gotY, (double)gotZ,
               (double)gotW);
        return BOOLEAN_FALSE;
    }
    return BOOLEAN_TRUE;
}

int main(void)
{
    MeshCamera camera;
    Boolean allAgree = BOOLEAN_TRUE;
    Unsigned32 step;

    /* Deliberately off centre and off origin: a matrix that forgets the
       translation still looks right for a model centred on nothing. */
    camera.centre[0] = 0.3f;
    camera.centre[1] = -0.7f;
    camera.centre[2] = 1.6f;
    camera.radius = 3.4f;

    printf("-- the matrix against the long way round --\n");
    for (step = 0U; step < 12U; step++)
    {
        Real32 angle = (Real32)step * 0.52f;

        if (!agreesAt(&camera, angle, 1.28f, 1.0f, 2.0f, 3.0f) ||
            !agreesAt(&camera, angle, 1.28f, -2.5f, 0.4f, 0.0f) ||
            !agreesAt(&camera, angle, 0.75f, 0.0f, 0.0f, 0.0f) ||
            !agreesAt(&camera, angle, 2.10f, 4.0f, -4.0f, 2.0f))
        {
            allAgree = BOOLEAN_FALSE;
        }
    }
    checkThat(&failureCount, "agrees at every angle, aspect and point tried", allAgree);

    printf("\n-- does a model end up on screen --\n");
    {
        /* The whole point of framing from the bounds: a model of any size and
           position should land inside the clip volume rather than needing a
           number tuned for it. */
        Real32 matrix[16];
        Boolean allVisible = BOOLEAN_TRUE;
        Unsigned32 corner;

        meshCameraBuildMatrix(&camera, 0.9f, 1.28f, matrix);
        for (corner = 0U; corner < 8U; corner++)
        {
            Real32 x = camera.centre[0] + (((corner & 1U) != 0U) ? camera.radius : -camera.radius);
            Real32 y = camera.centre[1] + (((corner & 2U) != 0U) ? camera.radius : -camera.radius);
            Real32 z = camera.centre[2] + (((corner & 4U) != 0U) ? camera.radius : -camera.radius);
            Real32 clipX = (matrix[0] * x) + (matrix[4] * y) + (matrix[8] * z) + matrix[12];
            Real32 clipY = (matrix[1] * x) + (matrix[5] * y) + (matrix[9] * z) + matrix[13];
            Real32 clipZ = (matrix[2] * x) + (matrix[6] * y) + (matrix[10] * z) + matrix[14];
            Real32 clipW = (matrix[3] * x) + (matrix[7] * y) + (matrix[11] * z) + matrix[15];

            if (clipW <= 0.0f || absolute(clipX) > clipW || absolute(clipY) > clipW ||
                clipZ < -clipW || clipZ > clipW)
            {
                printf("  corner %u falls outside the view\n", (unsigned)corner);
                allVisible = BOOLEAN_FALSE;
            }
        }
        checkThat(&failureCount, "every corner of the model's box is in front of the camera",
                  allVisible);
    }

    printf("\n-- the light turns with the model --\n");
    {
        Real32 atRest[3];
        Real32 turned[3];
        Real32 lengthAtRest;
        Real32 lengthTurned;

        meshCameraGetLightDirection(0.0f, atRest);
        meshCameraGetLightDirection(1.7f, turned);

        lengthAtRest = (atRest[0] * atRest[0]) + (atRest[1] * atRest[1]) + (atRest[2] * atRest[2]);
        lengthTurned = (turned[0] * turned[0]) + (turned[1] * turned[1]) + (turned[2] * turned[2]);

        /* Rotating a direction must not change how long it is, or the model
           would get brighter and dimmer as it spun. */
        checkThat(&failureCount, "rotating the light does not change its length",
                  absolute(lengthAtRest - lengthTurned) < 0.001f);
        checkThat(&failureCount, "and it does move", absolute(atRest[0] - turned[0]) > 0.01f);
        checkThat(&failureCount, "the vertical component is untouched",
                  absolute(atRest[2] - turned[2]) < 0.0001f);
    }

    return checkSummarize(failureCount, "mesh camera");
}
