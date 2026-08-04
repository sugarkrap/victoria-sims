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

MemorySize stringLength(const char *text)
{
    MemorySize length = 0UL;
    while (text[length] != '\0')
    {
        length += 1UL;
    }
    return length;
}

Boolean stringEquals(const char *first, const char *second)
{
    MemorySize index = 0UL;

    while (first[index] != '\0' && first[index] == second[index])
    {
        index += 1UL;
    }
    return (Boolean)(first[index] == second[index]);
}

MemorySize stringWriteUnsigned(char *destination, MemorySize destinationCapacity, MemorySize value)
{
    char reversedDigits[24];
    MemorySize digitCount = 0UL;
    MemorySize index;

    do
    {
        reversedDigits[digitCount] = (char)('0' + (char)(value % 10UL));
        digitCount += 1UL;
        value /= 10UL;
    } while (value != 0UL && digitCount < VICTORIA_ARRAY_LENGTH(reversedDigits));

    if (destinationCapacity < digitCount + 1UL)
    {
        if (destinationCapacity > 0UL)
        {
            destination[0] = '\0';
        }
        return 0UL;
    }

    for (index = 0UL; index < digitCount; index += 1UL)
    {
        destination[index] = reversedDigits[digitCount - 1UL - index];
    }
    destination[digitCount] = '\0';
    return digitCount;
}

MemorySize stringAppend(char *destination, MemorySize destinationCapacity, const char *source)
{
    MemorySize destinationLength = stringLength(destination);
    MemorySize sourceIndex = 0UL;

    while (source[sourceIndex] != '\0' && destinationLength + 1UL < destinationCapacity)
    {
        destination[destinationLength] = source[sourceIndex];
        destinationLength += 1UL;
        sourceIndex += 1UL;
    }

    if (destinationCapacity > 0UL)
    {
        destination[destinationLength] = '\0';
    }
    return destinationLength;
}

/* Odd-power Taylor series after reduction into [-pi, pi]. Accurate to roughly
   1e-6 over that range, which is far beyond what colour ramps and camera
   orbits need, and avoids a libm dependency on targets that lack one. */
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

    return result;
}

Real32 mathCosine(Real32 angleInRadians)
{
    return mathSine(angleInRadians + (VICTORIA_PI * 0.5f));
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
