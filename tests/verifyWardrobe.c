/* Checks the rule that decides which catalogue entry a Sim wears.
 *
 * Everything else in the chain from a catalogue entry to a mesh is arithmetic
 * the disc either agrees with or does not: a key list has a version, an index
 * lands inside it or past its end, a shape is on this disc or is not. Getting
 * any of that wrong shows up as a refusal, in a log line, on the first run.
 *
 * Choosing is not like that. A wrong choice resolves perfectly and draws a Sim
 * wearing somebody else's body — a child's mesh on an adult's skeleton, or a
 * face one tone off the neck under it. There is no refusal to read, and the
 * disc cannot be asked whether the answer was right. So the rule is written
 * where a check can state the claim and then break it, which is the only kind
 * of evidence available for a decision.
 *
 * Every case below is one this could plausibly get wrong, and two of them are
 * ones the first version did. */

#include <stdio.h>

#include "utils/assert.h"
#include "utils/strings.h"
#include "victoria/wardrobe.h"

static Integer32 failureCount = 0;

/* The parts, by the order they are joined. */
#define PART_BODY WARDROBE_PART_BODY
#define PART_FACE WARDROBE_PART_FACE
#define PART_HAIR WARDROBE_PART_HAIR
#define PART_TOP WARDROBE_PART_TOP
#define PART_BOTTOM WARDROBE_PART_BOTTOM

/* Dresses a Sim in one of each so an arrangement has something to arrange. */
static void offerOneOfEach(Wardrobe *wardrobe)
{
    (void)wardrobeOffer(wardrobe, "ambodyparka_green", 0x08U);
    (void)wardrobeOffer(wardrobe, "amtopcowboyshirt_brownstriped", 0x04U);
    (void)wardrobeOffer(wardrobe, "ambottomlongshorts_tantan", 0x10U);
}

