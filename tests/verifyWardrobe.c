
#include <stdio.h>

#include "utils/assert.h"
#include "utils/strings.h"
#include "victoria/wardrobe.h"

static Integer32 failureCount = 0;

#define PART_BODY WARDROBE_PART_BODY
#define PART_FACE WARDROBE_PART_FACE
#define PART_HAIR WARDROBE_PART_HAIR
#define PART_TOP WARDROBE_PART_TOP
#define PART_BOTTOM WARDROBE_PART_BOTTOM

static void offerOneOfEach(Wardrobe *wardrobe)
{
    (void)wardrobeOffer(wardrobe, "ambodyparka_green", 0x08U);
    (void)wardrobeOffer(wardrobe, "amtopcowboyshirt_brownstriped", 0x04U);
    (void)wardrobeOffer(wardrobe, "ambottomlongshorts_tantan", 0x10U);
}

int main(void)
{
    Wardrobe wardrobe;

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

    {
        wardrobeBegin(&wardrobe, "am", "", "");
        checkThat(&failureCount, "a CAS-prefixed name still matches its mark",
                  wardrobeOffer(&wardrobe, "CASIE_ambodytracksuit_blue", 0x08U) == PART_BODY);
    }

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

    {
        wardrobeBegin(&wardrobe, "am", "", "");
        (void)wardrobeOffer(&wardrobe, "ambottomlongshorts_tantan", 0x10U);
        checkThat(&failureCount, "a bottom with no top beside it is not worn",
                  !wardrobeIsWorn(&wardrobe, PART_BOTTOM) &&
                      wardrobeGetArrangement(&wardrobe) == WARDROBE_ARRANGEMENT_AS_ASSEMBLED);
    }

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

    {
        wardrobeBegin(&wardrobe, "am", "", "");
        checkThat(&failureCount, "an unnamed entry dresses nothing",
                  wardrobeOffer(&wardrobe, "", 0x08U) == (Unsigned32)WARDROBE_NOT_WORN);
        checkThat(&failureCount, "counted as unnamed and not as a bad name",
                  wardrobe.refusedUnnamed == 1U && wardrobe.refusedByMark == 0U);
    }

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

    {
        wardrobeBegin(&wardrobe, "am", "", "s1");
        checkThat(&failureCount, "a body whose name carries no tone is still worn",
                  wardrobeOffer(&wardrobe, "ambodyparka_green", 0x08U) == PART_BODY);
        checkThat(&failureCount, "and so is a hair",
                  wardrobeOffer(&wardrobe, "amhairshortgel_black", 0x01U) == PART_HAIR);
    }

    {
        wardrobeBegin(&wardrobe, "am", "", "s1");
        checkThat(&failureCount, "a name merely ending in the tone's letters carries no tone",
                  wardrobeOffer(&wardrobe, "CASIE_amfaces1", 0x02U) ==
                      (Unsigned32)WARDROBE_NOT_WORN);
        checkThat(&failureCount, "while the separator makes it one",
                  wardrobeOffer(&wardrobe, "CASIE_amface_s1", 0x02U) == PART_FACE &&
                      wardrobe.toneMatched[PART_FACE]);
    }

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

    {
        wardrobeBegin(&wardrobe, "am", "alien", "s1");
        checkThat(&failureCount, "a toneless face is refused even when asked for by name",
                  wardrobeOffer(&wardrobe, "amface_alien", 0x02U) ==
                      (Unsigned32)WARDROBE_NOT_WORN);
        checkThat(&failureCount, "as a nude body is",
                  wardrobeOffer(&wardrobe, "CASIE_ambodynaked_nude_s1", 0x08U) ==
                      (Unsigned32)WARDROBE_NOT_WORN);
    }

    {
        wardrobeBegin(&wardrobe, "am", "sweaterpants", "");
        checkThat(&failureCount, "what was asked for is still refused if it is the wrong Sim",
                  wardrobeOffer(&wardrobe, "cmbodylongsweaterpants_purplepattern", 0x08U) ==
                      (Unsigned32)WARDROBE_NOT_WORN);
        checkThat(&failureCount, "and refused before it was ever ranked against the name",
                  wardrobe.refusedByMark == 1U && wardrobe.matchedWanted == 0U);
    }

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

    {
        Wardrobe menu;

        wardrobeBegin(&menu, "am", "", "s1");
        wardrobeWant(&menu, WARDROBE_PART_TOP, "amtopcowboyshirt_brownstriped");
        wardrobeWant(&menu, WARDROBE_PART_BOTTOM, "ambottomshorts_blueplaid");
        checkThat(&failureCount, "a part remembers what it was asked for",
                  stringEquals(wardrobeGetWanted(&menu, WARDROBE_PART_TOP),
                               "amtopcowboyshirt_brownstriped"));

        (void)wardrobeOffer(&menu, "amtopplainshirt_s1", 0x04U);
        (void)wardrobeOffer(&menu, "amtopcowboyshirt_brownstriped", 0x04U);
        (void)wardrobeOffer(&menu, "ambottomjeans_s1", 0x10U);
        (void)wardrobeOffer(&menu, "ambottomshorts_blueplaid", 0x10U);
        checkThat(&failureCount, "and wears it over what it met first",
                  stringEquals(wardrobeGetChosenName(&menu, WARDROBE_PART_TOP),
                               "amtopcowboyshirt_brownstriped"));
        checkThat(&failureCount, "for both parts at once, which one preference could not do",
                  stringEquals(wardrobeGetChosenName(&menu, WARDROBE_PART_BOTTOM),
                               "ambottomshorts_blueplaid"));

        (void)wardrobeOffer(&menu, "amtopanothershirt_s1", 0x04U);
        checkThat(&failureCount, "and a later candidate with the right tone does not displace it",
                  stringEquals(wardrobeGetChosenName(&menu, WARDROBE_PART_TOP),
                               "amtopcowboyshirt_brownstriped"));

        {
            Wardrobe both;

            wardrobeBegin(&both, "am", "plain", "s1");
            wardrobeWant(&both, WARDROBE_PART_TOP, "amtopcowboyshirt_brownstriped");
            (void)wardrobeOffer(&both, "amtopplainshirt_s1", 0x04U);
            (void)wardrobeOffer(&both, "amtopcowboyshirt_brownstriped", 0x04U);
            checkThat(&failureCount, "an exact request outranks a fragment",
                      stringEquals(wardrobeGetChosenName(&both, WARDROBE_PART_TOP),
                                   "amtopcowboyshirt_brownstriped"));
        }

        wardrobeBegin(&menu, "am", "", "s1");
        wardrobeWant(&menu, WARDROBE_PART_TOP, "");
        (void)wardrobeOffer(&menu, "amtopplainshirt_s1", 0x04U);
        checkThat(&failureCount, "and asking for nothing leaves the catalogue to decide",
                  stringEquals(wardrobeGetChosenName(&menu, WARDROBE_PART_TOP),
                               "amtopplainshirt_s1"));
        checkThat(&failureCount, "a part past the end is refused rather than written past",
                  stringEquals(wardrobeGetWanted(&menu, 99U), ""));
    }

    {
        Wardrobe many;
        Unsigned32 index;

        wardrobeBegin(&many, "am", "", "s1");
        for (index = 0U; index < (Unsigned32)WARDROBE_ALTERNATIVE_LIMIT + 5U; index++)
        {
            char name[64];
            char number[16];

            name[0] = '\0';
            (void)stringAppend(name, sizeof(name), "amtopshirt");
            (void)stringWriteUnsigned(number, sizeof(number), index);
            (void)stringAppend(name, sizeof(name), number);
            (void)stringAppend(name, sizeof(name), "_s1");
            (void)wardrobeOffer(&many, name, 0x04U);
        }
        checkThat(&failureCount, "a part remembers as many candidates as there is room for",
                  wardrobeGetAlternativeCount(&many, WARDROBE_PART_TOP) ==
                      (Unsigned32)WARDROBE_ALTERNATIVE_LIMIT);
        checkThat(&failureCount, "and counts the ones it could not, rather than dropping them "
                                 "in silence",
                  wardrobeGetAlternativesBeyondRoom(&many, WARDROBE_PART_TOP) == 5U);
        checkThat(&failureCount, "and every one of them can be read back by name",
                  stringEquals(wardrobeGetAlternative(&many, WARDROBE_PART_TOP, 0U),
                               "amtopshirt0_s1") &&
                      stringEquals(wardrobeGetAlternative(&many, WARDROBE_PART_TOP,
                                                          (Unsigned32)WARDROBE_ALTERNATIVE_LIMIT -
                                                              1U),
                                   "amtopshirt127_s1"));
        checkThat(&failureCount, "a part that was offered nothing has nothing to offer",
                  wardrobeGetAlternativeCount(&many, WARDROBE_PART_HAIR) == 0U);
    }

    return checkSummarize(failureCount, "wardrobe");
}
