#include "victoria/pngReader.h"
#include "victoria/freestandingRuntime.h"
#include "utils/checksum.h"

#define PNG_SIGNATURE_SIZE 8UL
#define PNG_IHDR_DATA_SIZE 13UL
#define PNG_PALETTE_ENTRY_LIMIT 256UL

static const Unsigned8 PNG_SIGNATURE[PNG_SIGNATURE_SIZE] =
    {0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU};

static Unsigned32 readBigEndian32(const Unsigned8 *bytes)
{
    return ((Unsigned32)bytes[0] << 24) | ((Unsigned32)bytes[1] << 16) |
           ((Unsigned32)bytes[2] << 8) | (Unsigned32)bytes[3];
}

typedef struct PngHeader
{
    Unsigned32 width;
    Unsigned32 height;
    Unsigned8 colorType;
    Unsigned32 channels;
    MemorySize rowBytes;
    MemorySize filteredSize;
    Unsigned8 palette[PNG_PALETTE_ENTRY_LIMIT * 3UL];
} PngHeader;

static PngReadResult pngParseHeader(const Unsigned8 *data, MemorySize size, PngHeader *header)
{
    MemorySize cursor;
    Boolean sawIhdr = BOOLEAN_FALSE;
    Boolean sawPlte = BOOLEAN_FALSE;

    if (data == NULL_POINTER || size < PNG_SIGNATURE_SIZE ||
        memoryCompare(data, PNG_SIGNATURE, PNG_SIGNATURE_SIZE) != 0)
    {
        return PNG_READ_INVALID;
    }

    cursor = PNG_SIGNATURE_SIZE;
    while (cursor + 12UL <= size)
    {
        Unsigned32 chunkLength = readBigEndian32(data + cursor);
        const Unsigned8 *chunkType = data + cursor + 4UL;
        const Unsigned8 *chunkData = data + cursor + 8UL;
        MemorySize chunkTotal;

        if ((MemorySize)chunkLength > size)
        {
            return PNG_READ_INVALID;
        }
        chunkTotal = 8UL + (MemorySize)chunkLength + 4UL;
        if (cursor + chunkTotal > size)
        {
            return PNG_READ_INVALID;
        }
        if (checksumCrc32(chunkType, 4UL + (MemorySize)chunkLength) !=
            readBigEndian32(data + cursor + 8UL + (MemorySize)chunkLength))
        {
            return PNG_READ_INVALID;
        }

        if (memoryCompare(chunkType, "IHDR", 4UL) == 0)
        {
            Unsigned8 bitDepth;
            Unsigned8 interlaceMethod;

            if (sawIhdr || chunkLength != PNG_IHDR_DATA_SIZE)
            {
                return PNG_READ_INVALID;
            }
            header->width = readBigEndian32(chunkData);
            header->height = readBigEndian32(chunkData + 4UL);
            bitDepth = chunkData[8];
            header->colorType = chunkData[9];
            interlaceMethod = chunkData[12];
            sawIhdr = BOOLEAN_TRUE;

            if (header->width == 0U || header->height == 0U)
            {
                return PNG_READ_INVALID;
            }
            if (bitDepth != 8U || interlaceMethod != 0U)
            {
                return PNG_READ_UNSUPPORTED;
            }
            switch (header->colorType)
            {
            case 0U:
                header->channels = 1U;
                break;
            case 2U:
                header->channels = 3U;
                break;
            case 3U:
                header->channels = 1U;
                break;
            case 4U:
                header->channels = 2U;
                break;
            case 6U:
                header->channels = 4U;
                break;
            default:
                return PNG_READ_UNSUPPORTED;
            }
            header->rowBytes = (MemorySize)header->width * (MemorySize)header->channels;
            header->filteredSize = (MemorySize)header->height * (header->rowBytes + 1UL);
        }
        else if (memoryCompare(chunkType, "PLTE", 4UL) == 0)
        {
            MemorySize paletteBytes = (chunkLength <= PNG_PALETTE_ENTRY_LIMIT * 3UL)
                                          ? (MemorySize)chunkLength
                                          : PNG_PALETTE_ENTRY_LIMIT * 3UL;

            if (!sawIhdr)
            {
                return PNG_READ_INVALID;
            }
            memoryCopy(header->palette, chunkData, paletteBytes);
            sawPlte = BOOLEAN_TRUE;
        }
        else if (memoryCompare(chunkType, "IEND", 4UL) == 0)
        {
            break;
        }

        cursor += chunkTotal;
    }

    if (!sawIhdr)
    {
        return PNG_READ_INVALID;
    }
    if (header->colorType == 3U && !sawPlte)
    {
        return PNG_READ_INVALID;
    }
    return PNG_READ_OK;
}

