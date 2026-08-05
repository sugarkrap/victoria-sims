#ifndef VICTORIA_TEXTURE_DECODE_HEADER
#define VICTORIA_TEXTURE_DECODE_HEADER

#include "victoria/coreTypes.h"
#include "victoria/textureReader.h"

/* Turns a texture level into eight bit RGBA, which is the one layout every
 * backend here can take.
 *
 * The compressed formats are S3TC, which the game calls DXT. A four by four
 * block of pixels becomes eight or sixteen bytes: two endpoint colours and two
 * bits per pixel saying where between them each one sits. Hardware decodes this
 * natively, and one day this engine will hand the blocks straight to it — but
 * not every target can, the software rasterizer certainly cannot, and a decoder
 * is needed to check the reader against a real image regardless.
 *
 * Writes into storage the caller owns. Nothing here allocates. */

/* Bytes needed to decode a level of these dimensions: four per pixel. */
MemorySize textureDecodeGetRequiredBytes(Integer32 width, Integer32 height);

typedef enum TextureDecodeResult
{
    TEXTURE_DECODE_OK = 0,
    TEXTURE_DECODE_UNSUPPORTED_FORMAT,
    /* The source holds fewer bytes than the dimensions require. */
    TEXTURE_DECODE_TRUNCATED,
    TEXTURE_DECODE_DESTINATION_TOO_SMALL
} TextureDecodeResult;

const char *textureDecodeResultGetName(TextureDecodeResult result);

/* Decodes one level. destination receives width * height pixels, four bytes
   each, red first, top row first. */
TextureDecodeResult textureDecodeLevel(Unsigned8 *destination, MemorySize destinationCapacity,
                                       const Unsigned8 *source, MemorySize sourceSizeInBytes,
                                       TextureFormat format, Integer32 width, Integer32 height);

#endif
