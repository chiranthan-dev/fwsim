/*
 * infer.c - int8 MLP and decision-tree inference. Integer-only; see
 * infer.h for the arithmetic.
 */
#include "edgeai/infer.h"

#include <stddef.h>

#include "edgeai/model_data.h"   /* generated: the exported models */

static int8_t clamp8(int64_t v)
{
    return (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v);
}

/* round(v * mult / 2^shift), rounding half up (floor of v*mult + half). */
static int64_t requant(int64_t v, int64_t mult, int shift)
{
    return (v * mult + ((int64_t)1 << (shift - 1))) >> shift;
}

/* 2^-f for f in [0, 1), Q15, at 1/16 steps; linear in between. */
static const uint16_t exp2_neg_q15[17] = {
    32768, 31379, 30048, 28774, 27554, 26386, 25268, 24196, 23170,
    22188, 21247, 20347, 19484, 18658, 17867, 17109, 16384,
};

/* 2^(-d / 2^16) in Q15 for d >= 0. */
static uint32_t exp2_neg(int64_t d)
{
    int64_t ip = d >> 16;
    if (ip >= 16)
        return 0;
    uint32_t f = (uint32_t)(d & 0xFFFF);            /* fraction, Q16 */
    uint32_t i = f >> 12, r = f & 0xFFF;            /* table index, remainder */
    uint32_t a = exp2_neg_q15[i], b = exp2_neg_q15[i + 1];
    uint32_t v = a - (((a - b) * r) >> 12);
    return v >> ip;
}

int edgeai_infer_mlp(const edgeai_mlp_t *m, const int32_t x[EDGEAI_NFEAT], edgeai_out_t *o)
{
    if (!m)
        return -1;
    int8_t q[EDGEAI_NFEAT], h[EDGEAI_HIDDEN];
    for (int j = 0; j < EDGEAI_NFEAT; j++)
        q[j] = clamp8(requant((int64_t)x[j] - m->in_zp[j], m->in_mult[j], EDGEAI_IN_SHIFT));
    for (int k = 0; k < EDGEAI_HIDDEN; k++) {
        int32_t acc = m->b1[k];
        for (int j = 0; j < EDGEAI_NFEAT; j++)
            acc += (int32_t)m->w1[k][j] * q[j];
        h[k] = acc > 0 ? clamp8(requant(acc, m->m1, EDGEAI_REQ_SHIFT)) : 0;   /* ReLU */
    }
    int best = 0;
    for (int c = 0; c < EDGEAI_NCLASSES; c++) {
        int32_t acc = m->b2[c];
        for (int k = 0; k < EDGEAI_HIDDEN; k++)
            acc += (int32_t)m->w2[c][k] * h[k];
        o->logit[c] = acc;
        if (acc > o->logit[best])
            best = c;
    }
    /* softmax(top) = 1 / sum_c 2^-(log2e * s_out * (l_top - l_c)) */
    uint64_t sum = 0;
    for (int c = 0; c < EDGEAI_NCLASSES; c++) {
        int64_t d = ((int64_t)o->logit[best] - o->logit[c]) * m->logit_k_q16;
        sum += exp2_neg(d);
    }
    o->cls = (uint8_t)best;
    o->conf_pct = (uint8_t)((100u * 32768u + sum / 2) / (sum ? sum : 1));
    return 0;
}

int edgeai_infer_tree(const edgeai_tree_t *t, const int32_t x[EDGEAI_NFEAT], edgeai_out_t *o)
{
    if (!t || !t->nodes || t->nnodes == 0)
        return -1;
    uint16_t i = 0;
    /* Bounded: a path never visits more nodes than the tree has. */
    for (unsigned steps = 0; steps < t->nnodes; steps++) {
        const edgeai_node_t *n = &t->nodes[i];
        if (n->feat < 0) {
            o->cls = n->cls;
            o->conf_pct = n->conf;
            for (int c = 0; c < EDGEAI_NCLASSES; c++)
                o->logit[c] = c == n->cls;
            return 0;
        }
        i = x[n->feat] <= n->thr ? n->left : n->right;
        if (i >= t->nnodes)
            return -1;
    }
    return -1;
}

int edgeai_classify(const edgeai_model_t *m, const int32_t x[EDGEAI_NFEAT], edgeai_out_t *o)
{
    if (!m)
        return -1;
    return m->selected == EDGEAI_MODEL_TREE ? edgeai_infer_tree(&m->tree, x, o)
                                            : edgeai_infer_mlp(m->mlp, x, o);
}

const edgeai_model_t *edgeai_model_for(unsigned window)
{
    for (unsigned i = 0; i < EDGEAI_NMODELS; i++)
        if (edgeai_models[i].window == window)
            return &edgeai_models[i];
    return NULL;
}

unsigned edgeai_default_window(void)
{
    return EDGEAI_DEFAULT_WINDOW;
}

uint32_t edgeai_model_id(void)
{
    return EDGEAI_MODEL_ID;
}

unsigned edgeai_mlp_bytes(void)
{
    return (unsigned)sizeof(edgeai_mlp_t);
}

unsigned edgeai_tree_bytes(const edgeai_tree_t *t)
{
    return (unsigned)(t->nnodes * sizeof(edgeai_node_t));
}