typedef struct PngChunkSource
{
    const Unsigned8 *fileData;
    MemorySize fileSize;
    MemorySize chunkCursor;
    const Unsigned8 *idatData;
    MemorySize idatRemaining;
} PngChunkSource;

static Boolean pngChunkSourceNextByte(PngChunkSource *source, Unsigned8 *outByte)
{
    for (;;)
    {
        Unsigned32 chunkLength;
        const Unsigned8 *chunkType;
        MemorySize chunkTotal;

        if (source->idatRemaining > 0UL)
        {
            *outByte = *source->idatData;
            source->idatData++;
            source->idatRemaining--;
            return BOOLEAN_TRUE;
        }
        if (source->chunkCursor + 12UL > source->fileSize)
        {
            return BOOLEAN_FALSE;
        }
        chunkLength = readBigEndian32(source->fileData + source->chunkCursor);
        chunkType = source->fileData + source->chunkCursor + 4UL;
        if ((MemorySize)chunkLength > source->fileSize)
        {
            return BOOLEAN_FALSE;
        }
        chunkTotal = 8UL + (MemorySize)chunkLength + 4UL;
        if (source->chunkCursor + chunkTotal > source->fileSize)
        {
            return BOOLEAN_FALSE;
        }

        if (memoryCompare(chunkType, "IDAT", 4UL) == 0)
        {
            source->idatData = source->fileData + source->chunkCursor + 8UL;
            source->idatRemaining = (MemorySize)chunkLength;
            source->chunkCursor += chunkTotal;
        }
        else if (memoryCompare(chunkType, "IEND", 4UL) == 0)
        {
            return BOOLEAN_FALSE;
        }
        else
        {
            source->chunkCursor += chunkTotal;
        }
    }
}

typedef struct InflateBitReader
{
    PngChunkSource *source;
    Unsigned32 bitBuffer;
    Unsigned32 bitCount;
} InflateBitReader;

static Boolean inflateGetBit(InflateBitReader *reader, Unsigned32 *outBit)
{
    if (reader->bitCount == 0U)
    {
        Unsigned8 nextByte;

        if (!pngChunkSourceNextByte(reader->source, &nextByte))
        {
            return BOOLEAN_FALSE;
        }
        reader->bitBuffer = nextByte;
        reader->bitCount = 8U;
    }
    *outBit = reader->bitBuffer & 1U;
    reader->bitBuffer >>= 1;
    reader->bitCount--;
    return BOOLEAN_TRUE;
}

static Boolean inflateGetBits(InflateBitReader *reader, Unsigned32 count, Unsigned32 *outValue)
{
    Unsigned32 value = 0U;
    Unsigned32 index;

    for (index = 0U; index < count; index++)
    {
        Unsigned32 bit;

        if (!inflateGetBit(reader, &bit))
        {
            return BOOLEAN_FALSE;
        }
        value |= bit << index;
    }
    *outValue = value;
    return BOOLEAN_TRUE;
}

static void inflateAlignToByte(InflateBitReader *reader)
{
    reader->bitCount = 0U;
    reader->bitBuffer = 0U;
}

#define DEFLATE_MAX_BITS 15
#define DEFLATE_MAX_SYMBOLS 288U

typedef struct DeflateHuffTable
{
    Integer32 minCode[DEFLATE_MAX_BITS + 1];
    Integer32 maxCode[DEFLATE_MAX_BITS + 1];
    Integer32 firstSym[DEFLATE_MAX_BITS + 1];
    Unsigned16 values[DEFLATE_MAX_SYMBOLS];
} DeflateHuffTable;

