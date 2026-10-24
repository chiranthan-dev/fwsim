/*
 * irq.c - interrupt controller model. Semantics in irq.h.
 */
#include "hal/irq.h"

#include <string.h>

static uint32_t deliverable(const irq_ctrl_t *c)
{
    return c->pending & c->enabled & ~c->masked;
}

/* Lowest-numbered deliverable line = highest priority. */
static unsigned next_line(uint32_t bits)
{
    return (unsigned)__builtin_ctz(bits);
}

static void *irq_thread(void *arg)
{
    irq_ctrl_t *c = arg;
    pthread_mutex_lock(&c->lock);
    for (;;) {
        uint32_t d = deliverable(c);
        if (c->stop)
            break;
        if (!d) {
            pthread_cond_wait(&c->cv, &c->lock);
            continue;
        }
        unsigned line = next_line(d);
        c->pending &= ~(1u << line);       /* clear BEFORE the ISR runs */
        irq_handler_t fn = c->handler[line];
        void *ctx = c->ctx[line];
        c->running = 1;
        pthread_mutex_unlock(&c->lock);
        if (fn)
            fn(ctx);                        /* never under our lock */
        pthread_mutex_lock(&c->lock);
        c->running = 0;
        c->delivered[line]++;
        pthread_cond_broadcast(&c->cv);     /* irq_quiesce waiters */
    }
    pthread_mutex_unlock(&c->lock);
    return NULL;
}

int irq_init(irq_ctrl_t *c, irq_mode_t mode)
{
    memset(c, 0, sizeof(*c));
    if (pthread_mutex_init(&c->lock, NULL) != 0)
        return -1;
    if (pthread_cond_init(&c->cv, NULL) != 0) {
        pthread_mutex_destroy(&c->lock);
        return -1;
    }
    c->mode = mode;
    if (mode == IRQ_MODE_THREADED) {
        if (pthread_create(&c->thread, NULL, irq_thread, c) != 0) {
            pthread_cond_destroy(&c->cv);
            pthread_mutex_destroy(&c->lock);
            return -1;
        }
        c->thread_started = 1;
    }
    return 0;
}

void irq_destroy(irq_ctrl_t *c)
{
    if (c->thread_started) {
        pthread_mutex_lock(&c->lock);
        c->stop = 1;
        pthread_cond_broadcast(&c->cv);
        pthread_mutex_unlock(&c->lock);
        pthread_join(c->thread, NULL);
        c->thread_started = 0;
    }
    pthread_cond_destroy(&c->cv);
    pthread_mutex_destroy(&c->lock);
}

void irq_set_handler(irq_ctrl_t *c, unsigned line, irq_handler_t fn, void *ctx)
{
    if (line >= IRQ_LINES)
        return;
    pthread_mutex_lock(&c->lock);
    c->handler[line] = fn;
    c->ctx[line] = ctx;
    pthread_mutex_unlock(&c->lock);
}

/* Change one bit of a configuration word; wake the thread in case the
 * change made a pending line deliverable. */
static void update(irq_ctrl_t *c, uint32_t *word, unsigned line, int set)
{
    if (line >= IRQ_LINES)
        return;
    pthread_mutex_lock(&c->lock);
    if (set)
        *word |= 1u << line;
    else
        *word &= ~(1u << line);
    if (c->mode == IRQ_MODE_THREADED && deliverable(c))
        pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->lock);
}

void irq_enable(irq_ctrl_t *c, unsigned line)  { update(c, &c->enabled, line, 1); }
void irq_disable(irq_ctrl_t *c, unsigned line) { update(c, &c->enabled, line, 0); }
void irq_mask(irq_ctrl_t *c, unsigned line)    { update(c, &c->masked, line, 1); }
void irq_unmask(irq_ctrl_t *c, unsigned line)  { update(c, &c->masked, line, 0); }

void irq_raise(irq_ctrl_t *c, unsigned line)
{
    if (line >= IRQ_LINES)
        return;
    pthread_mutex_lock(&c->lock);
    c->pending |= 1u << line;
    c->raised[line]++;
    if (c->mode == IRQ_MODE_THREADED && deliverable(c))
        pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->lock);
}

unsigned irq_dispatch(irq_ctrl_t *c)
{
    if (c->mode != IRQ_MODE_DETERMINISTIC)
        return 0;
    unsigned ran = 0;
    pthread_mutex_lock(&c->lock);
    for (uint32_t d; (d = deliverable(c)) != 0;) {
        unsigned line = next_line(d);
        c->pending &= ~(1u << line);
        irq_handler_t fn = c->handler[line];
        void *ctx = c->ctx[line];
        pthread_mutex_unlock(&c->lock);
        if (fn)
            fn(ctx);
        pthread_mutex_lock(&c->lock);
        c->delivered[line]++;
        ran++;
    }
    pthread_mutex_unlock(&c->lock);
    return ran;
}

void irq_quiesce(irq_ctrl_t *c)
{
    if (c->mode != IRQ_MODE_THREADED)
        return;
    pthread_mutex_lock(&c->lock);
    while (deliverable(c) || c->running)
        pthread_cond_wait(&c->cv, &c->lock);
    pthread_mutex_unlock(&c->lock);
}

int irq_is_pending(irq_ctrl_t *c, unsigned line)
{
    if (line >= IRQ_LINES)
        return 0;
    pthread_mutex_lock(&c->lock);
    int p = (c->pending >> line) & 1u;
    pthread_mutex_unlock(&c->lock);
    return p;
}

uint64_t irq_delivered(irq_ctrl_t *c, unsigned line)
{
    if (line >= IRQ_LINES)
        return 0;
    pthread_mutex_lock(&c->lock);
    uint64_t n = c->delivered[line];
    pthread_mutex_unlock(&c->lock);
    return n;
}