int main(void)
{
    Wardrobe wardrobe;

    /* The rule table itself, because everything below is read against it and a
       check that agreed with a table it had also written would prove nothing.
       These are the values measured off a retail disc's catalogue. */
    /* The marks are composed from who the Sim is, not written down per part.
       An engine whose table said "amhair" could dress exactly one person. */
    {
        wardrobeBegin(&wardrobe, "cf", "", "");
        checkThat(&failureCount, "a part's mark is the archetype and then the part",
                  stringEquals(wardrobeGetPartMark(&wardrobe, PART_HAIR), "cfhair") &&
                      stringEquals(wardrobeGetPartMark(&wardrobe, PART_BOTTOM), "cfbottom"));
        checkThat(&failureCount, "so a child female wears a child female's clothes",
                  wardrobeOffer(&wardrobe, "cfbodyromper_yellow", 0x08U) == PART_BODY);
        checkThat(&failureCount, "and an adult male's are refused for her",
                  wardrobeOffer(&wardrobe, "ambodyparka_green", 0x08U) ==
                      (Unsigned32)WARDROBE_NOT_WORN);
    }

    /* No archetype means nobody to dress. An empty mark is contained in every
       name, so falling back rather than refusing would dress this Sim in the
       first garment of every slot on the disc. */
    {
        wardrobeBegin(&wardrobe, "", "", "");
        checkThat(&failureCount, "a Sim who is nobody wears nothing",
                  wardrobeOffer(&wardrobe, "ambodyparka_green", 0x08U) ==
                      (Unsigned32)WARDROBE_NOT_WORN &&
                      wardrobeOffer(&wardrobe, "cfbodyromper_yellow", 0x08U) ==
                      (Unsigned32)WARDROBE_NOT_WORN);
        checkThat(&failureCount, "and nothing is quietly taken instead",
                  wardrobeGetChosenCount(&wardrobe) == 0U && wardrobe.refusedByMark == 2U);
    }

    checkThat(&failureCount, "the body is dressed by the whole-body slot",
              (Integer32)wardrobeGetPartOutfit(PART_BODY) == 0x08);
    checkThat(&failureCount, "the face is dressed by the face slot",
              (Integer32)wardrobeGetPartOutfit(PART_FACE) == 0x02);
    checkThat(&failureCount, "the hair is dressed by the hair slot",
              (Integer32)wardrobeGetPartOutfit(PART_HAIR) == 0x01);
    checkThat(&failureCount, "the top is dressed by the top slot",
              (Integer32)wardrobeGetPartOutfit(PART_TOP) == 0x04);
    checkThat(&failureCount, "the bottom is dressed by the bottom slot",
              (Integer32)wardrobeGetPartOutfit(PART_BOTTOM) == 0x10);

    /* Nothing asked for and no tone known: the first of each slot is taken. */
    {
        wardrobeBegin(&wardrobe, "am", "", "");
        checkThat(&failureCount, "an adult male body outfit dresses the body",
                  wardrobeOffer(&wardrobe, "ambodyswimwear_redtrunks", 0x08U) == PART_BODY);
        checkThat(&failureCount, "an adult male hair dresses the hair",
                  wardrobeOffer(&wardrobe, "amhairshortgel_black", 0x01U) == PART_HAIR);
        checkThat(&failureCount, "and both are held",
                  wardrobeGetChosenCount(&wardrobe) == 2U &&
                      stringEquals(wardrobeGetChosenName(&wardrobe, PART_BODY),
                                   "ambodyswimwear_redtrunks"));
        checkThat(&failureCount, "the face is not dressed by anything that was offered",
                  !wardrobeIsChosen(&wardrobe, PART_FACE));
    }

    /* The guard that matters. Every part hangs on auskel_cres, so a mesh
       authored for any other skeleton must never be taken — it resolves
       perfectly and comes apart on the first pose. */
    {
        wardrobeBegin(&wardrobe, "am", "", "");
        checkThat(&failureCount, "a child's body is refused",
                  wardrobeOffer(&wardrobe, "cmbodylongsweaterpants_purplepattern", 0x08U) ==
                      (Unsigned32)WARDROBE_NOT_WORN);
        checkThat(&failureCount, "an adult female's body is refused",
                  wardrobeOffer(&wardrobe, "afbodyburglar", 0x08U) ==
                      (Unsigned32)WARDROBE_NOT_WORN);
        checkThat(&failureCount, "a teen's hair is refused",
                  wardrobeOffer(&wardrobe, "tmhairshortspikey_black", 0x01U) ==
                      (Unsigned32)WARDROBE_NOT_WORN);
        checkThat(&failureCount, "an elder's face is refused",
                  wardrobeOffer(&wardrobe, "CASIE_emface_s1", 0x02U) ==
                      (Unsigned32)WARDROBE_NOT_WORN);
        checkThat(&failureCount, "so nothing at all is worn",
                  wardrobeGetChosenCount(&wardrobe) == 0U && wardrobe.refusedByMark == 4U);
    }

    /* The prefix appears in the middle of a CAS entry's name, so the match
       cannot be anchored at the front. It was, first time round, and every
       catalogue entry on the disc begins CASIE_. */
    {
        wardrobeBegin(&wardrobe, "am", "", "");
        checkThat(&failureCount, "a CAS-prefixed name still matches its mark",
                  wardrobeOffer(&wardrobe, "CASIE_ambodytracksuit_blue", 0x08U) == PART_BODY);
    }

    /* A slot naming more than one part at once. 0x18 is a bottom and a whole
       body together, and there is no answer to which of the two it is — wearing
       either half is worse than not wearing it. */
    {
        wardrobeBegin(&wardrobe, "am", "", "");
        checkThat(&failureCount, "a top dresses the top",
                  wardrobeOffer(&wardrobe, "amtopjackettshirthang_grey", 0x04U) == PART_TOP);
        checkThat(&failureCount, "a bottom dresses the bottom",
                  wardrobeOffer(&wardrobe, "ambottomcargopants_black", 0x10U) == PART_BOTTOM);
        checkThat(&failureCount, "a slot naming two parts at once dresses neither",
                  wardrobeOffer(&wardrobe, "ambottomnaked_alien", 0x18U) ==
                      (Unsigned32)WARDROBE_NOT_WORN);
        checkThat(&failureCount, "counted against the slot rather than the name",
                  wardrobe.refusedBySlot == 1U && wardrobe.refusedByMark == 0U);
    }

    /* A top and a bottom are worn together or not at all. They and the whole
       body are the same volume of Sim described two ways, and joining both puts
       a pair of trousers through a pair of legs that are already there. */
    {
        wardrobeBegin(&wardrobe, "am", "", "");
        checkThat(&failureCount, "nothing offered leaves the Sim as it was assembled",
                  wardrobeGetArrangement(&wardrobe) == WARDROBE_ARRANGEMENT_AS_ASSEMBLED);

        (void)wardrobeOffer(&wardrobe, "ambodyparka_green", 0x08U);
        checkThat(&failureCount, "a whole body alone is worn whole",
                  wardrobeGetArrangement(&wardrobe) == WARDROBE_ARRANGEMENT_WHOLE &&
                      wardrobeIsWorn(&wardrobe, PART_BODY));

        (void)wardrobeOffer(&wardrobe, "amtopcowboyshirt_brownstriped", 0x04U);
        checkThat(&failureCount, "a top with no bottom beside it is not worn",
                  wardrobeGetArrangement(&wardrobe) == WARDROBE_ARRANGEMENT_WHOLE &&
                      !wardrobeIsWorn(&wardrobe, PART_TOP));
        checkThat(&failureCount, "though it is still recorded as chosen",
                  wardrobeIsChosen(&wardrobe, PART_TOP));

        (void)wardrobeOffer(&wardrobe, "ambottomlongshorts_tantan", 0x10U);
        checkThat(&failureCount, "a bottom completes the pair, which then replaces the body",
                  wardrobeGetArrangement(&wardrobe) == WARDROBE_ARRANGEMENT_PAIR &&
                      wardrobeIsWorn(&wardrobe, PART_TOP) &&
                      wardrobeIsWorn(&wardrobe, PART_BOTTOM));
        checkThat(&failureCount, "and the whole body is chosen and no longer worn",
                  wardrobeIsChosen(&wardrobe, PART_BODY) && !wardrobeIsWorn(&wardrobe, PART_BODY));
        checkThat(&failureCount, "the face and hair are worn under either arrangement",
                  wardrobeOffer(&wardrobe, "amhairshortgel_black", 0x01U) == PART_HAIR &&
                      wardrobeIsWorn(&wardrobe, PART_HAIR));
    }

    /* A bottom on its own is exactly as wrong as a top on its own, and it is
       worth checking both: the arrangement asks for two things and an && with
       one side dropped still passes half these cases. */
    {
        wardrobeBegin(&wardrobe, "am", "", "");
        (void)wardrobeOffer(&wardrobe, "ambottomlongshorts_tantan", 0x10U);
        checkThat(&failureCount, "a bottom with no top beside it is not worn",
                  !wardrobeIsWorn(&wardrobe, PART_BOTTOM) &&
                      wardrobeGetArrangement(&wardrobe) == WARDROBE_ARRANGEMENT_AS_ASSEMBLED);
    }

    /* Naming a whole-body garment settles the arrangement. Without this the
       pair wins whenever both halves turned up, which is nearly always, and
       --wear could never be pointed at a whole-body garment at all. */
    {
        wardrobeBegin(&wardrobe, "am", "parka", "");
        offerOneOfEach(&wardrobe);
        checkThat(&failureCount, "asking for a whole body by name wears it whole",
                  wardrobeGetArrangement(&wardrobe) == WARDROBE_ARRANGEMENT_WHOLE &&
                      wardrobeIsWorn(&wardrobe, PART_BODY) && !wardrobeIsWorn(&wardrobe, PART_TOP));
    }

    {
        wardrobeBegin(&wardrobe, "am", "cowboyshirt", "");
        offerOneOfEach(&wardrobe);
        checkThat(&failureCount, "asking for a top by name wears the pair",
                  wardrobeGetArrangement(&wardrobe) == WARDROBE_ARRANGEMENT_PAIR &&
                      !wardrobeIsWorn(&wardrobe, PART_BODY) && wardrobeIsWorn(&wardrobe, PART_TOP));
    }

    /* The unnamed entries are groupings. They carry a slot like everything
       else, so the name is the only thing that tells them apart. */
    {
        wardrobeBegin(&wardrobe, "am", "", "");
        checkThat(&failureCount, "an unnamed entry dresses nothing",
                  wardrobeOffer(&wardrobe, "", 0x08U) == (Unsigned32)WARDROBE_NOT_WORN);
        checkThat(&failureCount, "counted as unnamed and not as a bad name",
                  wardrobe.refusedUnnamed == 1U && wardrobe.refusedByMark == 0U);
    }

    /* What the part already wears. The base Sim is naked and bald and the
       catalogue names both of those meshes, so taking one would close the whole
       chain and change nothing on screen.
     *
       The marker is `_nude` and not `naked`, and that distinction is the whole
       rule: `amtopnaked_babybluetank` is a real garment whose MESH is called
       topnaked and whose texture is a tank top. Refusing on `naked` refuses most
       of the tops on the disc. */
    {
        wardrobeBegin(&wardrobe, "am", "", "");
        checkThat(&failureCount, "the naked body the Sim already wears is refused",
                  wardrobeOffer(&wardrobe, "CASIE_ambodynaked_nude_s1", 0x08U) ==
                      (Unsigned32)WARDROBE_NOT_WORN);
        checkThat(&failureCount, "the bald head likewise",
                  wardrobeOffer(&wardrobe, "amhairbald_skin_s1", 0x01U) ==
                      (Unsigned32)WARDROBE_NOT_WORN);
        checkThat(&failureCount, "and a bare torso likewise",
                  wardrobeOffer(&wardrobe, "CASIE_amtopnaked_nude_s3", 0x04U) ==
                      (Unsigned32)WARDROBE_NOT_WORN);
        checkThat(&failureCount, "counted apart from every other refusal",
                  wardrobe.refusedAsWorn == 3U);
        checkThat(&failureCount, "but a garment whose MESH is the bare one is worn",
                  wardrobeOffer(&wardrobe, "amtopnaked_babybluetank", 0x04U) == PART_TOP);
    }

    /* The tone, which is the face's whole point. A face is skin, not something
       worn over it, so one tone off is a head that does not belong to its
       neck. */
    {
        wardrobeBegin(&wardrobe, "am", "", "s3");
        checkThat(&failureCount, "a face of the wrong tone is taken when nothing better is held",
                  wardrobeOffer(&wardrobe, "CASIE_amface_s1", 0x02U) == PART_FACE);
        checkThat(&failureCount, "and is not recorded as matching the tone",
                  !wardrobe.toneMatched[PART_FACE]);
        checkThat(&failureCount, "a face of the right tone displaces it",
                  wardrobeOffer(&wardrobe, "CASIE_amface_s3", 0x02U) == PART_FACE);
        checkThat(&failureCount, "and is the one held",
                  stringEquals(wardrobeGetChosenName(&wardrobe, PART_FACE), "CASIE_amface_s3") &&
                      wardrobe.toneMatched[PART_FACE] && wardrobe.displaced == 1U);
        checkThat(&failureCount, "nothing displaces a face that already matches",
                  wardrobeOffer(&wardrobe, "CASIE_amface_s4", 0x02U) ==
                      (Unsigned32)WARDROBE_NOT_WORN);
        checkThat(&failureCount, "not even another of the same tone",
                  wardrobeOffer(&wardrobe, "amface_s3", 0x02U) == (Unsigned32)WARDROBE_NOT_WORN);
        checkThat(&failureCount, "and the first is still what is worn",
                  stringEquals(wardrobeGetChosenName(&wardrobe, PART_FACE), "CASIE_amface_s3"));
    }

    /* A face with no tone at all is not a face of the wrong colour — it is not
       an ordinary face. An alien and a mannequin sit in the same slot as every
       real face, and ranking them level with a real face of the wrong tone is
       how a teenager came out green. */
    {
        wardrobeBegin(&wardrobe, "tf", "", "s1");
        checkThat(&failureCount, "an alien face is never worn, even with nothing else offered",
                  wardrobeOffer(&wardrobe, "tfface_alien", 0x02U) ==
                      (Unsigned32)WARDROBE_NOT_WORN);
        checkThat(&failureCount, "nor a mannequin, which ends in no digits at all",
                  wardrobeOffer(&wardrobe, "CASIE_tfface_CASmannequin", 0x02U) ==
                      (Unsigned32)WARDROBE_NOT_WORN);
        checkThat(&failureCount, "so the Sim keeps the face the assembly gave her",
                  !wardrobeIsChosen(&wardrobe, PART_FACE) && wardrobe.refusedAsWorn == 2U);
        checkThat(&failureCount, "a real face of the wrong tone is worn",
                  wardrobeOffer(&wardrobe, "CASIE_tfface_s4", 0x02U) == PART_FACE &&
                      wardrobe.toneRank[PART_FACE] == 1U);
        checkThat(&failureCount, "and the right tone displaces it",
                  wardrobeOffer(&wardrobe, "CASIE_tfface_s1", 0x02U) == PART_FACE &&
                      wardrobe.toneRank[PART_FACE] == 2U);
        checkThat(&failureCount, "after which nothing toneless can win it back",
                  wardrobeOffer(&wardrobe, "tfface_alien", 0x02U) ==
                      (Unsigned32)WARDROBE_NOT_WORN &&
                      stringEquals(wardrobeGetChosenName(&wardrobe, PART_FACE),
                                   "CASIE_tfface_s1"));
    }

    /* A garment is not held to that. Its name ends in a colourway, which is not
       a tone and must not be read as one — a rule about faces applied to bodies
       would refuse every garment on the disc. */
    {
        wardrobeBegin(&wardrobe, "am", "", "s1");
        checkThat(&failureCount, "a body whose name carries no tone is still worn",
                  wardrobeOffer(&wardrobe, "ambodyparka_green", 0x08U) == PART_BODY);
        checkThat(&failureCount, "and so is a hair",
                  wardrobeOffer(&wardrobe, "amhairshortgel_black", 0x01U) == PART_HAIR);
    }

    /* The tone is a whole trailing component, not a couple of characters that
       happen to be at the end. Matching "s1" loose would match "CASmannequins1"
       and, worse, a garment named for its colour. */
    {
        wardrobeBegin(&wardrobe, "am", "", "s1");
        checkThat(&failureCount, "a name merely ending in the tone's letters carries no tone",
                  wardrobeOffer(&wardrobe, "CASIE_amfaces1", 0x02U) ==
                      (Unsigned32)WARDROBE_NOT_WORN);
        checkThat(&failureCount, "while the separator makes it one",
                  wardrobeOffer(&wardrobe, "CASIE_amface_s1", 0x02U) == PART_FACE &&
                      wardrobe.toneMatched[PART_FACE]);
    }

    /* A garment's tone is not a tone. "ambodyswimwear_redtrunks" ends the same
       way a face does and means something else entirely, so the body must not
       be chosen on it — if it were, a Sim would end up in whichever garment
       happened to be named after its skin. */
    {
        wardrobeBegin(&wardrobe, "am", "", "redtrunks");
        checkThat(&failureCount, "a body is taken whatever its colourway",
                  wardrobeOffer(&wardrobe, "ambodyparka_green", 0x08U) == PART_BODY);
        checkThat(&failureCount, "and nothing named for the tone displaces it",
                  wardrobeOffer(&wardrobe, "ambodyswimwear_redtrunks", 0x08U) ==
                      (Unsigned32)WARDROBE_NOT_WORN);
        checkThat(&failureCount, "because a garment is settled the moment it is taken",
                  stringEquals(wardrobeGetChosenName(&wardrobe, PART_BODY), "ambodyparka_green") &&
                      wardrobe.displaced == 0U);
    }

    /* Asked for by name. This is how a run is steered at something in
       particular, which is the only way to look at more than whatever the walk
       met first. */
    {
        wardrobeBegin(&wardrobe, "am", "parka", "");
        checkThat(&failureCount, "something else is worn until the one asked for turns up",
                  wardrobeOffer(&wardrobe, "ambodyswimwear_redtrunks", 0x08U) == PART_BODY);
        checkThat(&failureCount, "the one asked for displaces it",
                  wardrobeOffer(&wardrobe, "ambodyparka_green", 0x08U) == PART_BODY &&
                      wardrobe.matchedWanted == 1U);
        checkThat(&failureCount, "and nothing displaces it afterwards",
                  wardrobeOffer(&wardrobe, "ambodyparkalong_blue", 0x08U) ==
                      (Unsigned32)WARDROBE_NOT_WORN);
        checkThat(&failureCount, "not even something else asked for",
                  stringEquals(wardrobeGetChosenName(&wardrobe, PART_BODY), "ambodyparka_green"));
    }

    /* A name asked for is a preference, not a filter. As a filter it read
       perfectly well and dressed the Sim in a hoodie and nothing else: no hair
       and no face is named after a garment, so both were refused and a Sim
       came out clothed and bald. */
    {
        wardrobeBegin(&wardrobe, "am", "parka", "s1");
        checkThat(&failureCount, "a hair nobody asked about is still worn",
                  wardrobeOffer(&wardrobe, "amhairshortgel_black", 0x01U) == PART_HAIR);
        checkThat(&failureCount, "a face nobody asked about is still worn",
                  wardrobeOffer(&wardrobe, "CASIE_amface_s1", 0x02U) == PART_FACE);
        checkThat(&failureCount, "and the part that was asked about still gets what it asked for",
                  wardrobeOffer(&wardrobe, "ambodyparka_green", 0x08U) == PART_BODY &&
                      wardrobeGetChosenCount(&wardrobe) == 3U);
    }

    /* Asked for outranks the right tone, because someone who names a face wants
       that face whatever tone it turns out to be — and having named it, is not
       then overruled by the tone rule they were working around. */
    {
        wardrobeBegin(&wardrobe, "am", "s3", "s1");
        checkThat(&failureCount, "a face of the right tone is worn first",
                  wardrobeOffer(&wardrobe, "CASIE_amface_s1", 0x02U) == PART_FACE &&
                      wardrobe.toneMatched[PART_FACE]);
        checkThat(&failureCount, "and the one asked for displaces it despite the tone",
                  wardrobeOffer(&wardrobe, "CASIE_amface_s3", 0x02U) == PART_FACE);
        checkThat(&failureCount, "and the right tone does not win it back",
                  wardrobeOffer(&wardrobe, "CASIE_amface_s1", 0x02U) ==
                      (Unsigned32)WARDROBE_NOT_WORN &&
                      stringEquals(wardrobeGetChosenName(&wardrobe, PART_FACE),
                                   "CASIE_amface_s3"));
    }

    /* Asking for an alien by name does not get one either. It is the same rule
       as a nude body and a bald scalp: what a part already wears is the floor,
       the flag chooses among the things that clear it, and a run that could be
       steered under the floor would be a way to make a Sim worse on purpose. */
    {
        wardrobeBegin(&wardrobe, "am", "alien", "s1");
        checkThat(&failureCount, "a toneless face is refused even when asked for by name",
                  wardrobeOffer(&wardrobe, "amface_alien", 0x02U) ==
                      (Unsigned32)WARDROBE_NOT_WORN);
        checkThat(&failureCount, "as a nude body is",
                  wardrobeOffer(&wardrobe, "CASIE_ambodynaked_nude_s1", 0x08U) ==
                      (Unsigned32)WARDROBE_NOT_WORN);
    }

    /* A name asked for does not get to override the skeleton guard. Someone
       typing a garment they can see in the log should not be able to hang a
       child's mesh on an adult. */
    {
        wardrobeBegin(&wardrobe, "am", "sweaterpants", "");
        checkThat(&failureCount, "what was asked for is still refused if it is the wrong Sim",
                  wardrobeOffer(&wardrobe, "cmbodylongsweaterpants_purplepattern", 0x08U) ==
                      (Unsigned32)WARDROBE_NOT_WORN);
        checkThat(&failureCount, "and refused before it was ever ranked against the name",
                  wardrobe.refusedByMark == 1U && wardrobe.matchedWanted == 0U);
    }

    /* What each part could have worn instead, by name. The flag that steers a
       run takes one of these, so a report that counted them and named none
       would leave --wear with nothing to be pointed at. */
    {
        wardrobeBegin(&wardrobe, "am", "", "");
        (void)wardrobeOffer(&wardrobe, "ambodyparka_green", 0x08U);
        (void)wardrobeOffer(&wardrobe, "ambodyswimwear_redtrunks", 0x08U);
        (void)wardrobeOffer(&wardrobe, "afbodyburglar", 0x08U);
        (void)wardrobeOffer(&wardrobe, "amhairshortgel_black", 0x01U);
        checkThat(&failureCount, "what a part wears is named among what it could wear",
                  wardrobe.alternativeCount[PART_BODY] == 2U &&
                      stringEquals(wardrobeGetAlternative(&wardrobe, PART_BODY, 0),
                                   "ambodyparka_green"));
        checkThat(&failureCount, "and so is the one it turned down",
                  stringEquals(wardrobeGetAlternative(&wardrobe, PART_BODY, 1),
                               "ambodyswimwear_redtrunks"));
        checkThat(&failureCount, "one that never fitted the part is not offered as an alternative",
                  wardrobe.alternativeCount[PART_HAIR] == 1U &&
                      stringEquals(wardrobeGetAlternative(&wardrobe, PART_HAIR, 0),
                                   "amhairshortgel_black"));
        checkThat(&failureCount, "and reading past the end gives an empty name, not a stray one",
                  stringEquals(wardrobeGetAlternative(&wardrobe, PART_BODY, 2), "") &&
                      stringEquals(wardrobeGetAlternative(&wardrobe, PART_FACE, 0), ""));
    }

    /* Everything offered is accounted for. A wardrobe that dresses nothing and
       cannot say why looks exactly like a disc with nothing on it to wear —
       which is precisely the confusion the catalogue work has already been
       through twice. */
    {
        Unsigned32 accounted;

        wardrobeBegin(&wardrobe, "am", "", "s1");
        (void)wardrobeOffer(&wardrobe, "", 0x08U);
        (void)wardrobeOffer(&wardrobe, "amtopnaked_babybluetank", 0x04U);
        (void)wardrobeOffer(&wardrobe, "afbodyburglar", 0x08U);
        (void)wardrobeOffer(&wardrobe, "CASIE_ambodynaked_nude_s1", 0x08U);
        (void)wardrobeOffer(&wardrobe, "ambodyparka_green", 0x08U);
        (void)wardrobeOffer(&wardrobe, "ambodyswimwear_redtrunks", 0x08U);
        accounted = wardrobe.taken + wardrobe.displaced + wardrobe.refusedUnnamed +
                    wardrobe.refusedBySlot + wardrobe.refusedByMark + wardrobe.refusedAsWorn +
                    wardrobe.refusedAsSettled;
        checkThat(&failureCount, "every entry offered is either worn or counted as refused",
                  wardrobe.offered == 6U && accounted == wardrobe.offered);
    }

    return checkSummarize(failureCount, "wardrobe");
}
