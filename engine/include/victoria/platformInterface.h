#ifndef VICTORIA_PLATFORM_INTERFACE_HEADER
#define VICTORIA_PLATFORM_INTERFACE_HEADER

#include "victoria/coreTypes.h"

void platformLogMessage(const char *message);

Unsigned64 platformGetMicroseconds(void);

Boolean platformCacheStore(const char *name, const Unsigned8 *bytes, MemorySize byteCount);
MemorySize platformCacheLoad(const char *name, Unsigned8 *destination, MemorySize capacity);

#endif
