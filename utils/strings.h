#ifndef VICTORIA_UTILITIES_STRINGS_HEADER
#define VICTORIA_UTILITIES_STRINGS_HEADER

#include "victoria/coreTypes.h"

/* The string handling the engine has instead of a C library.
 *
 * The WebAssembly build links with -nostdlib and the oldest targets we support
 * cannot be assumed to ship a usable one, so these are not a convenience layer
 * over <string.h> — they are the only implementation there is. None of them
 * allocate, and none of them can: every one writes into storage the caller
 * already owns, and says so in its return value when that storage is too small.
 *
 * Named for what they operate on rather than what they replace, so
 * characterToLowerCase takes a character and stringLength takes a string. */

MemorySize stringLength(const char *text);
Boolean stringEquals(const char *first, const char *second);
Boolean stringStartsWith(const char *text, const char *prefix);

/* Whether text ends with suffix, ignoring case. Written for file extensions:
   a disc written with 8.3 names spells .PACKAGE and one written with Joliet
   spells .package, and neither is the right answer. Three callers had grown
   their own copy of this, which is three chances for them to disagree about
   what a package is. */
Boolean stringEndsWithIgnoringCase(const char *text, const char *suffix);

/* Whether needle appears anywhere in text, ignoring case. For deciding what a
   path is by what directory it is under, which on a disc laid out by somebody
   else is the only thing there is to go on. */
Boolean stringContainsIgnoringCase(const char *text, const char *needle);

/* Stops at the first character that is not a digit, so trailing junk is
   ignored rather than rejected. Returns zero for an empty or non-numeric
   string, which callers treat as "unset". */
MemorySize stringParseUnsigned(const char *text);

/* Writes decimal digits plus a terminator into destination. Returns the number
   of characters written, excluding the terminator, or zero if the buffer is
   too small. Takes the widest unsigned type so timing values do not have to be
   narrowed on 32-bit targets. */
MemorySize stringWriteUnsigned(char *destination, MemorySize destinationCapacity, Unsigned64 value);

/* The same in hexadecimal, prefixed 0x and padded to the requested number of
   digits. Format identifiers and version marks are quoted in hexadecimal
   everywhere they are documented, and printing one in decimal makes a log line
   that cannot be matched against anything. */
MemorySize stringWriteHexadecimal(char *destination, MemorySize destinationCapacity, Unsigned64 value,
                                  MemorySize digitCount);

/* Appends as much of source as fits, always terminating. Returns the new
   length of destination. */
MemorySize stringAppend(char *destination, MemorySize destinationCapacity, const char *source);

/* Only the twenty-six unaccented letters, deliberately. Case folding anything
   wider depends on a locale, and the callers here compare file paths off a disc
   written in 2004 — where a locale-dependent answer would be a bug, not a
   feature. */
char characterToLowerCase(char character);

#endif
