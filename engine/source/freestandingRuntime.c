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

    estimate = 1.0f;
    for (step = 0U; step < 5U; step++)
    {
        estimate = 0.5f * (estimate + (reduced / estimate));
    }
    return estimate * scale;
}

#if defined(VICTORIA_FREESTANDING_BUILTINS)
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
