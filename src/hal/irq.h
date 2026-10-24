/*
 * irq.h - a small interrupt controller: 32 lines, each with
 * enable / mask / pending bits and one handler (the ISR).
 *
 *   device model --irq_raise(line)--> PENDING --(enabled && !masked)--> ISR
 *
 * Semantics (modelled on a Cortex-M NVIC, simplified):
 *   - irq_raise() latches the line's PENDING bit. Raising a line that is
 *     already pending does nothing more: interrupts are edge events, and
 *     the controller remembers "at least one", not "how many". A device
 *     that needs a count keeps it in its own status register.
 *   - A pending line is delivered only while it is ENABLED (configured on)
 *     and not MASKED (temporarily blocked, e.g. by a critical section).
 *     While blocked it stays pending, and it is delivered as soon as it is
 *     unmasked - an interrupt is never silently lost.
 *   - PENDING is cleared just before the ISR runs, so an edge that arrives
 *     during the ISR re-pends the line and runs the ISR again afterwards.
 *
 * Two delivery modes:
 *   - DETERMINISTIC (default): nothing runs asynchronously. The firmware
 *     calls irq_dispatch() at its "instruction boundaries" - after each
 *     simulated sample tick - and pending ISRs run there, on the firmware
 *     thread, in line-number order. Same seed, same interrupt timing, same
 *     results, at any time_scale including 0.
 *   - THREADED: a dedicated interrupt thread runs ISRs as soon as a line
 *     becomes deliverable, truly concurrently with the firmware thread.
 *     Results are no longer reproducible; it exists so ThreadSanitizer can
 *     check that the ISR / bottom-half split (an SPSC ring between them,
 *     device state behind the bus lock) is actually race-free.
 */
#ifndef FWSIM_HAL_IRQ_H
#define FWSIM_HAL_IRQ_H

#include <pthread.h>
#include <stdint.h>

#define IRQ_LINES 32

typedef void (*irq_handler_t)(void *ctx);

typedef enum { IRQ_MODE_DETERMINISTIC = 0, IRQ_MODE_THREADED = 1 } irq_mode_t;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  cv;            /* threaded mode: work, and quiesce     */
    uint32_t        enabled, masked, pending;
    irq_handler_t   handler[IRQ_LINES];
    void           *ctx[IRQ_LINES];
    uint64_t        raised[IRQ_LINES];     /* irq_raise calls            */
    uint64_t        delivered[IRQ_LINES];  /* ISR invocations            */
    irq_mode_t      mode;
    int             running;       /* an ISR is executing (threaded mode)  */
    int             stop;
    int             thread_started;
    pthread_t       thread;
} irq_ctrl_t;

int  irq_init(irq_ctrl_t *c, irq_mode_t mode);   /* 0 or -1 */
void irq_destroy(irq_ctrl_t *c);                 /* stops the thread */

void irq_set_handler(irq_ctrl_t *c, unsigned line, irq_handler_t fn, void *ctx);
void irq_enable(irq_ctrl_t *c, unsigned line);
void irq_disable(irq_ctrl_t *c, unsigned line);
void irq_mask(irq_ctrl_t *c, unsigned line);
void irq_unmask(irq_ctrl_t *c, unsigned line);

/* Called by device models (any thread). */
void irq_raise(irq_ctrl_t *c, unsigned line);

/* Deterministic mode: run every deliverable ISR now, on the caller's
 * thread. Returns how many ran. No-op (0) in threaded mode. */
unsigned irq_dispatch(irq_ctrl_t *c);

/* Threaded mode: wait until no deliverable line is pending and no ISR is
 * running. Immediate in deterministic mode. */
void irq_quiesce(irq_ctrl_t *c);

int      irq_is_pending(irq_ctrl_t *c, unsigned line);
uint64_t irq_delivered(irq_ctrl_t *c, unsigned line);

#endif
