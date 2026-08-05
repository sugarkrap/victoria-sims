/* Checks the property set reader against bytes written out from the layout.
 *
 * A property set is the first resource here that is not part of the scenegraph,
 * and it does two things differently from everything already read — which is
 * the whole reason this file exists rather than a couple of checks bolted onto
 * a neighbour.
 *
 * Its strings are prefixed by a flat four-byte length. The scenegraph's are
 * prefixed by one to five bytes with a continuation bit. Read either with the
 * other's rule and the length that comes out is plausible, so the fixture below
 * uses a key long enough that the two rules disagree about where it ends.
 *
 * And a boolean is one byte, not four, in a stream where every other scalar is
 * four. Reading it wide swallows the start of the next property's type word.
 * The fixture puts a boolean in the middle with named properties either side,
 * so getting its width wrong loses everything after it rather than only it. */

#include <stdio.h>

#include "utils/assert.h"
#include "utils/strings.h"
#include "victoria/propertySet.h"

static Integer32 failureCount = 0;

#define BUILT_CAPACITY 1024UL

typedef struct Builder
{
    Unsigned8 bytes[BUILT_CAPACITY];
    MemorySize length;
} Builder;

static void putByte(Builder *builder, Unsigned8 value)
{
    if (builder->length < BUILT_CAPACITY)
    {
        builder->bytes[builder->length] = value;
        builder->length++;
    }
}

static void putUnsigned32(Builder *builder, Unsigned32 value)
{
    putByte(builder, (Unsigned8)(value & 0xFFU));
    putByte(builder, (Unsigned8)((value >> 8) & 0xFFU));
    putByte(builder, (Unsigned8)((value >> 16) & 0xFFU));
    putByte(builder, (Unsigned8)((value >> 24) & 0xFFU));
}

static void putUnsigned16(Builder *builder, Unsigned32 value)
{
    putByte(builder, (Unsigned8)(value & 0xFFU));
    putByte(builder, (Unsigned8)((value >> 8) & 0xFFU));
}

static void putReal32(Builder *builder, Real32 value)
{
    union
    {
        Unsigned32 word;
        Real32 value;
    } converter;

    converter.value = value;
    putUnsigned32(builder, converter.word);
}

/* Four bytes of length then the characters. Deliberately not the scenegraph's
   encoding — see the note at the top. */
static void putString(Builder *builder, const char *text)
{
    MemorySize length = stringLength(text);
    MemorySize index;

    putUnsigned32(builder, (Unsigned32)length);
    for (index = 0UL; index < length; index++)
    {
        putByte(builder, (Unsigned8)text[index]);
    }
}

