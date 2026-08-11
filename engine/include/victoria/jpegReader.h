#ifndef VICTORIA_JPEG_READER_HEADER
#define VICTORIA_JPEG_READER_HEADER

#include "victoria/coreTypes.h"

typedef enum JpegReadResult
{
    JPEG_READ_OK = 0,
    JPEG_READ_INVALID,
    JPEG_READ_UNSUPPORTED
} JpegReadResult;

#define JPEG_SCRATCH_BYTES 4096U

JpegReadResult jpegReadToRgba(const Unsigned8 *jpegData, MemorySize jpegSize,
                              Unsigned8 *outRgba, MemorySize outRgbaCapacity,
                              Unsigned32 *outWidth, Unsigned32 *outHeight);

#endif
