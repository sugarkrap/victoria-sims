#include "utils/checksum.h"

/* The polynomial in its reflected form, which is how it is written when the
   input bits are fed in least significant first. */
#define CRC32_REFLECTED_POLYNOMIAL 0xEDB88320UL

Unsigned32 checksumCrc32(const Unsigned8 *bytes, MemorySize byteCount)
{
    Unsigned32 remainder = 0xFFFFFFFFUL;
    MemorySize index;

    /* Bitwise rather than table driven. The tables this checks are tens of
       bytes long, and a kibibyte of lookup table to save a few hundred shifts
       is a budget spent on nothing. */
    for (index = 0UL; index < byteCount; index += 1UL)
    {
        Unsigned32 bit;

        remainder ^= (Unsigned32)bytes[index];
        for (bit = 0UL; bit < 8UL; bit += 1UL)
        {
            if ((remainder & 1UL) != 0UL)
            {
                remainder = (remainder >> 1) ^ (Unsigned32)CRC32_REFLECTED_POLYNOMIAL;
            }
            else
            {
                remainder >>= 1;
            }
        }
    }
    return remainder ^ 0xFFFFFFFFUL;
}
