#include "victoria/uiLayoutReader.h"

#include "utils/strings.h"

static Boolean isLineSpace(char character)
{
    return (character == ' ' || character == '\t' || character == '\r') ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

static Boolean spanEquals(const char *text, MemorySize start, MemorySize end, const char *literal)
{
    MemorySize length = end - start;
    MemorySize index;

    for (index = 0UL; index < length; index++)
    {
        if (literal[index] == '\0' || text[start + index] != literal[index])
        {
            return BOOLEAN_FALSE;
        }
    }
    return (literal[length] == '\0') ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

static Boolean spanStartsWith(const char *text, MemorySize start, MemorySize end, const char *literal)
{
    MemorySize literalLength = stringLength(literal);

    if (end - start < literalLength)
    {
        return BOOLEAN_FALSE;
    }
    return spanEquals(text, start, start + literalLength, literal);
}

static void copySpan(char *destination, MemorySize capacity, const char *text, MemorySize start, MemorySize end)
{
    MemorySize length = end - start;
    MemorySize index;

    if (length > capacity - 1UL)
    {
        length = capacity - 1UL;
    }
    for (index = 0UL; index < length; index++)
    {
        destination[index] = text[start + index];
    }
    destination[length] = '\0';
}

static void appendSpanWithSpace(char *destination, MemorySize capacity, const char *text, MemorySize start,
                                MemorySize end)
{
    MemorySize length = stringLength(destination);
    MemorySize spanLength = end - start;
    MemorySize index;

    if (length + 1UL >= capacity)
    {
        return;
    }
    destination[length] = ' ';
    length++;
    if (spanLength > capacity - 1UL - length)
    {
        spanLength = capacity - 1UL - length;
    }
    for (index = 0UL; index < spanLength; index++)
    {
        destination[length + index] = text[start + index];
    }
    destination[length + spanLength] = '\0';
}

static Unsigned32 spanParseHexadecimal(const char *text, MemorySize start, MemorySize end)
{
    Unsigned32 value = 0UL;
    MemorySize index = start;

    if (index + 1UL < end && text[index] == '0' && (text[index + 1UL] == 'x' || text[index + 1UL] == 'X'))
    {
        index += 2UL;
    }
    while (index < end)
    {
        char character = text[index];
        Unsigned32 digit;

        if (character >= '0' && character <= '9')
        {
            digit = (Unsigned32)(character - '0');
        }
        else if (character >= 'a' && character <= 'f')
        {
            digit = (Unsigned32)(character - 'a') + 10UL;
        }
        else if (character >= 'A' && character <= 'F')
        {
            digit = (Unsigned32)(character - 'A') + 10UL;
        }
        else
        {
            break;
        }
        value = (value << 4) | digit;
        index++;
    }
    return value;
}

static Integer32 spanParseSignedInteger(const char *text, MemorySize start, MemorySize end)
{
    Integer32 sign = 1;
    Integer32 value = 0;
    MemorySize index = start;

    if (index < end && text[index] == '-')
    {
        sign = -1;
        index++;
    }
    while (index < end && text[index] >= '0' && text[index] <= '9')
    {
        value = (value * 10) + (Integer32)(text[index] - '0');
        index++;
    }
    return value * sign;
}

static Unsigned32 spanParseSignedIntegerList(const char *text, MemorySize start, MemorySize end,
                                             char openBracket, char closeBracket, Integer32 *values,
                                             Unsigned32 maxCount)
{
    MemorySize index = start;
    Unsigned32 count = 0UL;

    if (index < end && text[index] == openBracket)
    {
        index++;
    }
    if (end > index && text[end - 1UL] == closeBracket)
    {
        end--;
    }
    while (index < end)
    {
        MemorySize fieldStart = index;

        while (index < end && text[index] != ',')
        {
            index++;
        }
        if (count < maxCount)
        {
            values[count] = spanParseSignedInteger(text, fieldStart, index);
            count++;
        }
        if (index < end)
        {
            index++;
        }
    }
    return count;
}

static Unsigned32 spanParseHexadecimalList(const char *text, MemorySize start, MemorySize end,
                                           Unsigned32 *values, Unsigned32 maxCount)
{
    MemorySize index = start;
    Unsigned32 count = 0UL;

    if (index < end && text[index] == '{')
    {
        index++;
    }
    if (end > index && text[end - 1UL] == '}')
    {
        end--;
    }
    while (index < end)
    {
        MemorySize fieldStart = index;

        while (index < end && text[index] != ',')
        {
            index++;
        }
        if (count < maxCount)
        {
            values[count] = spanParseHexadecimal(text, fieldStart, index);
            count++;
        }
        if (index < end)
        {
            index++;
        }
    }
    return count;
}

static void initializeElement(UIElement *element)
{
    element->className[0] = '\0';
    element->hasID = BOOLEAN_FALSE;
    element->id = 0UL;
    element->areaLeft = 0;
    element->areaTop = 0;
    element->areaRight = 0;
    element->areaBottom = 0;
    element->hasFillColor = BOOLEAN_FALSE;
    element->fillRed = 0U;
    element->fillGreen = 0U;
    element->fillBlue = 0U;
    element->noFill = BOOLEAN_FALSE;
    element->hasCaption = BOOLEAN_FALSE;
    element->caption[0] = '\0';
    element->hasImage = BOOLEAN_FALSE;
    element->imageNumberCount = 0UL;
    element->imageNumbers[0] = 0UL;
    element->imageNumbers[1] = 0UL;
    element->imageNumbers[2] = 0UL;
    element->visible = BOOLEAN_TRUE;
    element->enabled = BOOLEAN_TRUE;
    element->parentIndex = -1;
}

static void applyAttribute(UIElement *element, const char *text, MemorySize keyStart, MemorySize keyEnd,
                           MemorySize valueStart, MemorySize valueEnd, Boolean *previousWasClassName)
{
    Boolean isClassName = BOOLEAN_FALSE;

    if (valueEnd > valueStart && text[valueStart] == '"' && text[valueEnd - 1UL] == '"')
    {
        valueStart++;
        valueEnd--;
    }

    if (spanEquals(text, keyStart, keyEnd, "clsid"))
    {
        copySpan(element->className, UI_LAYOUT_CLASS_NAME_LIMIT, text, valueStart, valueEnd);
        isClassName = BOOLEAN_TRUE;
    }
    else if (spanEquals(text, keyStart, keyEnd, "id"))
    {
        element->hasID = BOOLEAN_TRUE;
        element->id = spanParseHexadecimal(text, valueStart, valueEnd);
    }
    else if (spanEquals(text, keyStart, keyEnd, "area"))
    {
        Integer32 values[4];

        if (spanParseSignedIntegerList(text, valueStart, valueEnd, '(', ')', values, 4U) == 4UL)
        {
            element->areaLeft = values[0];
            element->areaTop = values[1];
            element->areaRight = values[2];
            element->areaBottom = values[3];
        }
    }
    else if (spanEquals(text, keyStart, keyEnd, "fillcolor"))
    {
        Integer32 values[3];

        if (spanParseSignedIntegerList(text, valueStart, valueEnd, '(', ')', values, 3U) == 3UL)
        {
            element->hasFillColor = BOOLEAN_TRUE;
            element->fillRed = (Unsigned8)values[0];
            element->fillGreen = (Unsigned8)values[1];
            element->fillBlue = (Unsigned8)values[2];
        }
    }
    else if (spanEquals(text, keyStart, keyEnd, "caption"))
    {
        copySpan(element->caption, UI_LAYOUT_CAPTION_LIMIT, text, valueStart, valueEnd);
        element->hasCaption = BOOLEAN_TRUE;
    }
    else if (spanEquals(text, keyStart, keyEnd, "winflag_visible"))
    {
        element->visible = spanEquals(text, valueStart, valueEnd, "yes") ? BOOLEAN_TRUE : BOOLEAN_FALSE;
    }
    else if (spanEquals(text, keyStart, keyEnd, "winflag_enabled"))
    {
        element->enabled = spanEquals(text, valueStart, valueEnd, "yes") ? BOOLEAN_TRUE : BOOLEAN_FALSE;
    }
    else if (spanEquals(text, keyStart, keyEnd, "style"))
    {
        element->noFill = spanEquals(text, valueStart, valueEnd, "nofill") ? BOOLEAN_TRUE : BOOLEAN_FALSE;
    }
    else if (spanEquals(text, keyStart, keyEnd, "image"))
    {
        Unsigned32 values[3];
        Unsigned32 count = spanParseHexadecimalList(text, valueStart, valueEnd, values, 3U);

        if (count > 0UL)
        {
            Unsigned32 index;

            element->hasImage = BOOLEAN_TRUE;
            element->imageNumberCount = count;
            for (index = 0UL; index < count; index++)
            {
                element->imageNumbers[index] = values[index];
            }
        }
    }

    *previousWasClassName = isClassName;
}

static void parseAttributes(const char *text, MemorySize start, MemorySize end, UIElement *element)
{
    MemorySize index = start;
    Boolean previousWasClassName = BOOLEAN_FALSE;

    while (index < end)
    {
        MemorySize tokenStart;
        MemorySize tokenEnd;
        MemorySize equalsPosition;
        Boolean hasEquals;
        Boolean inQuote;

        while (index < end && isLineSpace(text[index]))
        {
            index++;
        }
        if (index >= end)
        {
            break;
        }
        tokenStart = index;
        inQuote = BOOLEAN_FALSE;
        while (index < end)
        {
            if (text[index] == '"')
            {
                inQuote = (inQuote == BOOLEAN_TRUE) ? BOOLEAN_FALSE : BOOLEAN_TRUE;
            }
            else if (inQuote == BOOLEAN_FALSE && isLineSpace(text[index]))
            {
                break;
            }
            index++;
        }
        tokenEnd = index;

        hasEquals = BOOLEAN_FALSE;
        equalsPosition = tokenStart;
        while (equalsPosition < tokenEnd)
        {
            if (text[equalsPosition] == '=')
            {
                hasEquals = BOOLEAN_TRUE;
                break;
            }
            equalsPosition++;
        }

        if (hasEquals == BOOLEAN_TRUE)
        {
            applyAttribute(element, text, tokenStart, equalsPosition, equalsPosition + 1UL, tokenEnd,
                           &previousWasClassName);
        }
        else if (previousWasClassName == BOOLEAN_TRUE)
        {
            appendSpanWithSpace(element->className, UI_LAYOUT_CLASS_NAME_LIMIT, text, tokenStart, tokenEnd);
        }
    }
}

const char *uiLayoutReadResultGetName(UILayoutReadResult result)
{
    switch (result)
    {
    case UI_LAYOUT_READ_OK:
        return "read";
    case UI_LAYOUT_READ_NO_ELEMENTS:
        return "named no LEGACY element at all";
    case UI_LAYOUT_READ_UNBALANCED_CHILDREN:
        return "a CHILDREN that does not close, or that closes one never opened";
    case UI_LAYOUT_READ_TOO_DEEPLY_NESTED:
        return "nested past what this reader tracks";
    default:
        return "an unnamed result";
    }
}

UILayoutReadResult uiLayoutRead(UILayoutDescription *description, const Unsigned8 *bytes,
                                MemorySize sizeInBytes)
{
    const char *text = (const char *)bytes;
    MemorySize position = 0UL;
    Integer32 parentStack[UI_LAYOUT_NESTING_LIMIT];
    Unsigned32 stackDepth = 0U;
    Integer32 currentParent = -1;
    Integer32 lastElementIndex = -1;

    description->elementCount = 0U;
    description->elementsOverflow = 0U;

    if (bytes == NULL_POINTER)
    {
        return UI_LAYOUT_READ_NO_ELEMENTS;
    }

    while (position < sizeInBytes)
    {
        MemorySize lineStart = position;
        MemorySize lineEnd = lineStart;
        MemorySize trimmedStart;
        MemorySize trimmedEnd;

        while (lineEnd < sizeInBytes && text[lineEnd] != '\n')
        {
            lineEnd++;
        }
        position = (lineEnd < sizeInBytes) ? lineEnd + 1UL : lineEnd;

        trimmedStart = lineStart;
        trimmedEnd = lineEnd;
        while (trimmedEnd > trimmedStart && isLineSpace(text[trimmedEnd - 1UL]))
        {
            trimmedEnd--;
        }
        while (trimmedStart < trimmedEnd && isLineSpace(text[trimmedStart]))
        {
            trimmedStart++;
        }
        if (trimmedStart >= trimmedEnd || text[trimmedStart] == '#')
        {
            continue;
        }

        if (spanEquals(text, trimmedStart, trimmedEnd, "<CHILDREN>"))
        {
            if (lastElementIndex == -1)
            {
                return UI_LAYOUT_READ_UNBALANCED_CHILDREN;
            }
            if (stackDepth >= UI_LAYOUT_NESTING_LIMIT)
            {
                return UI_LAYOUT_READ_TOO_DEEPLY_NESTED;
            }
            parentStack[stackDepth] = currentParent;
            stackDepth++;
            currentParent = lastElementIndex;
            continue;
        }
        if (spanEquals(text, trimmedStart, trimmedEnd, "</CHILDREN>"))
        {
            if (stackDepth == 0U)
            {
                return UI_LAYOUT_READ_UNBALANCED_CHILDREN;
            }
            stackDepth--;
            currentParent = parentStack[stackDepth];
            continue;
        }
        if (spanStartsWith(text, trimmedStart, trimmedEnd, "<LEGACY") && text[trimmedEnd - 1UL] == '>')
        {
            if (currentParent == -2 || description->elementCount >= UI_LAYOUT_ELEMENT_LIMIT)
            {
                description->elementsOverflow++;
                lastElementIndex = -2;
            }
            else
            {
                UIElement *element = &description->elements[description->elementCount];

                initializeElement(element);
                element->parentIndex = currentParent;
                parseAttributes(text, trimmedStart + 7UL, trimmedEnd - 1UL, element);
                lastElementIndex = (Integer32)description->elementCount;
                description->elementCount++;
            }
            continue;
        }
    }

    if (stackDepth != 0U)
    {
        return UI_LAYOUT_READ_UNBALANCED_CHILDREN;
    }
    if (description->elementCount == 0U)
    {
        return UI_LAYOUT_READ_NO_ELEMENTS;
    }
    return UI_LAYOUT_READ_OK;
}

void uiLayoutGetAbsoluteArea(const UILayoutDescription *description, Unsigned32 elementIndex,
                             Integer32 *left, Integer32 *top, Integer32 *right, Integer32 *bottom)
{
    Integer32 width = description->elements[elementIndex].areaRight - description->elements[elementIndex].areaLeft;
    Integer32 height = description->elements[elementIndex].areaBottom - description->elements[elementIndex].areaTop;
    Integer32 offsetLeft = 0;
    Integer32 offsetTop = 0;
    Integer32 index = (Integer32)elementIndex;

    while (index >= 0)
    {
        const UIElement *element = &description->elements[(Unsigned32)index];

        offsetLeft += element->areaLeft;
        offsetTop += element->areaTop;
        index = element->parentIndex;
    }

    *left = offsetLeft;
    *top = offsetTop;
    *right = offsetLeft + width;
    *bottom = offsetTop + height;
}

Boolean uiLayoutIsVisible(const UILayoutDescription *description, Unsigned32 elementIndex)
{
    Integer32 index = (Integer32)elementIndex;

    while (index >= 0)
    {
        const UIElement *element = &description->elements[(Unsigned32)index];

        if (element->visible == BOOLEAN_FALSE)
        {
            return BOOLEAN_FALSE;
        }
        index = element->parentIndex;
    }
    return BOOLEAN_TRUE;
}
