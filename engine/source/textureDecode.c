#include "victoria/textureDecode.h"

const char *textureDecodeResultGetName(TextureDecodeResult result)
{
    switch (result)
    {
    case TEXTURE_DECODE_OK:
        return "ok";
    case TEXTURE_DECODE_UNSUPPORTED_FORMAT:
        return "a pixel format this decoder does not handle";
    case TEXTURE_DECODE_TRUNCATED:
        return "fewer bytes than the image needs";
    case TEXTURE_DECODE_DESTINATION_TOO_SMALL:
        return "the decoded image does not fit where it was to go";
    default:
        return "unknown";
    }
}

MemorySize textureDecodeGetRequiredBytes(Integer32 width, Integer32 height)
{
    if (width <= 0 || height <= 0)
    {
        return 0UL;
    }
    return (MemorySize)width * (MemorySize)height * 4UL;
}

/* Five and six bit channels widened to eight by repeating the high bits into
 * the low ones. Shifting alone would make white come out as 248, and a texture
 * that never quite reaches white is a subtle, permanent wrongness. */
static void unpackColour565(Unsigned16 packed, Unsigned8 *colour)
{
    Unsigned32 red = (Unsigned32)((packed >> 11) & 0x1FU);
    Unsigned32 green = (Unsigned32)((packed >> 5) & 0x3FU);
    Unsigned32 blue = (Unsigned32)(packed & 0x1FU);

    colour[0] = (Unsigned8)((red << 3) | (red >> 2));
    colour[1] = (Unsigned8)((green << 2) | (green >> 4));
    colour[2] = (Unsigned8)((blue << 3) | (blue >> 2));
}

static Unsigned16 readUnsigned16(const Unsigned8 *bytes)
{
    return (Unsigned16)((Unsigned16)bytes[0] | ((Unsigned16)bytes[1] << 8));
}

/* The colour half of a block, which DXT1, DXT3 and DXT5 all share. When the
 * first endpoint is not greater than the second, DXT1 spends one of its four
 * slots on transparency and interpolates only one colour between them. The
 * other two formats carry alpha separately and always use four colours — which
 * is why this is told which rule to follow rather than working it out. */
static void decodeColourBlock(const Unsigned8 *block, Unsigned8 *colours, Boolean allowTransparent)
{
    Unsigned16 first = readUnsigned16(block);
    Unsigned16 second = readUnsigned16(&block[2]);
    Unsigned32 index;

    unpackColour565(first, &colours[0]);
    colours[3] = 255U;
    unpackColour565(second, &colours[4]);
    colours[7] = 255U;

    if (first > second || !allowTransparent)
    {
        for (index = 0U; index < 3U; index++)
        {
            colours[8 + index] =
                (Unsigned8)(((Unsigned32)colours[index] * 2U + (Unsigned32)colours[4 + index]) / 3U);
            colours[12 + index] =
                (Unsigned8)(((Unsigned32)colours[index] + (Unsigned32)colours[4 + index] * 2U) / 3U);
        }
        colours[11] = 255U;
        colours[15] = 255U;
    }
    else
    {
        for (index = 0U; index < 3U; index++)
        {
            colours[8 + index] =
                (Unsigned8)(((Unsigned32)colours[index] + (Unsigned32)colours[4 + index]) / 2U);
            colours[12 + index] = 0U;
        }
        colours[11] = 255U;
        colours[15] = 0U;
    }
}

/* The eight alpha values DXT5 interpolates between its two endpoints. The rule
 * changes when the first is not greater than the second: two of the eight
 * become fully transparent and fully opaque, and only four are interpolated. */
