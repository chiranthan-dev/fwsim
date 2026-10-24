/*
 * smart.h - SMART attributes, their 7/30-day history and the on-drive
 * failure-prediction model (an exported logistic regression).
 *
 * Where the attributes come from (fwsim's own counters, see media.c):
 *
 *     ID   name                     fwsim source
 *     5    Reallocated_Sector_Ct    LBAs moved to spares (disk-sim G-list)
 *     9    Power_On_Hours           simulated drive time
 *     187  Reported_Uncorrect       reads that failed every CRC retry
 *     188  Command_Timeout          0 (fwsim commands never time out)
 *     194  Temperature_Celsius      host-supplied input (no thermal model)
 *     197  Current_Pending_Sector   unreadable LBAs waiting for a rewrite
 *     198  Offline_Uncorrectable    0 (fwsim has no offline surface scan)
 *
 * History: one sample per simulated DAY in a 32-entry ring, enough for
 * "how much did counter X grow over the last 7 / 30 days" - the deltas the
 * model was trained on (tools/smart/features.py computes them the same
 * way from Backblaze's daily snapshots).
 *
 * Everything here is integer-only, allocation-free and owned by the
 * firmware thread. The module does not know about fw_t, so the unit tests
 * drive it directly.
 */
#ifndef FWSIM_SMART_H
#define FWSIM_SMART_H

#include <stdint.h>

#include "fwsim.h"
#include "smart_model.h"

/* The failure-related counters, in the model's feature order. */
enum { SMART_C_5 = 0, SMART_C_187, SMART_C_188, SMART_C_197, SMART_C_198, SMART_NCOUNTERS };

#define SMART_HIST_DAYS  32u          /* > 30 + today */
#define SMART_MS_PER_DAY 86400000ull
#define SMART_MS_PER_HOUR 3600000ull

typedef struct {
    uint64_t day;                     /* simulated day number */
    uint64_t c[SMART_NCOUNTERS];      /* counter values at the end of that day */
} smart_sample_t;

typedef struct {
    smart_sample_t hist[SMART_HIST_DAYS];
    uint32_t head;                    /* index of the newest sample */
    uint32_t n;                       /* valid samples (<= SMART_HIST_DAYS) */
    uint64_t now_ms;                  /* simulated time of the last update */
    uint64_t c[SMART_NCOUNTERS];      /* current values */
    uint32_t temp_c;
    uint8_t  worst[FW_SMART_NATTR];   /* lowest normalised value seen */
} smart_t;

void smart_init(smart_t *s);

/* Record the counters as of simulated time now_ms (must not go backwards).
 * Within a day the newest sample is overwritten; crossing into a new day
 * appends one; days skipped without an update (no command ran, so no
 * counter could change) carry the previous values, and today gets the
 * current ones: growth shows up on the day it was observed. */
void smart_update(smart_t *s, uint64_t now_ms, const uint64_t c[SMART_NCOUNTERS],
                  uint32_t temp_c);

/* Growth of counter k over the last `days` days: current value minus the
 * oldest sample inside [today - days, today]. 0 if it went down. */
uint64_t smart_delta(const smart_t *s, int k, uint32_t days);

/* 16*log2(1+x), integer approximation (identical to smartlib.lg_fx). */
int32_t smart_lg(uint64_t x);

/* The model's feature vector (order: SMART_FEATURE_NAMES). */
void smart_features(const smart_t *s, int16_t f[SMART_MODEL_NFEAT]);

/* Model: log-odds in 1/256 units, and P(fail within 30 days) in basis
 * points (1/10000). */
int32_t  smart_logit_q8(const int16_t f[SMART_MODEL_NFEAT]);
uint16_t smart_risk_bp(int32_t logit_q8);

/* Fill the SMART_READ payload: attribute table + prediction. */
void smart_report(smart_t *s, fw_smart_data_t *out);

#endif
