#include "victoria/wardrobe.h"

#include "utils/strings.h"

/* What each drawn part demands of a catalogue entry before it will wear it. */
typedef struct WardrobePartRule
{
    /* What follows the age and gender in the name — "body", "hair", "top".
     *
     * The age and gender come from the Sim, not from here. Together they make
     * the guard that matters: a child's body is weighted to a child's skeleton,
     * and hung on an adult's it does not read as a Sim in the wrong clothes —
     * the bones resolve to the wrong joints and the model comes apart. */
    const char *suffix;
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
    { "body", 0x08U, "_nude", BOOLEAN_FALSE },
    { "face", 0x02U, "", BOOLEAN_TRUE },
    { "hair", 0x01U, "hairbald", BOOLEAN_FALSE },
    { "top", 0x04U, "_nude", BOOLEAN_FALSE },
    { "bottom", 0x10U, "_nude", BOOLEAN_FALSE }
};

const char *wardrobeGetPartMark(const Wardrobe *wardrobe, Unsigned32 part)
{
    return (part < WARDROBE_PART_COUNT) ? wardrobe->marks[part] : "";
}

Unsigned32 wardrobeGetPartOutfit(Unsigned32 part)
{
    return (part < WARDROBE_PART_COUNT) ? wardrobeRules[part].outfit : 0U;
}

const char *wardrobeGetPartWorn(Unsigned32 part)
{
    return (part < WARDROBE_PART_COUNT) ? wardrobeRules[part].worn : "";
}

void wardrobeBegin(Wardrobe *wardrobe, const char *archetype, const char *wanted,
                   const char *tone)
{
    Unsigned32 part;

    wardrobe->archetype[0] = '\0';
    if (archetype != NULL_POINTER)
    {
        stringAppend(wardrobe->archetype, (MemorySize)WARDROBE_ARCHETYPE_LIMIT, archetype);
    }
    for (part = 0U; part < WARDROBE_PART_COUNT; part++)
    {
        /* An empty archetype leaves an empty mark, and an empty mark matches
           everything — so it is refused outright in the offer rather than
           quietly dressing this Sim in the whole catalogue. */
        wardrobe->marks[part][0] = '\0';
        stringAppend(wardrobe->marks[part], (MemorySize)WARDROBE_NAME_LIMIT,
                     wardrobe->archetype);
        stringAppend(wardrobe->marks[part], (MemorySize)WARDROBE_NAME_LIMIT,
                     wardrobeRules[part].suffix);
        wardrobe->chosen[part] = BOOLEAN_FALSE;
        wardrobe->toneMatched[part] = BOOLEAN_FALSE;
        wardrobe->toneRank[part] = 0U;
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

/* Whether a name ends in ANY skin tone — an underscore, an s, and then digits.
 *
 * A face that carries no tone at all is not a face of the wrong colour: it is
 * not an ordinary face. `tfface_alien` and `CASIE_puface_mannequin` are in the
 * same slot as `CASIE_tmface_s1` and are a green alien and a shop dummy, and a
 * rule that only asked "is this the right tone" ranked them level with every
 * real face of the wrong one — so a teenager came out green. */
static Boolean nameCarriesAnyTone(const char *name)
{
    MemorySize length = stringLength(name);
    MemorySize at = length;
    MemorySize digits = 0UL;

    while (at > 0UL && name[at - 1UL] >= '0' && name[at - 1UL] <= '9')
    {
        at -= 1UL;
        digits += 1UL;
    }
    if (digits == 0UL || at < 2UL)
    {
        return BOOLEAN_FALSE;
    }
    return (characterToLowerCase(name[at - 1UL]) == 's' && name[at - 2UL] == '_') ? BOOLEAN_TRUE
                                                                                 : BOOLEAN_FALSE;
}

/* How well a candidate's tone suits this Sim. Two is the tone asked for, one is
   some other real tone, nought is no tone at all. Three steps and not two,
   because "wrong colour" and "not a person" are different answers. */
static Unsigned32 toneRankOf(const char *name, const char *tone)
{
    if (nameCarriesTone(name, tone))
    {
        return 2U;
    }
    return nameCarriesAnyTone(name) ? 1U : 0U;
}

Unsigned32 wardrobeOffer(Wardrobe *wardrobe, const char *entryName, Unsigned32 outfitSlot)
{
    Unsigned32 part;
    Unsigned32 toneRank;
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

    /* No archetype means nobody to dress. Refused rather than defaulted: an
       empty mark is contained in every name, so falling back would dress this
       Sim in the first garment of every slot the disc holds. */
    if (wardrobe->archetype[0] == '\0' ||
        !stringContainsIgnoringCase(entryName, wardrobe->marks[part]))
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
    /* A part that does not care about the tone sits at the top of the scale, so
       nothing about a tone can ever displace it. */
    toneRank = wardrobeRules[part].wantsTone ? toneRankOf(entryName, wardrobe->tone) : 2U;
    tonedRight = (toneRank >= 2U) ? BOOLEAN_TRUE : BOOLEAN_FALSE;

    /* A face carrying no tone at all is never worn.
     *
     * The part already has a face — the assembly built one, and the stand-in
     * paints it in the Sim's own tone — so the wardrobe's job here is to improve
     * on that, not to replace it. An alien and a shop mannequin sit in the face
     * slot beside every real face and are neither; taking one because it was the
     * only thing the sample reached is how a teenager came out green while the
     * face she already had was correct.
     *
     * This is the face's version of the rule that a body will not wear `_nude`
     * and a scalp will not wear `hairbald`: what a part already wears is the
     * floor, and something that cannot beat it is not an improvement. */
    if (wardrobeRules[part].wantsTone && toneRank == 0U)
    {
        wardrobe->refusedAsWorn++;
        return (Unsigned32)WARDROBE_NOT_WORN;
    }

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
        Unsigned32 held = (wardrobe->nameWanted[part] ? 4U : 0U) + wardrobe->toneRank[part];
        Unsigned32 offer = (wantedRight ? 4U : 0U) + toneRank;

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
    wardrobe->toneRank[part] = toneRank;
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
