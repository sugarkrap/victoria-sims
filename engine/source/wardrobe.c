#include "victoria/wardrobe.h"

#include "utils/strings.h"

/* What each drawn part demands of a catalogue entry before it will wear it. */
typedef struct WardrobePartRule
{
    /* The age and gender the entry must be authored for, spelled the way the
     * catalogue spells it — "am" for an adult male, then the part.
     *
     * This is the guard that matters. A child's body is weighted to a child's
     * skeleton, and hung on an adult's it does not read as a Sim in the wrong
     * clothes: the bones resolve to the wrong joints and the model comes apart.
     * The engine hangs everything on auskel_cres, so everything it wears must
     * have been authored for one. */
    const char *mark;
    /* The outfit slot that dresses this part, measured on this disc rather than
       taken from a reference: 0x01 is hair, 0x02 the face, 0x04 a top, 0x08 a
       whole body, 0x10 a bottom. */
    Unsigned32 outfit;
    /* What the part already wears, refused.
     *
     * The hardcoded base Sim is naked and bald, and the catalogue carries the
     * very entries that name those same two meshes. Taking one would resolve
     * the whole chain and change nothing on screen — which proves the chain
     * closes and is indistinguishable from a wardrobe that did nothing at all.
     * Refusing it means the run either dresses the Sim or says why it could
     * not. */
    const char *worn;
    /* Whether the skin tone has to match.
     *
     * It does for a face, which is not a thing worn over skin but the skin
     * itself: a face one tone off is a head that does not belong to the body
     * under it. It does not for a garment, whose colourway sits in the same
     * position in the name — "ambodyswimwear_redbikini" — and would be read as
     * a tone by anything looking for one. */
    Boolean wantsTone;
} WardrobePartRule;

static const WardrobePartRule wardrobeRules[WARDROBE_PART_COUNT] = {
    { "ambody", 0x08U, "_nude", BOOLEAN_FALSE },
    { "amface", 0x02U, "", BOOLEAN_TRUE },
    { "amhair", 0x01U, "hairbald", BOOLEAN_FALSE },
    { "amtop", 0x04U, "_nude", BOOLEAN_FALSE },
    { "ambottom", 0x10U, "_nude", BOOLEAN_FALSE }
};

const char *wardrobeGetPartMark(Unsigned32 part)
{
    return (part < WARDROBE_PART_COUNT) ? wardrobeRules[part].mark : "";
}

Unsigned32 wardrobeGetPartOutfit(Unsigned32 part)
{
    return (part < WARDROBE_PART_COUNT) ? wardrobeRules[part].outfit : 0U;
}

const char *wardrobeGetPartWorn(Unsigned32 part)
{
    return (part < WARDROBE_PART_COUNT) ? wardrobeRules[part].worn : "";
}

void wardrobeBegin(Wardrobe *wardrobe, const char *wanted, const char *tone)
{
    Unsigned32 part;

    for (part = 0U; part < WARDROBE_PART_COUNT; part++)
    {
        wardrobe->chosen[part] = BOOLEAN_FALSE;
        wardrobe->toneMatched[part] = BOOLEAN_FALSE;
        wardrobe->nameWanted[part] = BOOLEAN_FALSE;
        wardrobe->names[part][0] = '\0';
        wardrobe->alternativeCount[part] = 0U;
    }
    wardrobe->wanted[0] = '\0';
    wardrobe->tone[0] = '\0';
    if (wanted != NULL_POINTER)
    {
        stringAppend(wardrobe->wanted, (MemorySize)WARDROBE_NAME_LIMIT, wanted);
    }
    if (tone != NULL_POINTER)
    {
        stringAppend(wardrobe->tone, (MemorySize)WARDROBE_NAME_LIMIT, tone);
    }
    wardrobe->offered = 0U;
    wardrobe->taken = 0U;
    wardrobe->displaced = 0U;
    wardrobe->refusedUnnamed = 0U;
    wardrobe->refusedBySlot = 0U;
    wardrobe->refusedByMark = 0U;
    wardrobe->refusedAsWorn = 0U;
    wardrobe->matchedWanted = 0U;
    wardrobe->refusedAsSettled = 0U;
}

