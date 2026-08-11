#include "victoria/tgaReader.h"

#define TGA_HEADER_SIZE 18U
#define TGA_IMAGE_TYPE_TRUE_COLOR 2U
#define TGA_IMAGE_TYPE_RLE_TRUE_COLOR 10U
#define TGA_DESCRIPTOR_TOP_TO_BOTTOM 0x20U
#define TGA_DESCRIPTOR_RIGHT_TO_LEFT 0x10U

static Unsigned16 readLittleEndian16(const Unsigned8 *data)
{
    return (Unsigned16)((Unsigned16)data[0] | ((Unsigned16)data[1] << 8));
}

static TgaReadResult tgaReadHeader(const Unsigned8 *tgaData, MemorySize tgaSize,
                                   Unsigned8 *outImageType, Unsigned8 *outPixelDepth,
                                   Unsigned8 *outDescriptor, MemorySize *outDataOffset,
                                   Unsigned32 *outWidth, Unsigned32 *outHeight)
{
    Unsigned8 idLength;
    Unsigned8 colorMapType;
    Unsigned16 colorMapLength;
    Unsigned8 colorMapEntrySize;
    MemorySize colorMapBytes;
    MemorySize dataOffset;

    if (tgaData == NULL_POINTER || tgaSize < TGA_HEADER_SIZE)
    {
        return TGA_READ_INVALID;
    }

    idLength = tgaData[0];
    colorMapType = tgaData[1];
    colorMapLength = readLittleEndian16(tgaData + 5);
    colorMapEntrySize = tgaData[7];
    colorMapBytes = ((MemorySize)colorMapLength * (MemorySize)colorMapEntrySize) / 8UL;
    dataOffset = TGA_HEADER_SIZE + (MemorySize)idLength + colorMapBytes;

    if (colorMapType != 0U || dataOffset > tgaSize)
    {
        return TGA_READ_INVALID;
    }

    *outImageType = tgaData[2];
    *outWidth = (Unsigned32)readLittleEndian16(tgaData + 12);
    *outHeight = (Unsigned32)readLittleEndian16(tgaData + 14);
    *outPixelDepth = tgaData[16];
    *outDescriptor = tgaData[17];
    *outDataOffset = dataOffset;

    if (*outWidth == 0U || *outHeight == 0U)
    {
        return TGA_READ_INVALID;
    }
    return TGA_READ_OK;
}

TgaReadResult tgaPeekDimensions(const Unsigned8 *tgaData, MemorySize tgaSize,
                                Unsigned32 *outWidth, Unsigned32 *outHeight)
{
    Unsigned8 imageType;
    Unsigned8 pixelDepth;
    Unsigned8 descriptor;
    MemorySize dataOffset;

    *outWidth = 0U;
    *outHeight = 0U;
    return tgaReadHeader(tgaData, tgaSize, &imageType, &pixelDepth, &descriptor,
                         &dataOffset, outWidth, outHeight);
}

static void tgaStorePixel(Unsigned8 *outRgba, Unsigned32 width, Unsigned32 height,
                          Unsigned8 descriptor, Unsigned32 fileRow, Unsigned32 column,
                          const Unsigned8 *bgra, Unsigned8 bytesPerPixel)
{
    Unsigned32 row = (descriptor & TGA_DESCRIPTOR_TOP_TO_BOTTOM) ? fileRow
                                                                 : (height - 1U - fileRow);
    Unsigned32 col = (descriptor & TGA_DESCRIPTOR_RIGHT_TO_LEFT) ? (width - 1U - column)
                                                                 : column;
    Unsigned8 *destination = outRgba + ((MemorySize)row * width + col) * 4UL;

    destination[0] = bgra[2];
    destination[1] = bgra[1];
    destination[2] = bgra[0];
    destination[3] = (bytesPerPixel == 4U) ? bgra[3] : 0xFFU;
}

