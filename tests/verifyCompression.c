
#include <stdio.h>

#include "utils/assert.h"
#include "victoria/compression.h"

#define OUTPUT_CAPACITY 4096UL

static Integer32 failureCount = 0;
static Unsigned8 output[OUTPUT_CAPACITY];

static MemorySize buildStream(Unsigned8 *stream, const Unsigned8 *body, MemorySize bodySize,
                              MemorySize decompressedSize)
{
    MemorySize index;

    stream[0] = 0U;
    stream[1] = 0U;
    stream[2] = 0U;
    stream[3] = 0U;
    stream[4] = 0x10U;
    stream[5] = 0xFBU;
    stream[6] = (Unsigned8)((decompressedSize >> 16) & 0xFFU);
    stream[7] = (Unsigned8)((decompressedSize >> 8) & 0xFFU);
    stream[8] = (Unsigned8)(decompressedSize & 0xFFU);
    for (index = 0UL; index < bodySize; index++)
    {
        stream[9UL + index] = body[index];
    }
    return 9UL + bodySize;
}

static Boolean outputMatches(const char *expected, MemorySize length)
{
    MemorySize index;

    for (index = 0UL; index < length; index++)
    {
        if (output[index] != (Unsigned8)expected[index])
        {
            return BOOLEAN_FALSE;
        }
    }
    return BOOLEAN_TRUE;
}

int main(void)
{
    Unsigned8 stream[256];
    MemorySize streamSize;
    MemorySize written = 0UL;
    CompressionResult result;

    printf("-- recognising a compressed resource --\n");
    {
        static const Unsigned8 body[] = { 0xFC };

        streamSize = buildStream(stream, body, sizeof(body), 0UL);
        checkThat(&failureCount, "spots the RefPack signature",
                  compressionLooksLikeRefPack(stream, streamSize));
        checkThat(&failureCount, "and does not spot it in a DBPF header",
                  !compressionLooksLikeRefPack((const Unsigned8 *)"DBPF\0\0\0\0\0\0", 10UL));
        checkThat(&failureCount, "reads the decompressed size from the header",
                  compressionGetDecompressedSize(stream, streamSize) == 0UL);
    }

    printf("\n-- literals only --\n");
    {
        static const Unsigned8 body[] = { 0xE0, 'V', 'i', 'c', 'k', 0xFC };

        streamSize = buildStream(stream, body, sizeof(body), 4UL);
        result = compressionDecompressRefPack(output, OUTPUT_CAPACITY, stream, streamSize, &written);
        checkThat(&failureCount, "decodes a literal run", result == COMPRESSION_OK);
        checkThat(&failureCount, "of the right length", written == 4UL);
        checkThat(&failureCount, "with the right bytes", outputMatches("Vick", 4UL));
    }

    printf("\n-- a short back reference --\n");
    {
        static const Unsigned8 body[] = { 0xE0, 'a', 'b', 'c', 'd',
                                          0x0B, 0x03, 'e', 'f', 'g',
                                          0xFC };

        streamSize = buildStream(stream, body, sizeof(body), 12UL);
        result = compressionDecompressRefPack(output, OUTPUT_CAPACITY, stream, streamSize, &written);
        checkThat(&failureCount, "decodes a two byte control", result == COMPRESSION_OK);
        checkThat(&failureCount, "producing twelve bytes", written == 12UL);
        checkThat(&failureCount, "with the run copied from earlier output",
                  outputMatches("abcdefgdefgd", 12UL));
    }

    printf("\n-- a run that overlaps itself --\n");
    {
        static const Unsigned8 body[] = { 0x0D, 0x00, 'z', 0xFC };

        streamSize = buildStream(stream, body, sizeof(body), 7UL);
        result = compressionDecompressRefPack(output, OUTPUT_CAPACITY, stream, streamSize, &written);
        checkThat(&failureCount, "decodes an overlapping run", result == COMPRESSION_OK);
        checkThat(&failureCount, "repeating the byte", written == 7UL && outputMatches("zzzzzzz", 7UL));
    }

    printf("\n-- the three byte control --\n");
    {
        static const Unsigned8 body[] = { 0xE0, 'h', 'e', 'l', 'p',
                                          0x84, 0x80, 0x03, 'x', 'y',
                                          0xFC };

        streamSize = buildStream(stream, body, sizeof(body), 14UL);
        result = compressionDecompressRefPack(output, OUTPUT_CAPACITY, stream, streamSize, &written);
        checkThat(&failureCount, "decodes a three byte control", result == COMPRESSION_OK);
        checkThat(&failureCount, "with the longer run",
                  written == 14UL && outputMatches("helpxylpxylpxy", 14UL));
    }

    printf("\n-- refusing what it should --\n");
    {
        static const Unsigned8 truncated[] = { 0xE0, 'a', 'b' };
        static const Unsigned8 backTooFar[] = { 0x0B, 0x03, 'e', 'f', 'g', 0xFC };
        static const Unsigned8 plenty[] = { 0xE0, 'a', 'b', 'c', 'd', 0xFC };

        checkThat(&failureCount, "rejects bytes with no signature",
                  compressionDecompressRefPack(output, OUTPUT_CAPACITY,
                                               (const Unsigned8 *)"not compressed at all", 21UL,
                                               &written) == COMPRESSION_NOT_COMPRESSED);

        streamSize = buildStream(stream, truncated, sizeof(truncated), 4UL);
        checkThat(&failureCount, "rejects a run of literals that is not all there",
                  compressionDecompressRefPack(output, OUTPUT_CAPACITY, stream, streamSize,
                                               &written) == COMPRESSION_TRUNCATED);

        streamSize = buildStream(stream, backTooFar, sizeof(backTooFar), 8UL);
        checkThat(&failureCount, "rejects a reference pointing before the output",
                  compressionDecompressRefPack(output, OUTPUT_CAPACITY, stream, streamSize,
                                               &written) == COMPRESSION_BAD_REFERENCE);

        streamSize = buildStream(stream, plenty, sizeof(plenty), 4UL);
        checkThat(&failureCount, "refuses to write into somewhere too small",
                  compressionDecompressRefPack(output, 2UL, stream, streamSize, &written) ==
                      COMPRESSION_DESTINATION_TOO_SMALL);

        streamSize = buildStream(stream, plenty, sizeof(plenty), 99UL);
        checkThat(&failureCount, "rejects a stream shorter than its header promises",
                  compressionDecompressRefPack(output, OUTPUT_CAPACITY, stream, streamSize,
                                               &written) == COMPRESSION_TRUNCATED);
    }

    return checkSummarize(failureCount, "compression");
}
