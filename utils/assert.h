#ifndef VICTORIA_UTILITIES_ASSERT_HEADER
#define VICTORIA_UTILITIES_ASSERT_HEADER

#include "victoria/coreTypes.h"

Boolean checkThat(Integer32 *failureCount, const char *description, Integer32 condition);

Integer32 checkSummarize(Integer32 failureCount, const char *subject);

#endif
