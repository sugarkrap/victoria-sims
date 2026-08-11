#ifndef VICTORIA_UTILITIES_STRINGS_HEADER
#define VICTORIA_UTILITIES_STRINGS_HEADER

#include "victoria/coreTypes.h"

MemorySize stringLength(const char *text);
Boolean stringEquals(const char *first, const char *second);
Boolean stringStartsWith(const char *text, const char *prefix);

Boolean stringEndsWithIgnoringCase(const char *text, const char *suffix);

Boolean stringContainsIgnoringCase(const char *text, const char *needle);

Boolean stringEqualsIgnoringCase(const char *first, const char *second);

MemorySize stringParseUnsigned(const char *text);

MemorySize stringWriteUnsigned(char *destination, MemorySize destinationCapacity, Unsigned64 value);

MemorySize stringWriteHexadecimal(char *destination, MemorySize destinationCapacity, Unsigned64 value,
                                  MemorySize digitCount);

MemorySize stringAppend(char *destination, MemorySize destinationCapacity, const char *source);

char characterToLowerCase(char character);

#endif
