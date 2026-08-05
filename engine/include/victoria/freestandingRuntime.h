#ifndef VICTORIA_FREESTANDING_RUNTIME_HEADER
#define VICTORIA_FREESTANDING_RUNTIME_HEADER

#include "victoria/coreTypes.h"

/* Replacements for the handful of C library facilities the engine needs. The
   WebAssembly build links with -nostdlib and the oldest Linux targets cannot
   be assumed to ship a usable libm, so the engine carries its own.

   String handling used to live here too and now sits in utils/strings.h. What
   is left is the part that genuinely stands in for a library the target does
   not have: block memory operations, which the compiler itself will emit calls
   to, and the arithmetic below. */

void memoryCopy(void *destination, const void *source, MemorySize sizeInBytes);
void memoryFill(void *destination, Unsigned8 value, MemorySize sizeInBytes);
Integer32 memoryCompare(const void *first, const void *second, MemorySize sizeInBytes);

Real32 mathSine(Real32 angleInRadians);
Real32 mathCosine(Real32 angleInRadians);

/* Accurate across the whole positive range, not merely near one. Nought for
   nought and for anything negative, so a caller measuring a length gets a
   length rather than a not-a-number. */
Real32 mathSquareRoot(Real32 value);

#define VICTORIA_PI 3.14159265358979323846f

#endif
