#ifndef VICTORIA_JPEG_READER_HEADER
#define VICTORIA_JPEG_READER_HEADER

#include "victoria/coreTypes.h"

/* Baseline JPEG decoder.
 *
 * Handles SOF0 (sequential baseline DCT), YCbCr, up to 4:2:0 chroma
 * subsampling.  No dynamic allocation — the caller provides the output
 * buffer.  Only one size is supported: the output buffer must be
 * outWidth * outHeight * 4 bytes of RGBA, where outWidth and outHeight
 * are filled in from the JPEG header.
 *
 * The JPEG data must already be decompressed (i.e. RefPack unwrapped).
 * The maximum supported image size is 512 × 512 pixels. */

typedef enum JpegReadResult
{
    JPEG_READ_OK = 0,
    JPEG_READ_INVALID,      /* Not JPEG, truncated, or corrupt. */
    JPEG_READ_UNSUPPORTED   /* Progressive, CMYK, or other unsupported variant. */
} JpegReadResult;

/* Minimum scratch size in bytes required by jpegReadToRgba. */
#define JPEG_SCRATCH_BYTES 4096U

/* Decode a JPEG into a caller-owned RGBA buffer.
 *
 * scratch / scratchSize : temporary workspace (JPEG_SCRATCH_BYTES minimum).
 * outRgba               : receives *outWidth × *outHeight × 4 RGBA bytes.
 *                         Must be large enough; 512×512×4 = 1 MiB max.
 * outWidth / outHeight  : set to the image dimensions on success. */
JpegReadResult jpegReadToRgba(const Unsigned8 *jpegData, MemorySize jpegSize,
                              Unsigned8 *outRgba, MemorySize outRgbaCapacity,
                              Unsigned32 *outWidth, Unsigned32 *outHeight);

#endif
