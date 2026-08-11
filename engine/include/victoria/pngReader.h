#ifndef VICTORIA_PNG_READER_HEADER
#define VICTORIA_PNG_READER_HEADER

#include "victoria/coreTypes.h"

typedef enum PngReadResult
{
    PNG_READ_OK = 0,
    PNG_READ_INVALID,
    PNG_READ_UNSUPPORTED
} PngReadResult;

PngReadResult pngPeekDimensions(const Unsigned8 *pngData, MemorySize pngSize,
                                Unsigned32 *outWidth, Unsigned32 *outHeight,
                                MemorySize *outScratchCapacity);

PngReadResult pngReadToRgba(const Unsigned8 *pngData, MemorySize pngSize,
                            Unsigned8 *outRgba, MemorySize outRgbaCapacity,
                            Unsigned8 *scratch, MemorySize scratchCapacity,
                            Unsigned32 *outWidth, Unsigned32 *outHeight);

#endif
