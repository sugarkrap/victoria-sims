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

Boolean stringEndsWithIgnoringCase(const char *text, const char *suffix)
{
    MemorySize textLength = stringLength(text);
    MemorySize suffixLength = stringLength(suffix);
    MemorySize index;

    if (textLength < suffixLength)
    {
        return BOOLEAN_FALSE;
    }
    for (index = 0UL; index < suffixLength; index += 1UL)
    {
        if (characterToLowerCase(text[textLength - suffixLength + index]) !=
            characterToLowerCase(suffix[index]))
        {
            return BOOLEAN_FALSE;
        }
    }
    return BOOLEAN_TRUE;
}

Boolean stringContainsIgnoringCase(const char *text, const char *needle)
{
    MemorySize textLength = stringLength(text);
    MemorySize needleLength = stringLength(needle);
    MemorySize at;

    if (needleLength == 0UL)
    {
        return BOOLEAN_TRUE;
    }
    if (textLength < needleLength)
    {
        return BOOLEAN_FALSE;
    }
    for (at = 0UL; at + needleLength <= textLength; at += 1UL)
    {
        MemorySize index;

        for (index = 0UL; index < needleLength; index += 1UL)
        {
            if (characterToLowerCase(text[at + index]) != characterToLowerCase(needle[index]))
            {
                break;
            }
        }
        if (index == needleLength)
        {
            return BOOLEAN_TRUE;
        }
    }
    return BOOLEAN_FALSE;
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

MemorySize stringWriteHexadecimal(char *destination, MemorySize destinationCapacity, Unsigned64 value,
                                  MemorySize digitCount)
{
    /* Upper case, because every reference that quotes these — the format notes,
       the tools, the wiki — writes them that way, and a log line that has to be
       case folded before it can be searched for is a log line that will not be. */
    static const char digits[16] = { '0', '1', '2', '3', '4', '5', '6', '7',
                                     '8', '9', 'A', 'B', 'C', 'D', 'E', 'F' };
    MemorySize index;

    if (digitCount == 0UL || digitCount > 16UL)
    {
        digitCount = 8UL;
    }
    if (destinationCapacity < digitCount + 3UL)
    {
        if (destinationCapacity > 0UL)
        {
            destination[0] = '\0';
        }
        return 0UL;
    }

    destination[0] = '0';
    destination[1] = 'x';
    for (index = 0UL; index < digitCount; index += 1UL)
    {
        MemorySize shift = (digitCount - 1UL - index) * 4UL;

        destination[2UL + index] = digits[(MemorySize)((value >> shift) & 0xFULL)];
    }
    destination[digitCount + 2UL] = '\0';
    return digitCount + 2UL;
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
