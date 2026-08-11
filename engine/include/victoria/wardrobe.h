#ifndef VICTORIA_WARDROBE_HEADER
#define VICTORIA_WARDROBE_HEADER

#include "victoria/coreTypes.h"

#define WARDROBE_PART_COUNT 5U
#define WARDROBE_PART_BODY 0U
#define WARDROBE_PART_FACE 1U
#define WARDROBE_PART_HAIR 2U
#define WARDROBE_PART_TOP 3U
#define WARDROBE_PART_BOTTOM 4U
#define WARDROBE_NOT_WORN 0xFFFFFFFFUL
#define WARDROBE_NAME_LIMIT 64U
#define WARDROBE_ALTERNATIVE_LIMIT 128U
#define WARDROBE_ALTERNATIVES_WORTH_LOGGING 8U
#define WARDROBE_ARCHETYPE_LIMIT 8U

typedef struct Wardrobe
{
    Boolean chosen[WARDROBE_PART_COUNT];
    Boolean toneMatched[WARDROBE_PART_COUNT];
    Unsigned32 toneRank[WARDROBE_PART_COUNT];
    Boolean nameWanted[WARDROBE_PART_COUNT];
    Boolean askedForByName[WARDROBE_PART_COUNT];
    char names[WARDROBE_PART_COUNT][WARDROBE_NAME_LIMIT];

    char archetype[WARDROBE_ARCHETYPE_LIMIT];
    char marks[WARDROBE_PART_COUNT][WARDROBE_NAME_LIMIT];

    char wanted[WARDROBE_NAME_LIMIT];
    char tone[WARDROBE_NAME_LIMIT];

    Unsigned32 offered;
    Unsigned32 taken;
    Unsigned32 displaced;
    Unsigned32 refusedUnnamed;
    Unsigned32 refusedBySlot;
    Unsigned32 refusedByMark;
    Unsigned32 refusedAsWorn;
    Unsigned32 matchedWanted;
    Unsigned32 refusedAsSettled;

    char alternatives[WARDROBE_PART_COUNT][WARDROBE_ALTERNATIVE_LIMIT][WARDROBE_NAME_LIMIT];
    Unsigned32 alternativeCount[WARDROBE_PART_COUNT];
    Unsigned32 alternativesBeyondRoom[WARDROBE_PART_COUNT];

    char wantedPerPart[WARDROBE_PART_COUNT][WARDROBE_NAME_LIMIT];
} Wardrobe;

void wardrobeBegin(Wardrobe *wardrobe, const char *archetype, const char *wanted,
                   const char *tone);

void wardrobeWant(Wardrobe *wardrobe, Unsigned32 part, const char *entryName);
const char *wardrobeGetWanted(const Wardrobe *wardrobe, Unsigned32 part);

Unsigned32 wardrobeOffer(Wardrobe *wardrobe, const char *entryName, Unsigned32 outfitSlot);

const char *wardrobeGetPartMark(const Wardrobe *wardrobe, Unsigned32 part);
const char *wardrobeGetPartName(Unsigned32 part);
Unsigned32 wardrobeGetPartOutfit(Unsigned32 part);
const char *wardrobeGetPartWorn(Unsigned32 part);

typedef enum WardrobeArrangement
{
    WARDROBE_ARRANGEMENT_WHOLE = 0,
    WARDROBE_ARRANGEMENT_PAIR,
    WARDROBE_ARRANGEMENT_AS_ASSEMBLED
} WardrobeArrangement;

WardrobeArrangement wardrobeGetArrangement(const Wardrobe *wardrobe);
Boolean wardrobeIsWorn(const Wardrobe *wardrobe, Unsigned32 part);

Boolean wardrobeIsChosen(const Wardrobe *wardrobe, Unsigned32 part);
const char *wardrobeGetChosenName(const Wardrobe *wardrobe, Unsigned32 part);
Unsigned32 wardrobeGetChosenCount(const Wardrobe *wardrobe);
const char *wardrobeGetAlternative(const Wardrobe *wardrobe, Unsigned32 part, Unsigned32 which);
Unsigned32 wardrobeGetAlternativeCount(const Wardrobe *wardrobe, Unsigned32 part);
Unsigned32 wardrobeGetAlternativesBeyondRoom(const Wardrobe *wardrobe, Unsigned32 part);

#endif
