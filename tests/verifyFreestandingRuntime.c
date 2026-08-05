/* Checks the arithmetic the engine carries in place of a C library.
 *
 * mathSquareRoot exists because of a bug it now cannot have again. The software
 * backend normalised its face normals with four Newton steps started at one,
 * written inline. Newton halves its error per step until the estimate is near
 * the root and only then doubles its digits, so a fixed few steps from one are
 * excellent near one and useless far from it. A Sim is under two units across
 * and carries eighteen hundred vertices, which makes its triangles' cross
 * products about a millionth; four steps returned 0.063 where the answer was
 * 0.001. Every normal came out a sixtieth of unit length, every lambert came
 * out near nought, and a whole body shaded flat at the ambient floor. The
 * teapot's triangles are ten times bigger, where the same loop is right to
 * within eight percent, which is why it stood.
 *
 * So the checks below are not "is the square root roughly right" — the old loop
 * would have passed that at the one scale anybody looked. They ask whether it
 * is right across the range the engine actually uses, and whether it is right
 * at the same relative accuracy at every scale. That second property is the one
 * the old loop did not have, and it is the one worth pinning. */

#include <stdio.h>

#include "utils/assert.h"
#include "victoria/freestandingRuntime.h"

static Integer32 failureCount = 0;

static Real32 absolute(Real32 value)
{
    return (value < 0.0f) ? -value : value;
}

/* Relative rather than absolute: an absolute tolerance is a different demand at
   a millionth than at a million, and being scale-free is the whole point. */
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
        /* Every root from a ten-millionth to ten million, by factors of ten.
           The roots are written out rather than computed, so this compares
           against arithmetic done elsewhere and not against itself. */
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
        /* The failing loop was not merely imprecise, it got worse the smaller
           the argument. So this measures the error at the smallest scale
           against the error at unity: if the two differ by orders of magnitude,
           the reduction is not doing its job however good the headline figure
           looks. */
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
        /* Not a synthetic number: an edge of a hundredth against a model under
         * two units across, which is what the disc yields. The old loop turned
         * this normal into one a sixtieth of unit length, so the dot product
         * against any light was near nought and the shade sat on its floor.
         *
         * Normalising and then measuring the length is the property the shading
         * depends on, stated the way the renderer needs it: a unit normal. */
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

        /* And the shade that follows, against a light down one axis. The
           renderer's own ramp: 0.28 ambient plus 0.72 of the lambert. A normal
           of the wrong length lands this on 0.28 whatever the geometry does. */
        lambert = absolute(normal[1]);
        printf("shade against a light on Y: %g\n", (double)(0.28f + (0.72f * lambert)));
        checkThat(&failureCount, "and shades well clear of the ambient floor",
                  (0.28f + (0.72f * lambert)) > 0.6f);
    }

    printf("\n-- and it refuses rather than returning a not-a-number --\n");
    {
        checkThat(&failureCount, "nought for nought", mathSquareRoot(0.0f) == 0.0f);
        checkThat(&failureCount, "nought for a negative", mathSquareRoot(-4.0f) == 0.0f);
        /* A length that underflowed to a denormal must still terminate. The
           reduction loops are counted for exactly this. */
        checkThat(&failureCount, "and answers a denormal without spinning",
                  mathSquareRoot(1e-40f) >= 0.0f);
    }

    printf("\n-- the trigonometry the camera turns on --\n");
    {
        /* Kept here because nothing else checks it directly: meshCamera's test
         * holds the engine's own sine on both sides of its comparison on
         * purpose — the algebra it is testing has to be compared like for like
         * — so it would pass with a sine that was wrong, and did.
         *
         * The tolerance is 1e-6 because that is what the comment on mathSine
         * claims. It used to claim it while being off by seven thousandths at a
         * half turn, which is the far end of the range it reduced into and
         * therefore the one place a truncated series is worst. A tolerance
         * loose enough to pass that would not be testing anything. */
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

        /* The identity is the check that needs no second implementation to
           compare against, and it holds only if both are right at once. */
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
