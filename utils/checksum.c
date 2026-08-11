#include "utils/checksum.h"

#define CRC32_REFLECTED_POLYNOMIAL 0xEDB88320UL

Unsigned32 checksumCrc32(const Unsigned8 *bytes, MemorySize byteCount)
{
    Unsigned32 remainder = 0xFFFFFFFFUL;
    MemorySize index;

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
