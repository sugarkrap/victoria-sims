#include "utils/resourceHash.h"

#include "utils/strings.h"

#define CRC24_POLYNOMIAL 0x864CFBUL
#define CRC24_INITIAL 0xB704CEUL

#define CRC32_POLYNOMIAL 0x04C11DB7UL
#define CRC32_INITIAL 0xFFFFFFFFUL

static Unsigned32 computeCrc24(const char *name)
{
    Unsigned32 remainder = (Unsigned32)CRC24_INITIAL;
    MemorySize index;

    if (name == NULL_POINTER)
    {
        return 0U;
    }
    for (index = 0UL; name[index] != '\0'; index++)
    {
        Unsigned32 bit;

        remainder ^= (Unsigned32)((Unsigned8)characterToLowerCase(name[index])) << 16;
        for (bit = 0U; bit < 8U; bit++)
        {
            remainder <<= 1;
            if ((remainder & 0x01000000UL) != 0U)
            {
                remainder ^= (Unsigned32)CRC24_POLYNOMIAL;
            }
        }
    }
    return remainder & 0x00FFFFFFUL;
}

static Unsigned32 computeCrc32Mpeg2(const char *name)
{
    Unsigned32 remainder = (Unsigned32)CRC32_INITIAL;
    MemorySize index;

    if (name == NULL_POINTER)
    {
        return 0U;
    }
    for (index = 0UL; name[index] != '\0'; index++)
    {
        Unsigned32 bit;

        remainder ^= (Unsigned32)((Unsigned8)characterToLowerCase(name[index])) << 24;
        for (bit = 0U; bit < 8U; bit++)
        {
            if ((remainder & 0x80000000UL) != 0U)
            {
                remainder = (remainder << 1) ^ (Unsigned32)CRC32_POLYNOMIAL;
            }
            else
            {
                remainder <<= 1;
            }
        }
    }
    return remainder;
}

Unsigned32 resourceHashInstance(const char *name)
{
    return computeCrc24(name) | 0xFF000000UL;
}

Unsigned32 resourceHashInstanceHigh(const char *name)
{
    return computeCrc32Mpeg2(name);
}

Unsigned32 resourceHashGroup(const char *name)
{
    return computeCrc24(name) | 0x7F000000UL;
}
