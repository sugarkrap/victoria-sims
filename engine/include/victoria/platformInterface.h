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

#endif
