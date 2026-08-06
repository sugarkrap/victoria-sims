#ifndef VICTORIA_WARDROBE_HEADER
#define VICTORIA_WARDROBE_HEADER

#include "victoria/coreTypes.h"

/* Which catalogue entry each of a Sim's drawn parts should wear.
 *
 * A Sim is assembled from four hardcoded names — a skeleton and three models
 * that skin to it — because those are the only parts the engine can find
 * without reading anything that names them. Everything else a Sim can be is
 * described in the catalogue: a property set carrying an outfit slot and a
 * shape index, and the shape at that index is a mesh on the disc.
 *
 * This is the part of that with judgement in it. The chain from an entry to a
 * shape is arithmetic and the disc either closes it or does not; choosing WHICH
 * entry is a decision, and a wrong one is not a refusal but a Sim wearing
 * somebody else's body. So it is here, on its own, where it can be checked
 * against a claim rather than against a disc.
 *
 * The offer is made once per entry as the catalogue is walked, so it must be
 * cheap and must not read anything. It decides on the name and the slot alone,
 * both of which the entry has already given up. */

/* The parts a Sim is drawn in, in the order they are joined. Nought is the
 * whole body, one the face, two the hair, three a top and four a bottom — the
 * same order as simPartNames in engineCore.c.
 *
 * A Sim wears EITHER a whole body OR a top and a bottom together. They are the
 * same volume of Sim described two ways, and the catalogue keeps them in
 * separate slots because a garment is one or the other. Which arrangement a
 * given Sim ends up in is wardrobeGetArrangement's answer. */
#define WARDROBE_PART_COUNT 5U
#define WARDROBE_PART_BODY 0U
#define WARDROBE_PART_FACE 1U
#define WARDROBE_PART_HAIR 2U
#define WARDROBE_PART_TOP 3U
#define WARDROBE_PART_BOTTOM 4U
#define WARDROBE_NOT_WORN 0xFFFFFFFFUL
#define WARDROBE_NAME_LIMIT 64U
/* How many of the entries a part could have worn instead get named.
 *
 * The counts alone say a hundred and fifty other garments fitted and name none
 * of them, which leaves a run steered by --wear with nothing to steer at. Eight
 * a part is enough to pick from and few enough not to bury the report. */
/* How many candidates a part remembers passing over.
 *
 * Was eight, which is the right number to LOG — a run naming eight alternatives
 * per part gives the next run's `--wear=` argument without burying the console.
 * It is the wrong number to keep, because the same list is what the menu's
 * clothing page offers, and a page of eight garments out of the several hundred
 * a disc carries for each slot is not a wardrobe, it is a sample. The log still
 * stops at eight; this is the store behind it. */
#define WARDROBE_ALTERNATIVE_LIMIT 128U
#define WARDROBE_ALTERNATIVES_WORTH_LOGGING 8U
/* An age and a gender, as the catalogue spells them: "am" is an adult male,
   "cf" a child female, "tu" a teen of neither. Two characters and a
   terminator, with room over. */
#define WARDROBE_ARCHETYPE_LIMIT 8U

/* What the caller must know to hold a choice: which part, and by what name. The
   resource the name resolves to is the caller's business — this decides, it
   does not fetch. */
typedef struct Wardrobe
{
    Boolean chosen[WARDROBE_PART_COUNT];
    /* Whether the held choice is the right tone for this Sim's skin. A face
       that is nearly the right tone is a face of the wrong colour, so a later
       entry displaces an earlier one when it matches and the earlier did not. */
    Boolean toneMatched[WARDROBE_PART_COUNT];
    /* Two if it is the tone asked for, one if it is some other real tone, and
       nought if it carries no tone at all — which is what an alien and a
       mannequin are, sitting in the same slot as every ordinary face. */
    Unsigned32 toneRank[WARDROBE_PART_COUNT];
    /* Whether the held choice is one the caller asked for by name. It outranks
       the tone, and nothing outranks it. */
    Boolean nameWanted[WARDROBE_PART_COUNT];
    Boolean askedForByName[WARDROBE_PART_COUNT];
    char names[WARDROBE_PART_COUNT][WARDROBE_NAME_LIMIT];

    /* Who this Sim is, and what each part's name must therefore contain.
     *
     * Composed here rather than written down per part, because the age and the
     * gender are one decision and five hardcoded strings are five places for it
     * to be made differently. An engine dressing an adult male out of a table
     * that said "amhair" could not dress anybody else at all. */
    char archetype[WARDROBE_ARCHETYPE_LIMIT];
    char marks[WARDROBE_PART_COUNT][WARDROBE_NAME_LIMIT];

    /* What was asked for, kept so the report can say what the choices were
       measured against. Empty means "whatever the catalogue offers first".
       It is a preference and not a filter: a part nothing matching was ever
       offered for still wears the best of what was. */
    char wanted[WARDROBE_NAME_LIMIT];
    /* The Sim's skin tone as the disc spells it — "s1" out of
       "ambodynaked-nude-s1". Empty means the tone is not known yet, in which
       case no candidate matches it and the first met is taken. */
    char tone[WARDROBE_NAME_LIMIT];

    /* Every entry that reached here, and why each was passed over. A wardrobe
       that dresses nothing and cannot say why is indistinguishable from a disc
       that carries nothing to wear. */
    Unsigned32 offered;
    Unsigned32 taken;
    Unsigned32 displaced;
    Unsigned32 refusedUnnamed;
    Unsigned32 refusedBySlot;
    Unsigned32 refusedByMark;
    Unsigned32 refusedAsWorn;
    /* Entries whose name held what was asked for, however they then ranked. */
    Unsigned32 matchedWanted;
    Unsigned32 refusedAsSettled;

    /* What each part could have worn instead — the first few that got as far as
       being ranked against what the part already held. Named rather than only
       counted, because a name here is what the next run's --wear takes. */
    char alternatives[WARDROBE_PART_COUNT][WARDROBE_ALTERNATIVE_LIMIT][WARDROBE_NAME_LIMIT];
    Unsigned32 alternativeCount[WARDROBE_PART_COUNT];
    /* Candidates a part passed over after its store was full. Counted, because
       a clothing page that stops at a hundred and twenty-eight and says nothing
       looks exactly like a disc that carries no more. */
    Unsigned32 alternativesBeyondRoom[WARDROBE_PART_COUNT];

    /* A garment asked for by name for ONE part.
     *
     * The single `wanted` above is a substring matched against every part, which
     * is right for a command-line argument — somebody types `cowboy` and means
     * whatever that turns out to be. It is wrong for a menu, where a top and a
     * bottom are chosen one after the other and both have to stick: a single
     * preference would have the second choice forget the first.
     *
     * Exact, and outranking the substring, so choosing a garment from a list of
     * real names beats a guess typed at a shell. */
    char wantedPerPart[WARDROBE_PART_COUNT][WARDROBE_NAME_LIMIT];
} Wardrobe;