TgaReadResult tgaReadToRgba(const Unsigned8 *tgaData, MemorySize tgaSize,
                            Unsigned8 *outRgba, MemorySize outRgbaCapacity,
                            Unsigned32 *outWidth, Unsigned32 *outHeight)
{
    Unsigned8 imageType;
    Unsigned8 pixelDepth;
    Unsigned8 descriptor;
    Unsigned8 bytesPerPixel;
    MemorySize dataOffset;
    MemorySize pixelCount;
    MemorySize requiredCapacity;
    TgaReadResult headerResult;

    *outWidth = 0U;
    *outHeight = 0U;

    headerResult = tgaReadHeader(tgaData, tgaSize, &imageType, &pixelDepth, &descriptor,
                                 &dataOffset, outWidth, outHeight);
    if (headerResult != TGA_READ_OK)
    {
        return headerResult;
    }
    if (imageType != TGA_IMAGE_TYPE_TRUE_COLOR && imageType != TGA_IMAGE_TYPE_RLE_TRUE_COLOR)
    {
        return TGA_READ_UNSUPPORTED;
    }
    if (pixelDepth != 24U && pixelDepth != 32U)
    {
        return TGA_READ_UNSUPPORTED;
    }
    bytesPerPixel = (Unsigned8)(pixelDepth / 8U);
    pixelCount = (MemorySize)(*outWidth) * (MemorySize)(*outHeight);
    requiredCapacity = pixelCount * 4UL;
    if (outRgba == NULL_POINTER || outRgbaCapacity < requiredCapacity)
    {
        return TGA_READ_INVALID;
    }

    if (imageType == TGA_IMAGE_TYPE_TRUE_COLOR)
    {
        MemorySize needed = dataOffset + pixelCount * (MemorySize)bytesPerPixel;
        MemorySize pixelIndex;

        if (needed > tgaSize)
        {
            return TGA_READ_INVALID;
        }
        for (pixelIndex = 0UL; pixelIndex < pixelCount; pixelIndex++)
        {
            const Unsigned8 *source = tgaData + dataOffset + pixelIndex * bytesPerPixel;
            Unsigned32 fileRow = (Unsigned32)(pixelIndex / (*outWidth));
            Unsigned32 column = (Unsigned32)(pixelIndex % (*outWidth));

            tgaStorePixel(outRgba, *outWidth, *outHeight, descriptor, fileRow, column,
                          source, bytesPerPixel);
        }
    }
    else
    {
        MemorySize cursor = dataOffset;
        MemorySize pixelIndex = 0UL;

        while (pixelIndex < pixelCount)
        {
            Unsigned8 packetHeader;
            Unsigned32 runLength;
            Unsigned32 i;

            if (cursor >= tgaSize)
            {
                return TGA_READ_INVALID;
            }
            packetHeader = tgaData[cursor];
            cursor++;
            runLength = (Unsigned32)(packetHeader & 0x7FU) + 1U;
            if (pixelIndex + runLength > pixelCount)
            {
                return TGA_READ_INVALID;
            }

            if (packetHeader & 0x80U)
            {
                const Unsigned8 *source;

                if (cursor + bytesPerPixel > tgaSize)
                {
                    return TGA_READ_INVALID;
                }
                source = tgaData + cursor;
                cursor += bytesPerPixel;
                for (i = 0U; i < runLength; i++)
                {
                    Unsigned32 fileRow = (Unsigned32)(pixelIndex / (*outWidth));
                    Unsigned32 column = (Unsigned32)(pixelIndex % (*outWidth));

                    tgaStorePixel(outRgba, *outWidth, *outHeight, descriptor, fileRow, column,
                                  source, bytesPerPixel);
                    pixelIndex++;
                }
            }
            else
            {
                if (cursor + (MemorySize)runLength * bytesPerPixel > tgaSize)
                {
                    return TGA_READ_INVALID;
                }
                for (i = 0U; i < runLength; i++)
                {
                    const Unsigned8 *source = tgaData + cursor;
                    Unsigned32 fileRow = (Unsigned32)(pixelIndex / (*outWidth));
                    Unsigned32 column = (Unsigned32)(pixelIndex % (*outWidth));

                    tgaStorePixel(outRgba, *outWidth, *outHeight, descriptor, fileRow, column,
                                  source, bytesPerPixel);
                    cursor += bytesPerPixel;
                    pixelIndex++;
                }
            }
        }
    }

    return TGA_READ_OK;
}
