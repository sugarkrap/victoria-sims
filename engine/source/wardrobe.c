#include "victoria/wardrobe.h"

#include "utils/strings.h"

typedef struct WardrobePartRule
{
    const char *suffix;
    Unsigned32 outfit;
    const char *worn;
    Boolean wantsTone;
} WardrobePartRule;

static const WardrobePartRule wardrobeRules[WARDROBE_PART_COUNT] = {
    { "body", 0x08U, "_nude", BOOLEAN_FALSE },
    { "face", 0x02U, "", BOOLEAN_TRUE },
    { "hair", 0x01U, "hairbald", BOOLEAN_FALSE },
    { "top", 0x04U, "_nude", BOOLEAN_FALSE },
    { "bottom", 0x10U, "_nude", BOOLEAN_FALSE }
};

const char *wardrobeGetPartName(Unsigned32 part)
{
    return (part < WARDROBE_PART_COUNT) ? wardrobeRules[part].suffix : "";
}

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
        wardrobe->marks[part][0] = '\0';
        stringAppend(wardrobe->marks[part], (MemorySize)WARDROBE_NAME_LIMIT,
                     wardrobe->archetype);
        stringAppend(wardrobe->marks[part], (MemorySize)WARDROBE_NAME_LIMIT,
                     wardrobeRules[part].suffix);
        wardrobe->chosen[part] = BOOLEAN_FALSE;
        wardrobe->toneMatched[part] = BOOLEAN_FALSE;
        wardrobe->toneRank[part] = 0U;
        wardrobe->nameWanted[part] = BOOLEAN_FALSE;
        wardrobe->askedForByName[part] = BOOLEAN_FALSE;
        wardrobe->names[part][0] = '\0';
        wardrobe->alternativeCount[part] = 0U;
        wardrobe->alternativesBeyondRoom[part] = 0U;
        wardrobe->wantedPerPart[part][0] = '\0';
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

void wardrobeWant(Wardrobe *wardrobe, Unsigned32 part, const char *entryName)
{
    if (part >= WARDROBE_PART_COUNT)
    {
        return;
    }
    wardrobe->wantedPerPart[part][0] = '\0';
    if (entryName != NULL_POINTER)
    {
        stringAppend(wardrobe->wantedPerPart[part], (MemorySize)WARDROBE_NAME_LIMIT, entryName);
    }
}

const char *wardrobeGetWanted(const Wardrobe *wardrobe, Unsigned32 part)
{
    return (part < WARDROBE_PART_COUNT) ? wardrobe->wantedPerPart[part] : "";
}

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
    Boolean askedForByName;

    wardrobe->offered++;

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
        wardrobe->refusedBySlot++;
        return (Unsigned32)WARDROBE_NOT_WORN;
    }

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

    wantedRight = (wardrobe->wanted[0] != '\0' &&
                   stringContainsIgnoringCase(entryName, wardrobe->wanted))
                      ? BOOLEAN_TRUE
                      : BOOLEAN_FALSE;
    askedForByName = (wardrobe->wantedPerPart[part][0] != '\0' &&
                      stringEqualsIgnoringCase(entryName, wardrobe->wantedPerPart[part]))
                         ? BOOLEAN_TRUE
                         : BOOLEAN_FALSE;
    if (askedForByName)
    {
        wantedRight = BOOLEAN_TRUE;
    }
    if (wantedRight)
    {
        wardrobe->matchedWanted++;
    }
    toneRank = wardrobeRules[part].wantsTone ? toneRankOf(entryName, wardrobe->tone) : 2U;
    tonedRight = (toneRank >= 2U) ? BOOLEAN_TRUE : BOOLEAN_FALSE;

    if (wardrobeRules[part].wantsTone && toneRank == 0U)
    {
        wardrobe->refusedAsWorn++;
        return (Unsigned32)WARDROBE_NOT_WORN;
    }

    if (wardrobe->alternativeCount[part] < WARDROBE_ALTERNATIVE_LIMIT)
    {
        char *slot = wardrobe->alternatives[part][wardrobe->alternativeCount[part]];

        wardrobe->alternativeCount[part]++;
        slot[0] = '\0';
        stringAppend(slot, (MemorySize)WARDROBE_NAME_LIMIT, entryName);
    }
    else
    {
        wardrobe->alternativesBeyondRoom[part]++;
    }

    if (wardrobe->chosen[part])
    {
        Unsigned32 held = (wardrobe->askedForByName[part] ? 8U : 0U) +
                          (wardrobe->nameWanted[part] ? 4U : 0U) + wardrobe->toneRank[part];
        Unsigned32 offer = (askedForByName ? 8U : 0U) + (wantedRight ? 4U : 0U) + toneRank;

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
    wardrobe->askedForByName[part] = askedForByName;
    wardrobe->names[part][0] = '\0';
    stringAppend(wardrobe->names[part], (MemorySize)WARDROBE_NAME_LIMIT, entryName);
    return part;
}

WardrobeArrangement wardrobeGetArrangement(const Wardrobe *wardrobe)
{
    if (wardrobe->chosen[WARDROBE_PART_BODY] && wardrobe->nameWanted[WARDROBE_PART_BODY] &&
        !wardrobe->nameWanted[WARDROBE_PART_TOP] && !wardrobe->nameWanted[WARDROBE_PART_BOTTOM])
    {
        return WARDROBE_ARRANGEMENT_WHOLE;
    }

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

Unsigned32 wardrobeGetAlternativeCount(const Wardrobe *wardrobe, Unsigned32 part)
{
    return (part < WARDROBE_PART_COUNT) ? wardrobe->alternativeCount[part] : 0U;
}

Unsigned32 wardrobeGetAlternativesBeyondRoom(const Wardrobe *wardrobe, Unsigned32 part)
{
    return (part < WARDROBE_PART_COUNT) ? wardrobe->alternativesBeyondRoom[part] : 0U;
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
