#ifndef VICTORIA_PLATFORM_INTERFACE_HEADER
#define VICTORIA_PLATFORM_INTERFACE_HEADER

#include "victoria/coreTypes.h"

/* Implemented once per platform backend. The engine core calls these and
   nothing else from the outside world. */

void platformLogMessage(const char *message);

/* Monotonic, in microseconds, from an unspecified origin. Only differences are
   meaningful. The profiler is the only caller that should care about the
   resolution, and it must tolerate a coarse clock: the oldest targets cannot
   promise better than the millisecond. */
Unsigned64 platformGetMicroseconds(void);

/* Somewhere to leave a block of bytes for the next run to find.
 *
 * This exists for one thing and is worth reading with that in mind: turning a
 * font's outlines into pixels is the most expensive thing this engine does per
 * unit of usefulness, and the machine at the floor of the device ladder is the
 * one that can least afford it. Written here once, the whole glyph sheet is
 * read back on every later run and no outline is ever touched again.
 *
 * The name is a plain identifier, not a path — no slashes, no dots leading
 * anywhere. The platform decides where that lands and is the only thing that
 * knows: a directory under the user's own cache natively, and nothing at all in
 * a browser, where there is no filesystem to write to and the sheet stays in an
 * arena for the session instead.
 *
 * Neither of these is ever required to work. A platform with nowhere to put it
 * says so and everything carries on, slower by the second it takes to
 * rasterize a font — which is why the return values are worth checking and
 * never worth failing over.
 *
 * platformCacheLoad returns how many bytes it read, or nought for "there is
 * nothing there, or it did not fit". A short read is a nought: half a cache
 * entry is not a cache entry. */
Boolean platformCacheStore(const char *name, const Unsigned8 *bytes, MemorySize byteCount);
MemorySize platformCacheLoad(const char *name, Unsigned8 *destination, MemorySize capacity);

#endif
