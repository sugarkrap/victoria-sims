#ifndef VICTORIA_PLATFORM_INTERFACE_HEADER
#define VICTORIA_PLATFORM_INTERFACE_HEADER

#include "victoria/coreTypes.h"

/* Implemented once per platform backend. The engine core calls these and
   nothing else from the outside world. */

void platformLogMessage(const char *message);

#endif
