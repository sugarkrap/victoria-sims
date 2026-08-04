#ifndef VICTORIA_COMPRESSION_HEADER
#define VICTORIA_COMPRESSION_HEADER

#include "victoria/coreTypes.h"

/* RefPack, which the game's tools call QFS.
 *
 * Almost everything on a retail disc is stored this way — a package that
 * carries a DIR resource is telling you some of its contents are compressed —
 * so without this the engine can read a disc, find a mesh, and then be handed
 * bytes that are not the mesh.
 *
 * It is a plain sliding window scheme, closer to LZ77 than to anything modern:
 * a control byte says how many literal bytes follow and how far back to go for
 * the rest. There is no entropy coding and no dictionary, which is why this
 * decodes in one pass with no state beyond the two cursors.
 *
 * Decompression writes into storage the caller already owns. Nothing here
 * allocates, and a stream claiming more than it was given room for is refused
 * rather than truncated: a half-decompressed resource is not a smaller
 * resource, it is a corrupt one. */

/* Two bytes at offset four of a compressed resource. */
#define COMPRESSION_REFPACK_SIGNATURE 0xFB10U

typedef enum CompressionResult
{
    COMPRESSION_OK = 0,
    COMPRESSION_NOT_COMPRESSED,
    COMPRESSION_TRUNCATED,
    /* A back reference pointing before the start of the output. */
    COMPRESSION_BAD_REFERENCE,
    COMPRESSION_DESTINATION_TOO_SMALL
} CompressionResult;

const char *compressionResultGetName(CompressionResult result);

/* Whether these bytes carry the RefPack header. Cheap, and wrong only in the
   sense that a file could coincidentally begin this way — which is why the
   decompressor validates as it goes rather than trusting this. */
Boolean compressionLooksLikeRefPack(const Unsigned8 *bytes, MemorySize sizeInBytes);

/* How large the resource will be once decompressed, from its header. Zero when
   these bytes are not RefPack. Callers need this before they can find room. */
MemorySize compressionGetDecompressedSize(const Unsigned8 *bytes, MemorySize sizeInBytes);

CompressionResult compressionDecompressRefPack(Unsigned8 *destination, MemorySize destinationCapacity,
                                               const Unsigned8 *source, MemorySize sourceSizeInBytes,
                                               MemorySize *decompressedSizeInBytes);

#endif
