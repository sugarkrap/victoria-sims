/* The string helpers, which are not a convenience layer over the C library —
   they are the only implementation the engine has. A bug here is a bug in every
   log line, every path comparison and every diagnostic that was supposed to
   explain some other bug.

   The hexadecimal writer earns its own checks because it exists to print format
   identifiers and version marks, and those are quoted in hexadecimal in every
   reference there is. A number printed wrong in a diagnostic is worse than no
   diagnostic: it sends the next reader looking in the wrong place. */

#include <stdio.h>

#include "utils/assert.h"
#include "utils/strings.h"

static Integer32 failureCount = 0;

static Boolean writesHexadecimal(Unsigned64 value, MemorySize digitCount, const char *expected)
{
    char buffer[32];

    if (stringWriteHexadecimal(buffer, sizeof(buffer), value, digitCount) == 0UL)
    {
        return BOOLEAN_FALSE;
    }
    return stringEquals(buffer, expected);
}

int main(void)
{
    printf("-- writing hexadecimal --\n");
    checkThat(&failureCount, "writes a collection mark the way it is documented",
              writesHexadecimal(0xFFFF0001ULL, 8UL, "0xFFFF0001"));
    checkThat(&failureCount, "pads to the width asked for",
              writesHexadecimal(0x1FULL, 8UL, "0x0000001F"));
    checkThat(&failureCount, "and to a narrower one", writesHexadecimal(0xACULL, 2UL, "0xAC"));
    checkThat(&failureCount, "writes zero", writesHexadecimal(0ULL, 4UL, "0x0000"));
    /* Upper case throughout: a log line that has to be case folded before it can
       be searched for is a log line that will not be. */
    checkThat(&failureCount, "in upper case", writesHexadecimal(0xABCDEFULL, 6UL, "0xABCDEF"));
    checkThat(&failureCount, "keeps the whole of a wide value",
              writesHexadecimal(0x0123456789ABCDEFULL, 16UL, "0x0123456789ABCDEF"));

    printf("\n-- refusing what it cannot fit --\n");
    {
        char small[6];

        checkThat(&failureCount, "refuses a buffer too small for the digits",
                  stringWriteHexadecimal(small, sizeof(small), 0xFFFF0001ULL, 8UL) == 0UL);
        /* Terminated even on refusal, so a caller that appends the buffer anyway
           adds nothing rather than whatever was on the stack. */
        checkThat(&failureCount, "and leaves it holding an empty string", small[0] == '\0');
        checkThat(&failureCount, "refuses a zero length buffer",
                  stringWriteHexadecimal(small, 0UL, 1ULL, 8UL) == 0UL);
    }

    printf("\n-- a nonsense width falls back rather than overruns --\n");
    checkThat(&failureCount, "treats zero digits as eight",
              writesHexadecimal(0xFFFF0001ULL, 0UL, "0xFFFF0001"));
    checkThat(&failureCount, "treats more than sixteen as eight",
              writesHexadecimal(0xFFFF0001ULL, 40UL, "0xFFFF0001"));

    printf("\n-- writing decimal --\n");
    {
        char buffer[24];

        stringWriteUnsigned(buffer, sizeof(buffer), 0ULL);
        checkThat(&failureCount, "writes zero as one digit", stringEquals(buffer, "0"));
        stringWriteUnsigned(buffer, sizeof(buffer), 4294967295ULL);
        checkThat(&failureCount, "writes the widest word", stringEquals(buffer, "4294967295"));
    }

    printf("\n-- folding case --\n");
    checkThat(&failureCount, "folds a letter", characterToLowerCase('Q') == 'q');
    checkThat(&failureCount, "leaves one already folded", characterToLowerCase('q') == 'q');
    /* Deliberately only the twenty-six: anything wider depends on a locale, and
       the callers here compare paths off a disc pressed in 2004. */
    checkThat(&failureCount, "leaves a digit alone", characterToLowerCase('7') == '7');
    checkThat(&failureCount, "leaves a separator alone", characterToLowerCase('/') == '/');

    printf("\n-- appending --\n");
    {
        char buffer[8];

        buffer[0] = '\0';
        stringAppend(buffer, sizeof(buffer), "abc");
        checkThat(&failureCount, "appends to an empty string", stringEquals(buffer, "abc"));
        stringAppend(buffer, sizeof(buffer), "defghij");
        checkThat(&failureCount, "truncates rather than overruns", stringEquals(buffer, "abcdefg"));
    }

    printf("\n-- recognising a file extension --\n");
    /* Three callers used to carry their own copy of this, all of them deciding
       whether a path off a disc is a package. A disc written with 8.3 names
       spells it one way and one written with Joliet another. */
    checkThat(&failureCount, "matches the plain spelling",
              stringEndsWithIgnoringCase("TSData/Res/Sims3D/Sims01.package", ".package"));
    checkThat(&failureCount, "and the shouted one",
              stringEndsWithIgnoringCase("TSDATA/RES/SIMS3D/SIMS01.PACKAGE", ".package"));
    checkThat(&failureCount, "and a suffix that is itself shouted",
              stringEndsWithIgnoringCase("sims01.package", ".PACKAGE"));
    checkThat(&failureCount, "refuses a different extension",
              !stringEndsWithIgnoringCase("setup.cab", ".package"));
    /* The suffix appearing anywhere but the end is not a match, which a
       comparison that searched rather than anchored would get wrong. */
    checkThat(&failureCount, "refuses it in the middle of the path",
              !stringEndsWithIgnoringCase("a.package/inside.txt", ".package"));
    checkThat(&failureCount, "refuses a path shorter than the suffix",
              !stringEndsWithIgnoringCase("a.p", ".package"));
    checkThat(&failureCount, "matches a path that is only the suffix",
              stringEndsWithIgnoringCase(".package", ".package"));
    checkThat(&failureCount, "and an empty suffix matches anything",
              stringEndsWithIgnoringCase("anything", ""));

    return checkSummarize(failureCount, "strings");
}