/* Empties it and records what to look for.
 *
 * `archetype` is the age and gender this Sim is — "am", "cf", "tu" — and every
 * part's mark is composed from it. Empty or null dresses nobody, which is the
 * right answer rather than a default: a wardrobe that quietly fell back to an
 * adult male would dress the wrong Sim rather than refuse. Either of wanted and
 * tone may be null or empty. */
void wardrobeBegin(Wardrobe *wardrobe, const char *archetype, const char *wanted,
                   const char *tone);

/* Asks for one named garment for one part, exactly. Call after wardrobeBegin,
   which clears these. An empty name asks for nothing, which is how a choice is
   taken back. */
void wardrobeWant(Wardrobe *wardrobe, Unsigned32 part, const char *entryName);
const char *wardrobeGetWanted(const Wardrobe *wardrobe, Unsigned32 part);

/* Offers one catalogue entry. Returns the part it should dress — in which case
   the caller records whatever it holds for that part, replacing what it held
   before — or WARDROBE_NOT_WORN.
 *
 * Only ever called for an entry whose shape index resolved to a shape that is
 * actually on this disc: an entry naming a mesh the disc does not carry is not
 * a wardrobe question. */
Unsigned32 wardrobeOffer(Wardrobe *wardrobe, const char *entryName, Unsigned32 outfitSlot);

/* What a part's name must contain, and which outfit slot dresses it. For the
   log, and for a check to read the rule out of the same place it is applied
   from. The mark depends on who the Sim is; the slot does not. */
const char *wardrobeGetPartMark(const Wardrobe *wardrobe, Unsigned32 part);
/* What a part is called, in words — "body", "face", "hair", "top", "bottom". */
const char *wardrobeGetPartName(Unsigned32 part);
Unsigned32 wardrobeGetPartOutfit(Unsigned32 part);
/* What the part will not wear whatever else recommends it. */
const char *wardrobeGetPartWorn(Unsigned32 part);

/* How this Sim's torso and legs are covered.
 *
 * A top and a bottom are worn together or not at all: half a pair is a Sim in a
 * shirt and nothing else, and the whole body underneath is the same volume
 * again, so drawing both puts two meshes through each other. */
typedef enum WardrobeArrangement
{
    /* The one whole-body garment the wardrobe settled on. */
    WARDROBE_ARRANGEMENT_WHOLE = 0,
    /* A top and a bottom, which between them replace the whole body. */
    WARDROBE_ARRANGEMENT_PAIR,
    /* Neither slot filled; the Sim keeps the body the assembly built. */
    WARDROBE_ARRANGEMENT_AS_ASSEMBLED
} WardrobeArrangement;

WardrobeArrangement wardrobeGetArrangement(const Wardrobe *wardrobe);
/* Whether this part is one the arrangement actually draws. A top chosen and
   then not worn because no bottom turned up is still a choice that was made,
   and the report says so — but it must not reach the model. */
Boolean wardrobeIsWorn(const Wardrobe *wardrobe, Unsigned32 part);

Boolean wardrobeIsChosen(const Wardrobe *wardrobe, Unsigned32 part);
const char *wardrobeGetChosenName(const Wardrobe *wardrobe, Unsigned32 part);
Unsigned32 wardrobeGetChosenCount(const Wardrobe *wardrobe);
/* One of the entries this part could have worn instead. Empty past the end. */
const char *wardrobeGetAlternative(const Wardrobe *wardrobe, Unsigned32 part, Unsigned32 which);
Unsigned32 wardrobeGetAlternativeCount(const Wardrobe *wardrobe, Unsigned32 part);
Unsigned32 wardrobeGetAlternativesBeyondRoom(const Wardrobe *wardrobe, Unsigned32 part);

#endif
