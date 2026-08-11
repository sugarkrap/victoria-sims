#ifndef VICTORIA_FREESTANDING_RUNTIME_HEADER
#define VICTORIA_FREESTANDING_RUNTIME_HEADER

#include "victoria/coreTypes.h"

void memoryCopy(void *destination, const void *source, MemorySize sizeInBytes);
void memoryFill(void *destination, Unsigned8 value, MemorySize sizeInBytes);
Integer32 memoryCompare(const void *first, const void *second, MemorySize sizeInBytes);

Real32 mathSine(Real32 angleInRadians);
Real32 mathCosine(Real32 angleInRadians);

Real32 mathSquareRoot(Real32 value);

#define VICTORIA_PI 3.14159265358979323846f

#endif
