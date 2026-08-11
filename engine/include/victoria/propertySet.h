#ifndef VICTORIA_PROPERTY_SET_HEADER
#define VICTORIA_PROPERTY_SET_HEADER

#include "victoria/coreTypes.h"

#define PROPERTY_SET_CPF_MAGIC 0xCBE750E0UL

#define PROPERTY_SET_TYPE_INTEGER 0xEB61E4F7UL
#define PROPERTY_SET_TYPE_STRING 0x0B8BEA18UL
#define PROPERTY_SET_TYPE_REAL 0xABC78708UL
#define PROPERTY_SET_TYPE_BOOLEAN 0xCBA908E1UL
#define PROPERTY_SET_TYPE_INTEGER_AGAIN 0x0C264712UL

#define PROPERTY_NAME_LIMIT 64UL

typedef enum PropertyValueKind
{
    PROPERTY_VALUE_INTEGER = 0,
    PROPERTY_VALUE_STRING,
    PROPERTY_VALUE_REAL,
    PROPERTY_VALUE_BOOLEAN
} PropertyValueKind;

typedef struct Property
{
    char name[PROPERTY_NAME_LIMIT];
    PropertyValueKind kind;
    Unsigned32 integerValue;
    Real32 realValue;
    char stringValue[PROPERTY_NAME_LIMIT];
    Boolean stringWasTruncated;
} Property;

typedef enum PropertySetReadResult
{
    PROPERTY_SET_OK = 0,
    PROPERTY_SET_NOT_BINARY,
    PROPERTY_SET_TRUNCATED,
    PROPERTY_SET_IMPLAUSIBLE_COUNT
} PropertySetReadResult;

#define PROPERTY_SET_READ_RESULT_COUNT 4U

const char *propertySetReadResultGetName(PropertySetReadResult result);

typedef struct PropertySet
{
    Unsigned32 version;
    Unsigned32 propertyCount;
    Unsigned32 storedPropertyCount;
    Property *properties;
    Unsigned32 propertyCapacity;
} PropertySet;

PropertySetReadResult propertySetRead(PropertySet *set, const Unsigned8 *bytes,
                                      MemorySize sizeInBytes);

const Property *propertySetFind(const PropertySet *set, const char *name);

const char *propertySetGetString(const PropertySet *set, const char *name, const char *fallback);

Unsigned32 propertySetGetInteger(const PropertySet *set, const char *name, Unsigned32 fallback);

#endif
