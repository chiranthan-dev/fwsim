/*
 * env_monitor.h - the firmware's shock / vibration monitor and the write
 * protection policies that use it.
 *
 *   accelerometer --SPI--> FIFO --(WTM IRQ)--> bottom half --> features
 *        |                                                    --> int8 model
 *        +--(SHOCK threshold IRQ)--> bottom half --+              |
 *                                                  v              v
 *                                     write gate (media_wpolicy_t) <- policy
 *
 * Policies (fw_config_t.vib_policy):
 *   FW_VIB_NONE       P0: no protection at all.
 *   FW_VIB_THRESHOLD  P1, the baseline without AI: the accelerometer's
 *                     threshold interrupt (|a| >= vib_threshold_mg) inhibits
 *                     writes until vib_settle_ms after the last
 *                     over-threshold sample; a write already seeking is
 *                     aborted at the write gate and retried.
 *   FW_VIB_AI         P2: the classifier runs on every hop (16 samples) over
 *                     the last `window` samples:
 *                       shock     -> inhibit until window end + settle, retry
 *                       vibration -> keep writing, but with an extra settle
 *                                    before each write (vib_extra_settle_ms)
 *                                    and verify-after-write
 *                       tap/quiet -> nothing
 *   FW_VIB_HYBRID     P3: P1's threshold interrupt for the fast inhibit, P2's
 *                     vibration response, and when the classifier calls a
 *                     window that contains the triggering sample "tap" or
 *                     "quiet", the threshold's inhibit is released early.
 *
 * All monitor work happens on the firmware thread (the ISR excepted, which
 * runs on the interrupt thread in threaded-IRQ mode). Time is the drive's
 * simulated clock, so everything reproduces from the seed.
 */
#ifndef FWSIM_ENV_MONITOR_H
#define FWSIM_ENV_MONITOR_H

#include "fwsim.h"
#include "media.h"

typedef struct env_monitor env_monitor_t;

/* NULL on error. Uses m->env as the physical world. */
env_monitor_t *envmon_create(media_t *m, const fw_config_t *cfg, fw_counters_t *ctr);
void           envmon_destroy(env_monitor_t *v);

/* media_wgate_fn: bring the monitor up to t_ms, return the directives. */
void envmon_gate(void *ctx, double t_ms, media_wpolicy_t *pol);
/* Bring the monitor towards t_ms without a write (keeps the dashboard
 * live), simulating at most ENVMON_POLL_MAX_MS per call. The monitor costs
 * real CPU per simulated sample: real-time playback (time_scale > 0) needs
 * the host to simulate 1 kHz / time_scale samples per second. */
#define ENVMON_POLL_MAX_MS 64.0
void envmon_poll(env_monitor_t *v, double t_ms);

/* ISR invocations so far (any thread). */
uint64_t envmon_isr_runs(env_monitor_t *v);

const char *vib_policy_str(int p);
int         vib_policy_parse(const char *s);   /* -1 if unknown */

#endif