/* Whether a name ends in this tone, the way the catalogue writes it: an
   underscore and then the tone, at the very end. Ending "s1" without the
   separator would match "casims1" and anything else that happens to finish
   with those two characters. */
static Boolean nameCarriesTone(const char *name, const char *tone)
{
    char suffix[WARDROBE_NAME_LIMIT];

    if (tone[0] == '\0')
    {
        return BOOLEAN_FALSE;
    }
    suffix[0] = '\0';
    stringAppend(suffix, sizeof(suffix), "_");
    stringAppend(suffix, sizeof(suffix), tone);
    return stringEndsWithIgnoringCase(name, suffix);
}

Unsigned32 wardrobeOffer(Wardrobe *wardrobe, const char *entryName, Unsigned32 outfitSlot)
{
    Unsigned32 part;
    Boolean tonedRight;
    Boolean wantedRight;

    wardrobe->offered++;

    /* The unnamed entries are the groupings — an entry whose keys are a
       transform tree and a dozen other catalogue entries. They dress nothing. */
    if (entryName == NULL_POINTER || entryName[0] == '\0')
    {
        wardrobe->refusedUnnamed++;
        return (Unsigned32)WARDROBE_NOT_WORN;
    }

    for (part = 0U; part < WARDROBE_PART_COUNT; part++)
    {
        if (wardrobeRules[part].outfit == outfitSlot)
        {
            break;
        }
    }
    if (part >= WARDROBE_PART_COUNT)
    {
        /* A top, a bottom, or a slot naming more than one part at once. The
           engine draws a whole body and has nowhere to put those yet. */
        wardrobe->refusedBySlot++;
        return (Unsigned32)WARDROBE_NOT_WORN;
    }

    if (!stringContainsIgnoringCase(entryName, wardrobeRules[part].mark))
    {
        wardrobe->refusedByMark++;
        return (Unsigned32)WARDROBE_NOT_WORN;
    }

    if (wardrobeRules[part].worn[0] != '\0' &&
        stringContainsIgnoringCase(entryName, wardrobeRules[part].worn))
    {
        wardrobe->refusedAsWorn++;
        return (Unsigned32)WARDROBE_NOT_WORN;
    }

    /* A name asked for is a PREFERENCE and not a filter.
     *
     * As a filter it read perfectly well and did the wrong thing: asking for a
     * garment refused every hair and every face too, because neither is named
     * after a garment, and the Sim came out dressed and bald. Nothing is read
     * during the walk — only remembered — so preferring costs exactly what
     * refusing did and leaves the parts nobody asked about wearing whatever the
     * catalogue offered them. */
    wantedRight = (wardrobe->wanted[0] != '\0' &&
                   stringContainsIgnoringCase(entryName, wardrobe->wanted))
                      ? BOOLEAN_TRUE
                      : BOOLEAN_FALSE;
    if (wantedRight)
    {
        wardrobe->matchedWanted++;
    }
    tonedRight = wardrobeRules[part].wantsTone
                     ? nameCarriesTone(entryName, wardrobe->tone)
                     : BOOLEAN_TRUE;

    /* Kept whether it wins or loses: what a part is wearing now is as much an
       alternative to what it wears next as the ones it passed over. */
    if (wardrobe->alternativeCount[part] < WARDROBE_ALTERNATIVE_LIMIT)
    {
        char *slot = wardrobe->alternatives[part][wardrobe->alternativeCount[part]];

        wardrobe->alternativeCount[part]++;
        slot[0] = '\0';
        stringAppend(slot, (MemorySize)WARDROBE_NAME_LIMIT, entryName);
    }

    if (wardrobe->chosen[part])
    {
        /* Ranked, and an equal rank does not displace — so the first of any two
           equally good candidates is worn, and a walk that met a thousand of
           them settles rather than changing its mind a thousand times. Asked
           for outranks the right tone: someone who named a face wants that
           face, whatever tone it turns out to be. */
        Unsigned32 held = (wardrobe->nameWanted[part] ? 2U : 0U) +
                          (wardrobe->toneMatched[part] ? 1U : 0U);
        Unsigned32 offer = (wantedRight ? 2U : 0U) + (tonedRight ? 1U : 0U);

        if (offer <= held)
        {
            wardrobe->refusedAsSettled++;
            return (Unsigned32)WARDROBE_NOT_WORN;
        }
        wardrobe->displaced++;
    }
    else
    {
        wardrobe->taken++;
    }

    wardrobe->chosen[part] = BOOLEAN_TRUE;
    wardrobe->toneMatched[part] = tonedRight;
    wardrobe->nameWanted[part] = wantedRight;
    wardrobe->names[part][0] = '\0';
    stringAppend(wardrobe->names[part], (MemorySize)WARDROBE_NAME_LIMIT, entryName);
    return part;
}