static Boolean buildDeflateHuffTable(DeflateHuffTable *table, const Unsigned8 *lengths,
                                     Unsigned32 symbolCount)
{
    Unsigned32 lengthCounts[DEFLATE_MAX_BITS + 1];
    Unsigned32 placeCursor[DEFLATE_MAX_BITS + 1];
    Unsigned32 symbolIndex;
    Integer32 bitLength;
    Integer32 code;

    for (bitLength = 0; bitLength <= DEFLATE_MAX_BITS; bitLength++)
    {
        lengthCounts[bitLength] = 0U;
    }
    for (symbolIndex = 0U; symbolIndex < symbolCount; symbolIndex++)
    {
        if (lengths[symbolIndex] > (Unsigned8)DEFLATE_MAX_BITS)
        {
            return BOOLEAN_FALSE;
        }
        lengthCounts[lengths[symbolIndex]]++;
    }
    lengthCounts[0] = 0U;

    code = 0;
    table->firstSym[0] = 0;
    for (bitLength = 1; bitLength <= DEFLATE_MAX_BITS; bitLength++)
    {
        code = (code + (Integer32)lengthCounts[bitLength - 1]) << 1;
        table->minCode[bitLength] = code;
        table->maxCode[bitLength] = (lengthCounts[bitLength] > 0U)
                                        ? code + (Integer32)lengthCounts[bitLength] - 1
                                        : -1;
        table->firstSym[bitLength] = table->firstSym[bitLength - 1] +
                                     (Integer32)lengthCounts[bitLength - 1];
        placeCursor[bitLength] = (Unsigned32)table->firstSym[bitLength];
    }

    for (symbolIndex = 0U; symbolIndex < symbolCount; symbolIndex++)
    {
        Unsigned8 length = lengths[symbolIndex];

        if (length == 0U)
        {
            continue;
        }
        table->values[placeCursor[length]] = (Unsigned16)symbolIndex;
        placeCursor[length]++;
    }
    return BOOLEAN_TRUE;
}

static Boolean decodeHuffSymbol(InflateBitReader *reader, const DeflateHuffTable *table,
                                Unsigned32 *outSymbol)
{
    Integer32 code = 0;
    Integer32 bitLength;

    for (bitLength = 1; bitLength <= DEFLATE_MAX_BITS; bitLength++)
    {
        Unsigned32 bit;

        if (!inflateGetBit(reader, &bit))
        {
            return BOOLEAN_FALSE;
        }
        code = (code << 1) | (Integer32)bit;
        if (table->maxCode[bitLength] >= 0 && code >= table->minCode[bitLength] &&
            code <= table->maxCode[bitLength])
        {
            *outSymbol = table->values[table->firstSym[bitLength] + (code - table->minCode[bitLength])];
            return BOOLEAN_TRUE;
        }
    }
    return BOOLEAN_FALSE;
}

static Boolean buildFixedLiteralTable(DeflateHuffTable *table)
{
    Unsigned8 lengths[DEFLATE_MAX_SYMBOLS];
    Unsigned32 symbolIndex;

    for (symbolIndex = 0U; symbolIndex < 144U; symbolIndex++)
        lengths[symbolIndex] = 8U;
    for (symbolIndex = 144U; symbolIndex < 256U; symbolIndex++)
        lengths[symbolIndex] = 9U;
    for (symbolIndex = 256U; symbolIndex < 280U; symbolIndex++)
        lengths[symbolIndex] = 7U;
    for (symbolIndex = 280U; symbolIndex < 288U; symbolIndex++)
        lengths[symbolIndex] = 8U;
    return buildDeflateHuffTable(table, lengths, DEFLATE_MAX_SYMBOLS);
}

static Boolean buildFixedDistanceTable(DeflateHuffTable *table)
{
    Unsigned8 lengths[30];
    Unsigned32 symbolIndex;

    for (symbolIndex = 0U; symbolIndex < 30U; symbolIndex++)
        lengths[symbolIndex] = 5U;
    return buildDeflateHuffTable(table, lengths, 30U);
}

static const Unsigned8 CODE_LENGTH_ORDER[19] =
    {16U, 17U, 18U, 0U, 8U, 7U, 9U, 6U, 10U, 5U, 11U, 4U, 12U, 3U, 13U, 2U, 14U, 1U, 15U};

