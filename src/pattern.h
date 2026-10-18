/*
 * pattern.h - self-describing sector contents for verification.
 *
 * Every sector the host writes in a verified test carries a header with
 * its own LBA and a per-LBA version number, followed by pseudo-random bytes
 * derived from (lba, version). Reading a sector back therefore tells us
 * exactly WHICH write it came from - essential for the power-loss test,
 * where "the data is there" is not enough: it must be the latest
 * acknowledged version, not an older one.
 */
#ifndef FWSIM_PATTERN_H
#define FWSIM_PATTERN_H

#include <stdint.h>

#define PAT_OK    1    /* valid pattern, *version filled in       */
#define PAT_ZERO  0    /* all zeros: never written                */
#define PAT_BAD  (-1)  /* garbage, wrong LBA, or corrupted payload */

void pattern_fill(void *sector, uint32_t size, uint64_t lba, uint32_t version);
int  pattern_check(const void *sector, uint32_t size, uint64_t lba, uint32_t *version);

#endif
