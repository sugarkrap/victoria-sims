#ifndef VICTORIA_FREESTANDING_RUNTIME_HEADER
#define VICTORIA_FREESTANDING_RUNTIME_HEADER

#include "victoria/coreTypes.h"

/* Replacements for the handful of C library facilities the engine needs. The
   WebAssembly build links with -nostdlib and the oldest Linux targets cannot
   be assumed to ship a usable libm, so the engine carries its own. */

void memoryCopy(void *destination, const void *source, MemorySize sizeInBytes);
void memoryFill(void *destination, Unsigned8 value, MemorySize sizeInBytes);
Integer32 memoryCompare(const void *first, const void *second, MemorySize sizeInBytes);

MemorySize stringLength(const char *text);
Boolean stringEquals(const char *first, const char *second);
Boolean stringStartsWith(const char *text, const char *prefix);

/* Stops at the first character that is not a digit, so trailing junk is
   ignored rather than rejected. Returns zero for an empty or non-numeric
   string, which callers treat as "unset". */
MemorySize stringParseUnsigned(const char *text);

/* Writes decimal digits plus a terminator into destination. Returns the number
   of characters written, excluding the terminator, or zero if the buffer is
   too small. Takes the widest unsigned type so timing values do not have to be
   narrowed on 32-bit targets. */
MemorySize stringWriteUnsigned(char *destination, MemorySize destinationCapacity, Unsigned64 value);

/* Appends as much of source as fits, always terminating. Returns the new
   length of destination. */
MemorySize stringAppend(char *destination, MemorySize destinationCapacity, const char *source);

Real32 mathSine(Real32 angleInRadians);
Real32 mathCosine(Real32 angleInRadians);

#define VICTORIA_PI 3.14159265358979323846f

#endif
