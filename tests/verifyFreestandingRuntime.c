
#include <stdio.h>

#include "utils/assert.h"
#include "victoria/freestandingRuntime.h"

static Integer32 failureCount = 0;

static Real32 absolute(Real32 value)
{
    return (value < 0.0f) ? -value : value;
}

static Real32 relativeError(Real32 measured, Real32 expected)
{
    if (expected == 0.0f)
    {
        return absolute(measured);
    }
    return absolute(measured - expected) / expected;
}

int main(void)
{
    Real32 worstError = 0.0f;
    Unsigned32 index;

    printf("-- the square root over the range a renderer works in --\n");
    {
        static const Real32 roots[] = {
            0.0001f, 0.001f, 0.01f, 0.1f, 1.0f, 10.0f, 100.0f, 1000.0f, 10000.0f,
            0.0003f, 0.003f, 0.03f, 0.3f, 3.0f, 30.0f, 300.0f, 3000.0f,
            0.5f, 2.0f, 7.0f, 123.0f, 0.00025f, 1.4142135f
        };
        const Unsigned32 rootCount = (Unsigned32)(sizeof roots / sizeof roots[0]);

        for (index = 0U; index < rootCount; index++)
        {
            Real32 expected = roots[index];
            Real32 measured = mathSquareRoot(expected * expected);
            Real32 error = relativeError(measured, expected);

            if (error > worstError)
            {
                worstError = error;
            }
        }
        printf("worst relative error over %u magnitudes: %g\n", rootCount, (double)worstError);
        checkThat(&failureCount, "accurate to a part in a hundred thousand everywhere",
                  worstError < 0.00001f);
    }

    printf("\n-- and equally accurate at every scale --\n");
    {
        Real32 nearUnity = relativeError(mathSquareRoot(1.21f), 1.1f);
        Real32 atSimScale = relativeError(mathSquareRoot(0.001f * 0.001f), 0.001f);
        Real32 farBelow = relativeError(mathSquareRoot(1e-12f), 1e-6f);

        printf("error at 1.1: %g   at 0.001: %g   at 0.000001: %g\n",
               (double)nearUnity, (double)atSimScale, (double)farBelow);
        checkThat(&failureCount, "a Sim's triangles are as accurate as a teapot's",
                  atSimScale < 0.00001f);
        checkThat(&failureCount, "and so is a millionth", farBelow < 0.00001f);
    }

    printf("\n-- the normal a Sim's triangle actually produces --\n");
    {
        Real32 normal[3] = { 0.00004f, -0.00007f, 0.00002f };
        Real32 lengthSquared = (normal[0] * normal[0]) + (normal[1] * normal[1]) +
                               (normal[2] * normal[2]);
        Real32 length = mathSquareRoot(lengthSquared);
        Real32 unitLengthSquared = 0.0f;
        Real32 lambert;

        for (index = 0U; index < 3U; index++)
        {
            normal[index] /= length;
            unitLengthSquared += normal[index] * normal[index];
        }
        printf("normalised length: %g\n", (double)mathSquareRoot(unitLengthSquared));
        checkThat(&failureCount, "it comes out unit length, not a sixtieth of one",
                  absolute(mathSquareRoot(unitLengthSquared) - 1.0f) < 0.0001f);

        lambert = absolute(normal[1]);
        printf("shade against a light on Y: %g\n", (double)(0.28f + (0.72f * lambert)));
        checkThat(&failureCount, "and shades well clear of the ambient floor",
                  (0.28f + (0.72f * lambert)) > 0.6f);
    }

    printf("\n-- and it refuses rather than returning a not-a-number --\n");
    {
        checkThat(&failureCount, "nought for nought", mathSquareRoot(0.0f) == 0.0f);
        checkThat(&failureCount, "nought for a negative", mathSquareRoot(-4.0f) == 0.0f);
        checkThat(&failureCount, "and answers a denormal without spinning",
                  mathSquareRoot(1e-40f) >= 0.0f);
    }

    printf("\n-- the trigonometry the camera turns on --\n");
    {
        static const Real32 angles[] = {
            0.0f, 0.5f, 1.0f, 1.5707963f, 2.0f, 2.5f, 3.0f, 3.1415926f,
            -3.1415926f, -1.0f, 4.0f, 5.5f, 6.28f, 20.0f, -20.0f
        };
        Real32 worstIdentity = 0.0f;

        checkThat(&failureCount, "sine of nought", absolute(mathSine(0.0f)) < 0.000001f);
        checkThat(&failureCount, "sine of a right angle",
                  absolute(mathSine(VICTORIA_PI * 0.5f) - 1.0f) < 0.000001f);
        checkThat(&failureCount, "sine of a straight angle, where it was worst",
                  absolute(mathSine(VICTORIA_PI)) < 0.000001f);
        checkThat(&failureCount, "cosine of nought",
                  absolute(mathCosine(0.0f) - 1.0f) < 0.000001f);

        for (index = 0U; index < (Unsigned32)(sizeof angles / sizeof angles[0]); index++)
        {
            Real32 sine = mathSine(angles[index]);
            Real32 cosine = mathCosine(angles[index]);
            Real32 error = absolute((sine * sine) + (cosine * cosine) - 1.0f);

            if (error > worstIdentity)
            {
                worstIdentity = error;
            }
        }
        printf("worst departure from the identity: %g\n", (double)worstIdentity);
        checkThat(&failureCount, "and the identity holds everywhere, turns out too",
                  worstIdentity < 0.000002f);
    }

    return checkSummarize(failureCount, "freestanding runtime");
}
