#ifndef VICTORIA_UTILITIES_CHECKSUM_HEADER
#define VICTORIA_UTILITIES_CHECKSUM_HEADER

#include "victoria/coreTypes.h"

/* The CRC32 everything outside this game uses.
 *
 * Deliberately not in resourceHash.c beside the other one. That file computes
 * CRC32 in its MPEG-2 form — same polynomial, no reflection, no final inversion
 * — because that is what the game's resource keys are built from. This is the
 * reflected, inverted variant that zip files, PNG chunks and installer headers
 * use. Two functions that differ only in a bit order are exactly the pair that
 * gets confused, so they are named for what they are and kept apart. */
Unsigned32 checksumCrc32(const Unsigned8 *bytes, MemorySize byteCount);

#endif