WardrobeArrangement wardrobeGetArrangement(const Wardrobe *wardrobe)
{
    /* Asked for by name settles it before anything else does. Somebody who
       names a whole-body garment means to look at that garment, and the pair
       would otherwise win every time and never let them. */
    if (wardrobe->chosen[WARDROBE_PART_BODY] && wardrobe->nameWanted[WARDROBE_PART_BODY] &&
        !wardrobe->nameWanted[WARDROBE_PART_TOP] && !wardrobe->nameWanted[WARDROBE_PART_BOTTOM])
    {
        return WARDROBE_ARRANGEMENT_WHOLE;
    }

    /* Then the pair, because it is the arrangement that says something a whole
     * body cannot: two garments chosen independently. A Sim who has both offered
     * wears both.
     *
     * A top on its own is refused rather than drawn. The bottom half of a Sim
     * would then be the assembled body showing through under a shirt — two
     * meshes occupying the same legs — which reads as a rendering fault rather
     * than as half a wardrobe. */
    if (wardrobe->chosen[WARDROBE_PART_TOP] && wardrobe->chosen[WARDROBE_PART_BOTTOM])
    {
        return WARDROBE_ARRANGEMENT_PAIR;
    }
    if (wardrobe->chosen[WARDROBE_PART_BODY])
    {
        return WARDROBE_ARRANGEMENT_WHOLE;
    }
    return WARDROBE_ARRANGEMENT_AS_ASSEMBLED;
}

Boolean wardrobeIsWorn(const Wardrobe *wardrobe, Unsigned32 part)
{
    WardrobeArrangement arrangement;

    if (part >= WARDROBE_PART_COUNT || !wardrobe->chosen[part])
    {
        return BOOLEAN_FALSE;
    }
    if (part != WARDROBE_PART_BODY && part != WARDROBE_PART_TOP && part != WARDROBE_PART_BOTTOM)
    {
        return BOOLEAN_TRUE;
    }
    arrangement = wardrobeGetArrangement(wardrobe);
    if (part == WARDROBE_PART_BODY)
    {
        return (arrangement == WARDROBE_ARRANGEMENT_WHOLE) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
    }
    return (arrangement == WARDROBE_ARRANGEMENT_PAIR) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

Boolean wardrobeIsChosen(const Wardrobe *wardrobe, Unsigned32 part)
{
    return (part < WARDROBE_PART_COUNT) ? wardrobe->chosen[part] : BOOLEAN_FALSE;
}

const char *wardrobeGetChosenName(const Wardrobe *wardrobe, Unsigned32 part)
{
    return (part < WARDROBE_PART_COUNT) ? wardrobe->names[part] : "";
}

const char *wardrobeGetAlternative(const Wardrobe *wardrobe, Unsigned32 part, Unsigned32 which)
{
    if (part >= WARDROBE_PART_COUNT || which >= wardrobe->alternativeCount[part])
    {
        return "";
    }
    return wardrobe->alternatives[part][which];
}

Unsigned32 wardrobeGetChosenCount(const Wardrobe *wardrobe)
{
    Unsigned32 part;
    Unsigned32 count = 0U;

    for (part = 0U; part < WARDROBE_PART_COUNT; part++)
    {
        if (wardrobe->chosen[part])
        {
            count++;
        }
    }
    return count;
}