static Boolean readDynamicTables(InflateBitReader *reader, DeflateHuffTable *literalTable,
                                 DeflateHuffTable *distanceTable)
{
    Unsigned32 hlit, hdist, hclen;
    Unsigned8 codeLengthLengths[19];
    DeflateHuffTable codeLengthTable;
    Unsigned8 lengths[DEFLATE_MAX_SYMBOLS + 32U];
    Unsigned32 symbolsFilled;
    Unsigned32 orderIndex;

    if (!inflateGetBits(reader, 5U, &hlit) || !inflateGetBits(reader, 5U, &hdist) ||
        !inflateGetBits(reader, 4U, &hclen))
    {
        return BOOLEAN_FALSE;
    }
    hlit += 257U;
    hdist += 1U;
    hclen += 4U;

    for (orderIndex = 0U; orderIndex < 19U; orderIndex++)
    {
        codeLengthLengths[orderIndex] = 0U;
    }
    for (orderIndex = 0U; orderIndex < hclen; orderIndex++)
    {
        Unsigned32 value;

        if (!inflateGetBits(reader, 3U, &value))
        {
            return BOOLEAN_FALSE;
        }
        codeLengthLengths[CODE_LENGTH_ORDER[orderIndex]] = (Unsigned8)value;
    }
    if (!buildDeflateHuffTable(&codeLengthTable, codeLengthLengths, 19U))
    {
        return BOOLEAN_FALSE;
    }

    if (hlit + hdist > VICTORIA_ARRAY_LENGTH(lengths))
    {
        return BOOLEAN_FALSE;
    }
    symbolsFilled = 0U;
    while (symbolsFilled < hlit + hdist)
    {
        Unsigned32 symbol;

        if (!decodeHuffSymbol(reader, &codeLengthTable, &symbol))
        {
            return BOOLEAN_FALSE;
        }
        if (symbol < 16U)
        {
            lengths[symbolsFilled] = (Unsigned8)symbol;
            symbolsFilled++;
        }
        else if (symbol == 16U)
        {
            Unsigned32 repeat;
            Unsigned8 previous;

            if (symbolsFilled == 0U || !inflateGetBits(reader, 2U, &repeat))
            {
                return BOOLEAN_FALSE;
            }
            repeat += 3U;
            previous = lengths[symbolsFilled - 1U];
            if (symbolsFilled + repeat > hlit + hdist)
            {
                return BOOLEAN_FALSE;
            }
            while (repeat-- > 0U)
            {
                lengths[symbolsFilled] = previous;
                symbolsFilled++;
            }
        }
        else if (symbol == 17U || symbol == 18U)
        {
            Unsigned32 repeat;
            Unsigned32 extraBits = (symbol == 17U) ? 3U : 7U;
            Unsigned32 base = (symbol == 17U) ? 3U : 11U;

            if (!inflateGetBits(reader, extraBits, &repeat))
            {
                return BOOLEAN_FALSE;
            }
            repeat += base;
            if (symbolsFilled + repeat > hlit + hdist)
            {
                return BOOLEAN_FALSE;
            }
            while (repeat-- > 0U)
            {
                lengths[symbolsFilled] = 0U;
                symbolsFilled++;
            }
        }
        else
        {
            return BOOLEAN_FALSE;
        }
    }

    if (!buildDeflateHuffTable(literalTable, lengths, hlit))
    {
        return BOOLEAN_FALSE;
    }
    return buildDeflateHuffTable(distanceTable, lengths + hlit, hdist);
}

static const Unsigned16 LENGTH_BASE[29] =
    {3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 13U, 15U, 17U, 19U, 23U, 27U, 31U, 35U, 43U, 51U,
     59U, 67U, 83U, 99U, 115U, 131U, 163U, 195U, 227U, 258U};
static const Unsigned8 LENGTH_EXTRA_BITS[29] =
    {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 1U, 1U, 1U, 2U, 2U, 2U, 2U, 3U, 3U, 3U, 3U,
     4U, 4U, 4U, 4U, 5U, 5U, 5U, 5U, 0U};
