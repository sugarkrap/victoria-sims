#include "victoria/propertySet.h"

#include "utils/strings.h"

const char *propertySetReadResultGetName(PropertySetReadResult result)
{
    switch (result)
    {
    case PROPERTY_SET_OK:
        return "ok";
    case PROPERTY_SET_NOT_BINARY:
        return "not the binary form, so probably the XML one";
    case PROPERTY_SET_TRUNCATED:
        return "the resource ends part way through";
    case PROPERTY_SET_IMPLAUSIBLE_COUNT:
        return "more properties than the resource has room for";
    default:
        return "unknown";
    }
}

typedef struct Cursor
{
    const Unsigned8 *bytes;
    MemorySize size;
    MemorySize position;
    Boolean overran;
} Cursor;

static Unsigned8 readByte(Cursor *cursor)
{
    if (cursor->position >= cursor->size)
    {
        cursor->overran = BOOLEAN_TRUE;
        return 0U;
    }
    return cursor->bytes[cursor->position++];
}

static Unsigned32 readUnsigned32(Cursor *cursor)
{
    Unsigned32 value = (Unsigned32)readByte(cursor);

    value |= (Unsigned32)readByte(cursor) << 8;
    value |= (Unsigned32)readByte(cursor) << 16;
    value |= (Unsigned32)readByte(cursor) << 24;
    return value;
}

static Unsigned32 readUnsigned16(Cursor *cursor)
{
    Unsigned32 value = (Unsigned32)readByte(cursor);

    value |= (Unsigned32)readByte(cursor) << 8;
    return value;
}

static Real32 readReal32(Cursor *cursor)
{
    union
    {
        Unsigned32 word;
        Real32 value;
    } converter;

    converter.word = readUnsigned32(cursor);
    return converter.value;
}

static void readString(Cursor *cursor, char *destination, MemorySize capacity, Boolean *truncated)
{
    Unsigned32 length = readUnsigned32(cursor);
    Unsigned32 index;

    if (destination != NULL_POINTER && capacity > 0UL)
    {
        destination[0] = '\0';
    }
    if (truncated != NULL_POINTER)
    {
        *truncated = BOOLEAN_FALSE;
    }
    if (cursor->overran || (MemorySize)length > cursor->size - cursor->position)
    {
        cursor->overran = BOOLEAN_TRUE;
        return;
    }

    for (index = 0U; index < length; index++)
    {
        Unsigned8 character = readByte(cursor);

        if (destination != NULL_POINTER && (MemorySize)index + 1UL < capacity)
        {
            destination[index] = (char)character;
            destination[index + 1UL] = '\0';
        }
        else if (truncated != NULL_POINTER)
        {
            *truncated = BOOLEAN_TRUE;
        }
    }
}

