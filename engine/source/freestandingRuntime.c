#include "victoria/freestandingRuntime.h"

void memoryCopy(void *destination, const void *source, MemorySize sizeInBytes)
{
    Unsigned8 *destinationBytes = (Unsigned8 *)destination;
    const Unsigned8 *sourceBytes = (const Unsigned8 *)source;
    MemorySize index;

    for (index = 0UL; index < sizeInBytes; index += 1UL)
    {
        destinationBytes[index] = sourceBytes[index];
    }
}

void memoryFill(void *destination, Unsigned8 value, MemorySize sizeInBytes)
{
    Unsigned8 *destinationBytes = (Unsigned8 *)destination;
    MemorySize index;

    for (index = 0UL; index < sizeInBytes; index += 1UL)
    {
        destinationBytes[index] = value;
    }
}

Integer32 memoryCompare(const void *first, const void *second, MemorySize sizeInBytes)
{
    const Unsigned8 *firstBytes = (const Unsigned8 *)first;
    const Unsigned8 *secondBytes = (const Unsigned8 *)second;
    MemorySize index;

    for (index = 0UL; index < sizeInBytes; index += 1UL)
    {
        if (firstBytes[index] != secondBytes[index])
        {
            return (Integer32)firstBytes[index] - (Integer32)secondBytes[index];
        }
    }
    return 0;
}


/* Odd-power Taylor series after reduction into [-pi/2, pi/2].
 *
 * The quarter turn, not the half turn. A truncated Taylor series is worst at
 * the far end of whatever range it is used over, and this one used to run to
 * pi: five terms there leave the next one, x^11 over 39916800, at seven
 * thousandths — a thousand times the accuracy the comment here claimed, and
 * measured rather than argued. Folding on sin(pi - x) = sin(x) halves the
 * range, which cuts that term by a factor of two thousand, and one more term
 * takes the worst case to under 1e-7. That is at the limit of what a 32-bit
 * float can hold, which is where this should have been all along.
 *
 * Nothing in the engine was visibly wrong because of it: the camera orbit and
 * the colour ramps do not care about a thousandth. A bone whose animated
 * rotation passes a half turn does, since eulerDegreesToQuaternion takes the
 * sine of half its angle. */
Real32 mathSine(Real32 angleInRadians)
{
    Real32 squared;
    Real32 term;
    Real32 result;
    Integer32 wholeTurns;

    wholeTurns = (Integer32)(angleInRadians / (2.0f * VICTORIA_PI));
    angleInRadians -= (Real32)wholeTurns * (2.0f * VICTORIA_PI);

    if (angleInRadians > VICTORIA_PI)
    {
        angleInRadians -= 2.0f * VICTORIA_PI;
    }
    else if (angleInRadians < -VICTORIA_PI)
    {
        angleInRadians += 2.0f * VICTORIA_PI;
    }

    /* Reflect the outer quarters onto the inner ones. The sine is unchanged by
       this, which is what makes it free. */
    if (angleInRadians > (VICTORIA_PI * 0.5f))
    {
        angleInRadians = VICTORIA_PI - angleInRadians;
    }
    else if (angleInRadians < -(VICTORIA_PI * 0.5f))
    {
        angleInRadians = -VICTORIA_PI - angleInRadians;
    }

    squared = angleInRadians * angleInRadians;
    term = angleInRadians;
    result = term;

    term *= -squared / 6.0f;
    result += term;
    term *= -squared / 20.0f;
    result += term;
    term *= -squared / 42.0f;
    result += term;
    term *= -squared / 72.0f;
    result += term;
    term *= -squared / 110.0f;
    result += term;

    return result;
}

Real32 mathCosine(Real32 angleInRadians)
{
    return mathSine(angleInRadians + (VICTORIA_PI * 0.5f));
}

/* Newton's method, over an argument first brought into a range the iteration
 * converges on quickly.
 *
 * Newton only doubles its correct digits once the estimate is already near the
 * root; before that it merely halves the error each step. So a fixed few steps
 * started at one are excellent near one and worthless far from it — four steps
 * give 0.063 for the square root of a millionth, which is off by sixty times.
 * That is not a corner: it is the size of a Sim's triangles. The software
 * backend normalised its face normals with exactly that loop, got normals a
 * sixtieth of unit length, and shaded an entire body at the ambient floor.
 *
 * Dividing by four and doubling the answer are both exact in binary floating
 * point, so the reduction below costs no accuracy at all — it only moves the
 * problem to where five steps are enough for every input.
 *
 * The reductions are counted rather than left to run until the condition
 * clears: an infinity would never satisfy either one, and this is called per
 * triangle. A hundred and sixty steps covers the exponent range of a 32-bit
 * float twice over. */
Real32 mathSquareRoot(Real32 value)
{
    Real32 reduced = value;
    Real32 scale = 1.0f;
    Real32 estimate;
    Unsigned32 step;

    if (!(value > 0.0f))
    {
        return 0.0f;
    }

    for (step = 0U; step < 160U && reduced > 1.0f; step++)
    {
        reduced *= 0.25f;
        scale *= 2.0f;
    }
    for (step = 0U; step < 160U && reduced < 0.25f; step++)
    {
        reduced *= 4.0f;
        scale *= 0.5f;
    }

    /* Whatever is left lies in [0.25, 1], where one is within a factor of two
       of the answer and five steps land on it. */
    estimate = 1.0f;
    for (step = 0U; step < 5U; step++)
    {
        estimate = 0.5f * (estimate + (reduced / estimate));
    }
    return estimate * scale;
}

#if defined(VICTORIA_FREESTANDING_BUILTINS)
/* Compilers lower struct copies and array initialisation onto these regardless
   of whether a C library is linked, so the freestanding build must supply
   them. */
void *memcpy(void *destination, const void *source, unsigned long sizeInBytes);
void *memset(void *destination, int value, unsigned long sizeInBytes);
void *memmove(void *destination, const void *source, unsigned long sizeInBytes);

void *memcpy(void *destination, const void *source, unsigned long sizeInBytes)
{
    memoryCopy(destination, source, (MemorySize)sizeInBytes);
    return destination;
}

void *memset(void *destination, int value, unsigned long sizeInBytes)
{
    memoryFill(destination, (Unsigned8)value, (MemorySize)sizeInBytes);
    return destination;
}

void *memmove(void *destination, const void *source, unsigned long sizeInBytes)
{
    Unsigned8 *destinationBytes = (Unsigned8 *)destination;
    const Unsigned8 *sourceBytes = (const Unsigned8 *)source;
    MemorySize index;

    if (destinationBytes < sourceBytes)
    {
        for (index = 0UL; index < (MemorySize)sizeInBytes; index += 1UL)
        {
            destinationBytes[index] = sourceBytes[index];
        }
    }
    else
    {
        for (index = (MemorySize)sizeInBytes; index > 0UL; index -= 1UL)
        {
            destinationBytes[index - 1UL] = sourceBytes[index - 1UL];
        }
    }
    return destination;
}
#endif
