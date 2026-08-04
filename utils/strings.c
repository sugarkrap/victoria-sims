#include "utils/strings.h"

MemorySize stringLength(const char *text)
{
    MemorySize length = 0UL;
    while (text[length] != '\0')
    {
        length += 1UL;
    }
    return length;
}

Boolean stringEquals(const char *first, const char *second)
{
    MemorySize index = 0UL;

    while (first[index] != '\0' && first[index] == second[index])
    {
        index += 1UL;
    }
    return (Boolean)(first[index] == second[index]);
}

Boolean stringStartsWith(const char *text, const char *prefix)
{
    MemorySize index = 0UL;

    while (prefix[index] != '\0')
    {
        if (text[index] != prefix[index])
        {
            return BOOLEAN_FALSE;
        }
        index += 1UL;
    }
    return BOOLEAN_TRUE;
}

MemorySize stringParseUnsigned(const char *text)
{
    MemorySize value = 0UL;
    MemorySize index = 0UL;

    while (text[index] >= '0' && text[index] <= '9')
    {
        value = (value * 10UL) + (MemorySize)(text[index] - '0');
        index += 1UL;
    }
    return value;
}

MemorySize stringWriteUnsigned(char *destination, MemorySize destinationCapacity, Unsigned64 value)
{
    char reversedDigits[24];
    MemorySize digitCount = 0UL;
    MemorySize index;

    do
    {
        reversedDigits[digitCount] = (char)('0' + (char)(value % 10ULL));
        digitCount += 1UL;
        value /= 10ULL;
    } while (value != 0ULL && digitCount < VICTORIA_ARRAY_LENGTH(reversedDigits));

    if (destinationCapacity < digitCount + 1UL)
    {
        if (destinationCapacity > 0UL)
        {
            destination[0] = '\0';
        }
        return 0UL;
    }

    for (index = 0UL; index < digitCount; index += 1UL)
    {
        destination[index] = reversedDigits[digitCount - 1UL - index];
    }
    destination[digitCount] = '\0';
    return digitCount;
}

MemorySize stringAppend(char *destination, MemorySize destinationCapacity, const char *source)
{
    MemorySize destinationLength = stringLength(destination);
    MemorySize sourceIndex = 0UL;

    while (source[sourceIndex] != '\0' && destinationLength + 1UL < destinationCapacity)
    {
        destination[destinationLength] = source[sourceIndex];
        destinationLength += 1UL;
        sourceIndex += 1UL;
    }

    if (destinationCapacity > 0UL)
    {
        destination[destinationLength] = '\0';
    }
    return destinationLength;
}

char characterToLowerCase(char character)
{
    if (character >= 'A' && character <= 'Z')
    {
        return (char)(character - 'A' + 'a');
    }
    return character;
}
