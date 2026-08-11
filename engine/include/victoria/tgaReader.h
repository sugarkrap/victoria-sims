#ifndef VICTORIA_TGA_READER_HEADER
#define VICTORIA_TGA_READER_HEADER

#include "victoria/coreTypes.h"

typedef enum TgaReadResult
{
    TGA_READ_OK = 0,
    TGA_READ_INVALID,
    TGA_READ_UNSUPPORTED
} TgaReadResult;

TgaReadResult tgaReadToRgba(const Unsigned8 *tgaData, MemorySize tgaSize,
                            Unsigned8 *outRgba, MemorySize outRgbaCapacity,
                            Unsigned32 *outWidth, Unsigned32 *outHeight);

TgaReadResult tgaPeekDimensions(const Unsigned8 *tgaData, MemorySize tgaSize,
                                Unsigned32 *outWidth, Unsigned32 *outHeight);

#endif
