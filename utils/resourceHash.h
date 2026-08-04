#ifndef VICTORIA_UTILITIES_RESOURCE_HASH_HEADER
#define VICTORIA_UTILITIES_RESOURCE_HASH_HEADER

#include "victoria/coreTypes.h"

/* A scenegraph resource's key is its name, hashed.
 *
 * This is not a convention the format documents; it is arithmetic, and it was
 * confirmed against three resources in the fixture package by computing the
 * hash of each name and comparing it to the key the file actually filed the
 * resource under. All six words matched exactly.
 *
 * It explains things that looked arbitrary until now. Every instance
 * identifier met so far begins 0xFF because the low hash sets that byte, not
 * because of some pattern in the data. A model called
 * "#0x7f9bd9b9!age3_0_cres" has its own group hash spelled into its name, and
 * 0x7F is the group mask.
 *
 * What it buys is a lookup that does not need to read anything. Finding a
 * texture called "x_txtr" somewhere on a disc was a matter of opening every
 * package, decompressing every texture and comparing the name inside it.
 * Hashing the name gives the key directly, and a key can be matched against
 * index entries that are already parsed.
 *
 * Names are folded to lower case first, so a name written with capitals hashes
 * to the same key as one without — which the game relies on and a reader that
 * skips it will silently fail to find half of what it looks for.
 *
 * Nothing here is a security hash and none of it should be used as one. */

/* CRC24 of the folded name, with 0xFF in the top byte. */
Unsigned32 resourceHashInstance(const char *name);

/* CRC32 in its MPEG-2 form: no reflection, no final inversion. Different from
   the CRC32 in every zip file, and using that one instead produces a plausible
   number that matches nothing. */
Unsigned32 resourceHashInstanceHigh(const char *name);

/* The same CRC24 as the instance hash, with 0x7F in the top byte instead. Used
   for group identifiers, which name a collection of resources rather than one. */
Unsigned32 resourceHashGroup(const char *name);

#endif