PropertySetReadResult propertySetRead(PropertySet *set, const Unsigned8 *bytes, MemorySize sizeInBytes)
{
    Cursor cursor;
    Unsigned32 index;

    set->version = 0U;
    set->propertyCount = 0U;
    set->storedPropertyCount = 0U;
    if (set->properties == NULL_POINTER || set->propertyCapacity == 0U || bytes == NULL_POINTER)
    {
        return PROPERTY_SET_TRUNCATED;
    }

    cursor.bytes = bytes;
    cursor.size = sizeInBytes;
    cursor.position = 0UL;
    cursor.overran = BOOLEAN_FALSE;

    if (readUnsigned32(&cursor) != (Unsigned32)PROPERTY_SET_CPF_MAGIC)
    {
        return PROPERTY_SET_NOT_BINARY;
    }
    set->version = readUnsigned16(&cursor);
    set->propertyCount = readUnsigned32(&cursor);
    if (cursor.overran)
    {
        return PROPERTY_SET_TRUNCATED;
    }
    if ((MemorySize)set->propertyCount > sizeInBytes / 9UL)
    {
        return PROPERTY_SET_IMPLAUSIBLE_COUNT;
    }

    for (index = 0U; index < set->propertyCount; index++)
    {
        Unsigned32 fieldType = readUnsigned32(&cursor);
        Property *property;

        if (cursor.overran)
        {
            break;
        }
        property = (set->storedPropertyCount < set->propertyCapacity)
                       ? &set->properties[set->storedPropertyCount]
                       : NULL_POINTER;

        {
            char scratchName[PROPERTY_NAME_LIMIT];
            char *nameTarget = (property != NULL_POINTER) ? property->name : scratchName;

            readString(&cursor, nameTarget, PROPERTY_NAME_LIMIT, NULL_POINTER);
        }

        switch (fieldType)
        {
        case (Unsigned32)PROPERTY_SET_TYPE_INTEGER:
        case (Unsigned32)PROPERTY_SET_TYPE_INTEGER_AGAIN:
        {
            Unsigned32 value = readUnsigned32(&cursor);

            if (property != NULL_POINTER)
            {
                property->kind = PROPERTY_VALUE_INTEGER;
                property->integerValue = value;
                property->realValue = 0.0f;
                property->stringValue[0] = '\0';
                property->stringWasTruncated = BOOLEAN_FALSE;
            }
            break;
        }
        case (Unsigned32)PROPERTY_SET_TYPE_REAL:
        {
            Real32 value = readReal32(&cursor);

            if (property != NULL_POINTER)
            {
                property->kind = PROPERTY_VALUE_REAL;
                property->realValue = value;
                property->integerValue = 0U;
                property->stringValue[0] = '\0';
                property->stringWasTruncated = BOOLEAN_FALSE;
            }
            break;
        }
        case (Unsigned32)PROPERTY_SET_TYPE_BOOLEAN:
        {
            Unsigned32 value = (Unsigned32)readByte(&cursor);

            if (property != NULL_POINTER)
            {
                property->kind = PROPERTY_VALUE_BOOLEAN;
                property->integerValue = value;
                property->realValue = 0.0f;
                property->stringValue[0] = '\0';
                property->stringWasTruncated = BOOLEAN_FALSE;
            }
            break;
        }
        case (Unsigned32)PROPERTY_SET_TYPE_STRING:
        {
            char scratchValue[PROPERTY_NAME_LIMIT];
            char *valueTarget = (property != NULL_POINTER) ? property->stringValue : scratchValue;
            Boolean truncated = BOOLEAN_FALSE;

            readString(&cursor, valueTarget, PROPERTY_NAME_LIMIT, &truncated);
            if (property != NULL_POINTER)
            {
                property->kind = PROPERTY_VALUE_STRING;
                property->stringWasTruncated = truncated;
                property->integerValue = 0U;
                property->realValue = 0.0f;
            }
            break;
        }
        default:
            return (set->storedPropertyCount > 0U) ? PROPERTY_SET_OK : PROPERTY_SET_TRUNCATED;
        }

        if (cursor.overran)
        {
            break;
        }
        if (property != NULL_POINTER)
        {
            set->storedPropertyCount++;
        }
    }

    return (set->storedPropertyCount > 0U) ? PROPERTY_SET_OK : PROPERTY_SET_TRUNCATED;
}

const Property *propertySetFind(const PropertySet *set, const char *name)
{
    Unsigned32 index;

    if (set == NULL_POINTER || name == NULL_POINTER)
    {
        return NULL_POINTER;
    }
    for (index = 0U; index < set->storedPropertyCount; index++)
    {
        if (stringEqualsIgnoringCase(set->properties[index].name, name) == BOOLEAN_TRUE)
        {
            return &set->properties[index];
        }
    }
    return NULL_POINTER;
}

const char *propertySetGetString(const PropertySet *set, const char *name, const char *fallback)
{
    const Property *property = propertySetFind(set, name);

    if (property == NULL_POINTER || property->kind != PROPERTY_VALUE_STRING)
    {
        return fallback;
    }
    return property->stringValue;
}

Unsigned32 propertySetGetInteger(const PropertySet *set, const char *name, Unsigned32 fallback)
{
    const Property *property = propertySetFind(set, name);

    if (property == NULL_POINTER ||
        (property->kind != PROPERTY_VALUE_INTEGER && property->kind != PROPERTY_VALUE_BOOLEAN))
    {
        return fallback;
    }
    return property->integerValue;
}