static void decodeAlphaEndpoints(Unsigned8 first, Unsigned8 second, Unsigned8 *alpha)
{
    Unsigned32 index;

    alpha[0] = first;
    alpha[1] = second;
    if (first > second)
    {
        for (index = 0U; index < 6U; index++)
        {
            alpha[2 + index] = (Unsigned8)(((6U - index) * (Unsigned32)first +
                                            (1U + index) * (Unsigned32)second) /
                                           7U);
        }
    }
    else
    {
        for (index = 0U; index < 4U; index++)
        {
            alpha[2 + index] = (Unsigned8)(((4U - index) * (Unsigned32)first +
                                            (1U + index) * (Unsigned32)second) /
                                           5U);
        }
        alpha[6] = 0U;
        alpha[7] = 255U;
    }
}

static void writePixel(Unsigned8 *destination, Integer32 width, Integer32 height, Integer32 x,
                       Integer32 y, const Unsigned8 *colour, Unsigned8 alpha)
{
    MemorySize offset;

    /* A block covers four by four pixels whether or not the image does, so the
     * edge blocks of an image that is not a multiple of four carry pixels that
     * are not part of it. Dropped rather than wrapped. */
    if (x < 0 || y < 0 || x >= width || y >= height)
    {
        return;
    }
    offset = ((MemorySize)y * (MemorySize)width + (MemorySize)x) * 4UL;
    destination[offset] = colour[0];
    destination[offset + 1UL] = colour[1];
    destination[offset + 2UL] = colour[2];
    destination[offset + 3UL] = alpha;
}

static TextureDecodeResult decodeBlockCompressed(Unsigned8 *destination, const Unsigned8 *source,
                                                 MemorySize sourceSizeInBytes, TextureFormat format,
                                                 Integer32 width, Integer32 height)
{
    MemorySize blockBytes = textureFormatGetBlockBytes(format);
    MemorySize colourOffset = (format == TEXTURE_FORMAT_DXT1) ? 0UL : 8UL;
    Integer32 blocksAcross = (width + 3) / 4;
    Integer32 blocksDown = (height + 3) / 4;
    Integer32 blockY;
    Integer32 blockX;
    MemorySize position = 0UL;

    for (blockY = 0; blockY < blocksDown; blockY++)
    {
        for (blockX = 0; blockX < blocksAcross; blockX++)
        {
            Unsigned8 colours[16];
            Unsigned8 alpha[8];
            const Unsigned8 *block;
            Unsigned32 indices;
            Integer32 innerY;

            if (position + blockBytes > sourceSizeInBytes)
            {
                return TEXTURE_DECODE_TRUNCATED;
            }
            block = &source[position];
            position += blockBytes;

            decodeColourBlock(&block[colourOffset], colours,
                              (format == TEXTURE_FORMAT_DXT1) ? BOOLEAN_TRUE : BOOLEAN_FALSE);
            if (format == TEXTURE_FORMAT_DXT5)
            {
                decodeAlphaEndpoints(block[0], block[1], alpha);
            }

            indices = (Unsigned32)block[colourOffset + 4UL] |
                      ((Unsigned32)block[colourOffset + 5UL] << 8) |
                      ((Unsigned32)block[colourOffset + 6UL] << 16) |
                      ((Unsigned32)block[colourOffset + 7UL] << 24);

            for (innerY = 0; innerY < 4; innerY++)
            {
                Integer32 innerX;

                for (innerX = 0; innerX < 4; innerX++)
                {
                    Unsigned32 pixel = (Unsigned32)(innerY * 4 + innerX);
                    Unsigned32 selector = (indices >> (pixel * 2U)) & 0x3U;
                    Unsigned8 pixelAlpha = colours[selector * 4U + 3U];

                    if (format == TEXTURE_FORMAT_DXT3)
                    {
                        /* Four bits a pixel, two to a byte, low nibble first.
                         * Widened by repeating rather than shifting, so fifteen
                         * reaches 255. */
                        Unsigned8 packed = block[pixel / 2U];
                        Unsigned32 value =
                            ((pixel & 1U) == 0U) ? (packed & 0x0FU) : ((packed >> 4) & 0x0FU);

                        pixelAlpha = (Unsigned8)((value << 4) | value);
                    }
                    else if (format == TEXTURE_FORMAT_DXT5)
                    {
                        /* Three bits a pixel across six bytes, so a value can
                         * straddle a byte boundary. Assembled from the whole
                         * forty-eight bit run rather than byte at a time. */
                        Unsigned32 bit = pixel * 3U;
                        Unsigned32 low = (Unsigned32)block[2U + (bit / 8U)];
                        Unsigned32 high =
                            ((bit / 8U) + 1U < 6U) ? (Unsigned32)block[3U + (bit / 8U)] : 0U;
                        Unsigned32 selectorValue = ((low | (high << 8)) >> (bit % 8U)) & 0x7U;

                        pixelAlpha = alpha[selectorValue];
                    }

                    writePixel(destination, width, height, blockX * 4 + innerX, blockY * 4 + innerY,
                               &colours[selector * 4U], pixelAlpha);
                }
            }
        }
    }
    return TEXTURE_DECODE_OK;
}

