#include "utils/assert.h"

#include <stdio.h>

Boolean checkThat(Integer32 *failureCount, const char *description, Integer32 condition)
{
    printf("%s  %s\n", condition ? "ok  " : "FAIL", description);
    if (!condition)
    {
        *failureCount += 1;
    }
    return condition ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

Integer32 checkSummarize(Integer32 failureCount, const char *subject)
{
    if (failureCount > 0)
    {
        printf("\n%d check(s) failed\n", (int)failureCount);
        return 1;
    }
    printf("\nall %s checks passed\n", subject);
    return 0;
}