static const Unsigned16 DIST_BASE[30] =
    {1U, 2U, 3U, 4U, 5U, 7U, 9U, 13U, 17U, 25U, 33U, 49U, 65U, 97U, 129U, 193U, 257U, 385U,
     513U, 769U, 1025U, 1537U, 2049U, 3073U, 4097U, 6145U, 8193U, 12289U, 16385U, 24577U};
static const Unsigned8 DIST_EXTRA_BITS[30] =
    {0U, 0U, 0U, 0U, 1U, 1U, 2U, 2U, 3U, 3U, 4U, 4U, 5U, 5U, 6U, 6U, 7U, 7U, 8U, 8U,
     9U, 9U, 10U, 10U, 11U, 11U, 12U, 12U, 13U, 13U};

static PngReadResult inflateStream(PngChunkSource *source, Unsigned8 *output,
                                   MemorySize outputCapacity, MemorySize *outputPosition)
{
    InflateBitReader reader;
    Unsigned8 cmf, flg;
    Unsigned32 isFinal;

    *outputPosition = 0UL;

    if (!pngChunkSourceNextByte(source, &cmf) || !pngChunkSourceNextByte(source, &flg))
    {
        return PNG_READ_INVALID;
    }
    if ((cmf & 0x0FU) != 8U || (((Unsigned32)cmf << 8) | (Unsigned32)flg) % 31U != 0U ||
        (flg & 0x20U) != 0U)
    {
        return PNG_READ_INVALID;
    }

    reader.source = source;
    reader.bitBuffer = 0U;
    reader.bitCount = 0U;

    do
    {
        Unsigned32 blockType;

        if (!inflateGetBits(&reader, 1U, &isFinal) || !inflateGetBits(&reader, 2U, &blockType))
        {
            return PNG_READ_INVALID;
        }

        if (blockType == 0U)
        {
            Unsigned8 lengthBytes[4];
            Unsigned32 storedLength;
            Unsigned32 copyIndex;

            inflateAlignToByte(&reader);
            for (copyIndex = 0U; copyIndex < 4U; copyIndex++)
            {
                if (!pngChunkSourceNextByte(source, &lengthBytes[copyIndex]))
                {
                    return PNG_READ_INVALID;
                }
            }
            storedLength = (Unsigned32)lengthBytes[0] | ((Unsigned32)lengthBytes[1] << 8);
            if (*outputPosition + (MemorySize)storedLength > outputCapacity)
            {
                return PNG_READ_INVALID;
            }
            for (copyIndex = 0U; copyIndex < storedLength; copyIndex++)
            {
                Unsigned8 rawByte;

                if (!pngChunkSourceNextByte(source, &rawByte))
                {
                    return PNG_READ_INVALID;
                }
                output[*outputPosition] = rawByte;
                (*outputPosition)++;
            }
        }
        else if (blockType == 1U || blockType == 2U)
        {
            DeflateHuffTable literalTable;
            DeflateHuffTable distanceTable;

            if (blockType == 1U)
            {
                if (!buildFixedLiteralTable(&literalTable) || !buildFixedDistanceTable(&distanceTable))
                {
                    return PNG_READ_INVALID;
                }
            }
            else if (!readDynamicTables(&reader, &literalTable, &distanceTable))
            {
                return PNG_READ_INVALID;
            }

            for (;;)
            {
                Unsigned32 symbol;

                if (!decodeHuffSymbol(&reader, &literalTable, &symbol))
                {
                    return PNG_READ_INVALID;
                }
                if (symbol < 256U)
                {
                    if (*outputPosition >= outputCapacity)
                    {
                        return PNG_READ_INVALID;
                    }
                    output[*outputPosition] = (Unsigned8)symbol;
                    (*outputPosition)++;
                }
                else if (symbol == 256U)
                {
                    break;
                }
                else
                {
                    Unsigned32 lengthIndex = symbol - 257U;
                    Unsigned32 lengthExtra;
                    Unsigned32 length;
                    Unsigned32 distSymbol;
                    Unsigned32 distExtra;
                    Unsigned32 distance;
                    MemorySize copySource;
                    Unsigned32 copyIndex;

                    if (lengthIndex >= 29U ||
                        !inflateGetBits(&reader, LENGTH_EXTRA_BITS[lengthIndex], &lengthExtra))
                    {
                        return PNG_READ_INVALID;
                    }
                    length = (Unsigned32)LENGTH_BASE[lengthIndex] + lengthExtra;

                    if (!decodeHuffSymbol(&reader, &distanceTable, &distSymbol) || distSymbol >= 30U ||
                        !inflateGetBits(&reader, DIST_EXTRA_BITS[distSymbol], &distExtra))
                    {
                        return PNG_READ_INVALID;
                    }
                    distance = (Unsigned32)DIST_BASE[distSymbol] + distExtra;

                    if ((MemorySize)distance > *outputPosition ||
                        *outputPosition + (MemorySize)length > outputCapacity)
                    {
                        return PNG_READ_INVALID;
                    }
                    copySource = *outputPosition - (MemorySize)distance;
                    for (copyIndex = 0U; copyIndex < length; copyIndex++)
                    {
                        output[*outputPosition] = output[copySource];
                        (*outputPosition)++;
                        copySource++;
                    }
                }
            }
        }
        else
        {
            return PNG_READ_INVALID;
        }
    } while (!isFinal);

    return PNG_READ_OK;
}