static TextureDecodeResult decodePlain(Unsigned8 *destination, const Unsigned8 *source,
                                       MemorySize sourceSizeInBytes, TextureFormat format,
                                       Integer32 width, Integer32 height)
{
    MemorySize pixelBytes = textureFormatGetPixelBytes(format);
    MemorySize count = (MemorySize)width * (MemorySize)height;
    MemorySize index;

    if (count * pixelBytes > sourceSizeInBytes)
    {
        return TEXTURE_DECODE_TRUNCATED;
    }
    for (index = 0UL; index < count; index++)
    {
        const Unsigned8 *pixel = &source[index * pixelBytes];
        Unsigned8 *out = &destination[index * 4UL];

        switch (format)
        {
        case TEXTURE_FORMAT_RGBA32:
            out[0] = pixel[0];
            out[1] = pixel[1];
            out[2] = pixel[2];
            out[3] = pixel[3];
            break;
        case TEXTURE_FORMAT_BGR24:
        case TEXTURE_FORMAT_BGR24_REPEAT:
            /* Blue first on disc, red first here. */
            out[0] = pixel[2];
            out[1] = pixel[1];
            out[2] = pixel[0];
            out[3] = 255U;
            break;
        case TEXTURE_FORMAT_ALPHA8:
            out[0] = 255U;
            out[1] = 255U;
            out[2] = 255U;
            out[3] = pixel[0];
            break;
        case TEXTURE_FORMAT_LUMINANCE8:
            /* White with the bits carrying transparency, which is what the
             * game means by luminance here — not a grey ramp. */
            out[0] = 255U;
            out[1] = 255U;
            out[2] = 255U;
            out[3] = pixel[0];
            break;
        case TEXTURE_FORMAT_LUMINANCE16:
            out[0] = 255U;
            out[1] = 255U;
            out[2] = 255U;
            out[3] = pixel[1];
            break;
        default:
            return TEXTURE_DECODE_UNSUPPORTED_FORMAT;
        }
    }
    return TEXTURE_DECODE_OK;
}

TextureDecodeResult textureDecodeLevel(Unsigned8 *destination, MemorySize destinationCapacity,
                                       const Unsigned8 *source, MemorySize sourceSizeInBytes,
                                       TextureFormat format, Integer32 width, Integer32 height)
{
    MemorySize required = textureDecodeGetRequiredBytes(width, height);

    if (destination == NULL_POINTER || source == NULL_POINTER || required == 0UL)
    {
        return TEXTURE_DECODE_TRUNCATED;
    }
    if (destinationCapacity < required)
    {
        return TEXTURE_DECODE_DESTINATION_TOO_SMALL;
    }

    if (textureFormatGetBlockBytes(format) != 0UL)
    {
        return decodeBlockCompressed(destination, source, sourceSizeInBytes, format, width, height);
    }
    if (textureFormatGetPixelBytes(format) != 0UL)
    {
        return decodePlain(destination, source, sourceSizeInBytes, format, width, height);
    }
    return TEXTURE_DECODE_UNSUPPORTED_FORMAT;
}
