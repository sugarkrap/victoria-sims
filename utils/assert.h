#ifndef VICTORIA_UTILITIES_ASSERT_HEADER
#define VICTORIA_UTILITIES_ASSERT_HEADER

#include "victoria/coreTypes.h"

/* The reporting half of the verifiers under tests/.
 *
 * Every verifier had its own copy of these two, which meant three places to
 * change the moment the output format needed to say anything new, and three
 * chances for one of them to quietly stop counting.
 *
 * This is host-side test support and is never linked into the engine: it prints,
 * which the engine has no way to do on a freestanding target, and the engine has
 * no business asserting at run time in any case. Failures there are values a
 * caller checks, not a process that stops.
 *
 * The count lives with the caller rather than in a global here, so two suites in
 * one binary cannot contaminate each other's tally. */

/* Reports one check and adds to failureCount if it did not hold. Returns
 * whether it held, so a caller can skip work that depends on it.
 *
 * The condition is an Integer32 rather than a Boolean on purpose: Boolean is a
 * byte, and a caller passing a mask or a count straight in would have it
 * truncated on the way — 0x100 would arrive as false. */
Boolean checkThat(Integer32 *failureCount, const char *description, Integer32 condition);

/* Prints the tally and returns what main should. */
Integer32 checkSummarize(Integer32 failureCount, const char *subject);

#endif