static Integer32 paethPredictor(Integer32 left, Integer32 up, Integer32 upLeft)
{
    Integer32 estimate = left + up - upLeft;
    Integer32 distanceToLeft = (estimate > left) ? estimate - left : left - estimate;
    Integer32 distanceToUp = (estimate > up) ? estimate - up : up - estimate;
    Integer32 distanceToUpLeft = (estimate > upLeft) ? estimate - upLeft : upLeft - estimate;

    if (distanceToLeft <= distanceToUp && distanceToLeft <= distanceToUpLeft)
    {
        return left;
    }
    return (distanceToUp <= distanceToUpLeft) ? up : upLeft;
}

static PngReadResult defilterScanlines(Unsigned8 *scratch, Unsigned32 height, MemorySize rowBytes,
                                       Unsigned32 bytesPerPixel)
{
    MemorySize stride = rowBytes + 1UL;
    Unsigned32 row;

    for (row = 0U; row < height; row++)
    {
        Unsigned8 filterType = scratch[(MemorySize)row * stride];
        Unsigned8 *current = scratch + (MemorySize)row * stride + 1UL;
        const Unsigned8 *prior = (row == 0U) ? NULL_POINTER
                                             : scratch + (MemorySize)(row - 1U) * stride + 1UL;
        MemorySize x;

        switch (filterType)
        {
        case 0U:
            break;
        case 1U:
            for (x = 0UL; x < rowBytes; x++)
            {
                Unsigned8 left = (x >= (MemorySize)bytesPerPixel) ? current[x - bytesPerPixel] : 0U;

                current[x] = (Unsigned8)(current[x] + left);
            }
            break;
        case 2U:
            for (x = 0UL; x < rowBytes; x++)
            {
                Unsigned8 up = (prior != NULL_POINTER) ? prior[x] : 0U;

                current[x] = (Unsigned8)(current[x] + up);
            }
            break;
        case 3U:
            for (x = 0UL; x < rowBytes; x++)
            {
                Unsigned32 left = (x >= (MemorySize)bytesPerPixel) ? current[x - bytesPerPixel] : 0U;
                Unsigned32 up = (prior != NULL_POINTER) ? prior[x] : 0U;

                current[x] = (Unsigned8)(current[x] + (left + up) / 2U);
            }
            break;
        case 4U:
            for (x = 0UL; x < rowBytes; x++)
            {
                Integer32 left = (x >= (MemorySize)bytesPerPixel) ? (Integer32)current[x - bytesPerPixel] : 0;
                Integer32 up = (prior != NULL_POINTER) ? (Integer32)prior[x] : 0;
                Integer32 upLeft = (x >= (MemorySize)bytesPerPixel && prior != NULL_POINTER)
                                       ? (Integer32)prior[x - bytesPerPixel]
                                       : 0;

                current[x] = (Unsigned8)(current[x] + paethPredictor(left, up, upLeft));
            }
            break;
        default:
            return PNG_READ_INVALID;
        }
    }
    return PNG_READ_OK;
}