int main(void)
{
    static Builder builder;
    static Property properties[16];
    PropertySet set;

    printf("-- a catalogue entry, written out from the layout --\n");
    builder.length = 0UL;
    putUnsigned32(&builder, (Unsigned32)PROPERTY_SET_CPF_MAGIC);
    putUnsigned16(&builder, 2U);
    putUnsigned32(&builder, 6U);

    /* A key long enough that a reader using the scenegraph's length rule would
       take its first byte as the length and stop four characters in. */
    putUnsigned32(&builder, (Unsigned32)PROPERTY_SET_TYPE_STRING);
    putString(&builder, "type");
    putString(&builder, "skin");

    putUnsigned32(&builder, (Unsigned32)PROPERTY_SET_TYPE_STRING);
    putString(&builder, "name");
    putString(&builder, "CASIE_efbodynightgown_floralpink");

    putUnsigned32(&builder, (Unsigned32)PROPERTY_SET_TYPE_INTEGER);
    putString(&builder, "category");
    putUnsigned32(&builder, 0x00000008UL);

    /* One byte. Everything after it depends on that being known. */
    putUnsigned32(&builder, (Unsigned32)PROPERTY_SET_TYPE_BOOLEAN);
    putString(&builder, "genetic");
    putByte(&builder, 1U);

    putUnsigned32(&builder, (Unsigned32)PROPERTY_SET_TYPE_REAL);
    putString(&builder, "fitness");
    putReal32(&builder, 0.75f);

    /* The format spells an integer two ways and means the same by both. */
    putUnsigned32(&builder, (Unsigned32)PROPERTY_SET_TYPE_INTEGER_AGAIN);
    putString(&builder, "shapekeyidx");
    putUnsigned32(&builder, 3U);

    set.properties = properties;
    set.propertyCapacity = 16U;
    checkThat(&failureCount, "it reads",
              propertySetRead(&set, builder.bytes, builder.length) == PROPERTY_SET_OK);
    checkThat(&failureCount, "with every property the header claimed",
              set.propertyCount == 6U && set.storedPropertyCount == 6U);

    checkThat(&failureCount, "a string value arrives whole",
              stringEquals(propertySetGetString(&set, "type", ""), "skin"));
    /* Thirty-two characters, which the scenegraph's rule would cut at four. */
    checkThat(&failureCount, "and so does one longer than a single length byte",
              stringEquals(propertySetGetString(&set, "name", ""),
                           "CASIE_efbodynightgown_floralpink"));
    checkThat(&failureCount, "an integer arrives", propertySetGetInteger(&set, "category", 0U) == 8U);
    checkThat(&failureCount, "a boolean counts as one", propertySetGetInteger(&set, "genetic", 0U) == 1U);

    /* The two properties after the boolean are the ones that prove its width.
       Read it as a word and the stream is three bytes out from here on. */
    checkThat(&failureCount, "the property after a boolean is still found",
              propertySetFind(&set, "fitness") != NULL_POINTER &&
                  propertySetFind(&set, "fitness")->kind == PROPERTY_VALUE_REAL);
    checkThat(&failureCount, "and the one after that",
              propertySetGetInteger(&set, "shapekeyidx", 0U) == 3U);
    /* Guarded rather than dereferenced. An unguarded arrow here segfaulted the
       moment the property genuinely went missing, which is precisely when this
       check was supposed to speak — a test that crashes instead of failing
       reports nothing at all. */
    checkThat(&failureCount, "both spellings of an integer read the same",
              propertySetFind(&set, "shapekeyidx") != NULL_POINTER &&
                  propertySetFind(&set, "shapekeyidx")->kind == PROPERTY_VALUE_INTEGER);

    printf("\n-- names are matched the way the format spells them --\n");
    checkThat(&failureCount, "without regard to case",
              stringEquals(propertySetGetString(&set, "TYPE", ""), "skin"));
    checkThat(&failureCount, "and a key that is not there says so",
              propertySetFind(&set, "shoe") == NULL_POINTER);
    checkThat(&failureCount, "with the caller's fallback returned for it",
              stringEquals(propertySetGetString(&set, "shoe", "none"), "none"));
    checkThat(&failureCount, "a string asked for as an integer does not answer",
              propertySetGetInteger(&set, "type", 99U) == 99U);

    printf("\n-- more properties than the caller will hold --\n");
    {
        /* Kept properties stop; reading does not. Every record is variable
           width, so the ones past the limit still have to be walked or the
           count would be wrong and the tail unreachable. */
        set.propertyCapacity = 2U;
        checkThat(&failureCount, "it still reads",
                  propertySetRead(&set, builder.bytes, builder.length) == PROPERTY_SET_OK);
        checkThat(&failureCount, "keeping only what fits", set.storedPropertyCount == 2U);
        checkThat(&failureCount, "while still reporting what was there",
                  set.propertyCount == 6U);
        set.propertyCapacity = 16U;
    }

    printf("\n-- and what it refuses --\n");
    {
        static Builder other;

        /* The XML spelling, which shares the resource type and is not read
           here. Told apart from rubbish because the two call for opposite
           responses: one wants an XML reader written, the other wants nothing. */
        other.length = 0UL;
        putByte(&other, (Unsigned8)'<');
        putByte(&other, (Unsigned8)'c');
        putByte(&other, (Unsigned8)'G');
        putByte(&other, (Unsigned8)'Z');
        checkThat(&failureCount, "an XML property set is named, not called rubbish",
                  propertySetRead(&set, other.bytes, other.length) == PROPERTY_SET_NOT_BINARY);

        /* A count larger than the resource has room to describe. Refused before
           anything is written on the strength of it, as every other count in
           this engine is. */
        other.length = 0UL;
        putUnsigned32(&other, (Unsigned32)PROPERTY_SET_CPF_MAGIC);
        putUnsigned16(&other, 2U);
        putUnsigned32(&other, 100000U);
        checkThat(&failureCount, "a count the resource cannot hold is refused",
                  propertySetRead(&set, other.bytes, other.length) ==
                      PROPERTY_SET_IMPLAUSIBLE_COUNT);

        /* A field width this does not know cannot be stepped over, so it stops
           — but keeps what it already read, because a property set is a bag of
           independent values and the ones before are still true. */
        other.length = 0UL;
        putUnsigned32(&other, (Unsigned32)PROPERTY_SET_CPF_MAGIC);
        putUnsigned16(&other, 2U);
        putUnsigned32(&other, 3U);
        putUnsigned32(&other, (Unsigned32)PROPERTY_SET_TYPE_STRING);
        putString(&other, "type");
        putString(&other, "skin");
        putUnsigned32(&other, 0xDEADBEEFUL);
        putString(&other, "whatever");
        checkThat(&failureCount, "an unknown field kind stops rather than guessing a width",
                  propertySetRead(&set, other.bytes, other.length) == PROPERTY_SET_OK);
        checkThat(&failureCount, "keeping what was read before it",
                  set.storedPropertyCount == 1U &&
                      stringEquals(propertySetGetString(&set, "type", ""), "skin"));
    }

    return checkSummarize(failureCount, "property set");
}
