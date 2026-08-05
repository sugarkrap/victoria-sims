#ifndef VICTORIA_PROPERTY_SET_HEADER
#define VICTORIA_PROPERTY_SET_HEADER

#include "victoria/coreTypes.h"

/* Reads a cGZPropertySetString — a flat bag of named values.
 *
 * This is how the game says what a Sim is made of, and it is the first resource
 * here that is not part of the scenegraph. A scenegraph resource points at
 * another scenegraph resource; a property set names things in text and leaves
 * the resolving to whoever reads it.
 *
 * Why it is needed: the four names a whole Sim is currently built from —
 * auskel, amBodyNaked, amFace, amHairBald — are hardcoded, and everything else
 * a Sim wears or is built from arrives through these. A Sim has no brows, eyes
 * or lips because those are catalogue entries and nothing here read the
 * catalogue. Clothing is the same. So is a face archetype.
 *
 * Two encodings share the type. The binary one, CPF, starts with the magic
 * below. Anything else is XML — the same properties spelled as
 * <cGZPropertySetString> with AnyString and AnyUint32 elements — and is NOT
 * read here. Retail discs use both, so a caller must expect to be told which it
 * met rather than assuming.
 *
 * Nothing is allocated. A caller hands in the storage for the properties it is
 * willing to keep, and a set with more than that is truncated rather than
 * refused: the property wanted is usually in the first handful, and a catalogue
 * entry with an unusual tail is still worth the name it carries. */

/* First four bytes of the binary form, little endian on disc as 0xE0 0x50 0xE7
   0xCB. */
#define PROPERTY_SET_CPF_MAGIC 0xCBE750E0UL

/* The five value encodings the format uses. Two spellings of an integer exist
   and mean the same thing, which is the format's business and not a caller's —
   both arrive as PROPERTY_VALUE_INTEGER. */
#define PROPERTY_SET_TYPE_INTEGER 0xEB61E4F7UL
#define PROPERTY_SET_TYPE_STRING 0x0B8BEA18UL
#define PROPERTY_SET_TYPE_REAL 0xABC78708UL
#define PROPERTY_SET_TYPE_BOOLEAN 0xCBA908E1UL
#define PROPERTY_SET_TYPE_INTEGER_AGAIN 0x0C264712UL

/* Keys and string values are short in every entry met — "type", "name",
   "category", a mesh name. One that overruns is truncated rather than losing
   the property, for the same reason a primitive name is. */
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
    /* Whichever of these the kind says. A string value longer than the limit is
       kept truncated, and truncated is flagged so a caller comparing one
       against a full name knows not to trust the match. */
    Unsigned32 integerValue;
    Real32 realValue;
    char stringValue[PROPERTY_NAME_LIMIT];
    Boolean stringWasTruncated;
} Property;

typedef enum PropertySetReadResult
{
    PROPERTY_SET_OK = 0,
    /* Not the binary form. Almost certainly the XML one, which this does not
       read — distinguished from rubbish because the two call for opposite
       responses. */
    PROPERTY_SET_NOT_BINARY,
    PROPERTY_SET_TRUNCATED,
    /* A count larger than the resource has bytes to describe. */
    PROPERTY_SET_IMPLAUSIBLE_COUNT
} PropertySetReadResult;

#define PROPERTY_SET_READ_RESULT_COUNT 4U

const char *propertySetReadResultGetName(PropertySetReadResult result);

typedef struct PropertySet
{
    Unsigned32 version;
    /* What the file said it holds, which may be more than was kept. */
    Unsigned32 propertyCount;
    Unsigned32 storedPropertyCount;
    Property *properties;
    Unsigned32 propertyCapacity;
} PropertySet;

/* Reads into storage the caller owns. Set `properties` and `propertyCapacity`
   before calling; everything else is filled in. */
PropertySetReadResult propertySetRead(PropertySet *set, const Unsigned8 *bytes,
                                      MemorySize sizeInBytes);

/* The property of that name, or null. Compared without regard to case: the
   format's own keys are not consistent about it. */
const Property *propertySetFind(const PropertySet *set, const char *name);

/* Its string value, or the fallback when there is no such property or it is not
   a string. Saves every caller the same two checks. */
const char *propertySetGetString(const PropertySet *set, const char *name, const char *fallback);

/* Its integer value, or the fallback. A boolean counts as an integer here,
   being one in the file. */
Unsigned32 propertySetGetInteger(const PropertySet *set, const char *name, Unsigned32 fallback);

#endif