static void expandToRgba(const Unsigned8 *scratch, Unsigned32 width, Unsigned32 height,
                         MemorySize rowBytes, Unsigned8 colorType, Unsigned32 channels,
                         const Unsigned8 *palette, Unsigned8 *outRgba)
{
    MemorySize stride = rowBytes + 1UL;
    Unsigned32 row;

    for (row = 0U; row < height; row++)
    {
        const Unsigned8 *sourceRow = scratch + (MemorySize)row * stride + 1UL;
        Unsigned8 *destinationRow = outRgba + (MemorySize)row * width * 4UL;
        Unsigned32 column;

        for (column = 0U; column < width; column++)
        {
            const Unsigned8 *sample = sourceRow + (MemorySize)column * channels;
            Unsigned8 *destination = destinationRow + (MemorySize)column * 4UL;
            Unsigned8 red, green, blue, alpha;

            switch (colorType)
            {
            case 0U:
                red = sample[0];
                green = sample[0];
                blue = sample[0];
                alpha = 255U;
                break;
            case 2U:
                red = sample[0];
                green = sample[1];
                blue = sample[2];
                alpha = 255U;
                break;
            case 3U:
                red = palette[(MemorySize)sample[0] * 3UL];
                green = palette[(MemorySize)sample[0] * 3UL + 1UL];
                blue = palette[(MemorySize)sample[0] * 3UL + 2UL];
                alpha = 255U;
                break;
            case 4U:
                red = sample[0];
                green = sample[0];
                blue = sample[0];
                alpha = sample[1];
                break;
            default:
                red = sample[0];
                green = sample[1];
                blue = sample[2];
                alpha = sample[3];
                break;
            }

            destination[0] = red;
            destination[1] = green;
            destination[2] = blue;
            destination[3] = alpha;
        }
    }
}

PngReadResult pngPeekDimensions(const Unsigned8 *pngData, MemorySize pngSize,
                                Unsigned32 *outWidth, Unsigned32 *outHeight,
                                MemorySize *outScratchCapacity)
{
    PngHeader header;
    PngReadResult result;

    *outWidth = 0U;
    *outHeight = 0U;
    *outScratchCapacity = 0UL;

    result = pngParseHeader(pngData, pngSize, &header);
    if (result != PNG_READ_OK)
    {
        return result;
    }
    *outWidth = header.width;
    *outHeight = header.height;
    *outScratchCapacity = header.filteredSize;
    return PNG_READ_OK;
}

PngReadResult pngReadToRgba(const Unsigned8 *pngData, MemorySize pngSize,
                            Unsigned8 *outRgba, MemorySize outRgbaCapacity,
                            Unsigned8 *scratch, MemorySize scratchCapacity,
                            Unsigned32 *outWidth, Unsigned32 *outHeight)
{
    PngHeader header;
    PngReadResult result;
    PngChunkSource source;
    MemorySize decodedBytes = 0UL;
    MemorySize requiredRgba;

    *outWidth = 0U;
    *outHeight = 0U;

    result = pngParseHeader(pngData, pngSize, &header);
    if (result != PNG_READ_OK)
    {
        return result;
    }

    requiredRgba = (MemorySize)header.width * (MemorySize)header.height * 4UL;
    if (outRgba == NULL_POINTER || outRgbaCapacity < requiredRgba ||
        scratch == NULL_POINTER || scratchCapacity < header.filteredSize)
    {
        return PNG_READ_INVALID;
    }

    source.fileData = pngData;
    source.fileSize = pngSize;
    source.chunkCursor = PNG_SIGNATURE_SIZE;
    source.idatData = NULL_POINTER;
    source.idatRemaining = 0UL;

    result = inflateStream(&source, scratch, header.filteredSize, &decodedBytes);
    if (result != PNG_READ_OK || decodedBytes != header.filteredSize)
    {
        return PNG_READ_INVALID;
    }

    result = defilterScanlines(scratch, header.height, header.rowBytes, header.channels);
    if (result != PNG_READ_OK)
    {
        return result;
    }

    expandToRgba(scratch, header.width, header.height, header.rowBytes, header.colorType,
                 header.channels, header.palette, outRgba);

    *outWidth = header.width;
    *outHeight = header.height;
    return PNG_READ_OK;
}
