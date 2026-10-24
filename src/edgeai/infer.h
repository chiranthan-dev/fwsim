/*
 * infer.h - on-drive inference for the vibration classifier: an int8 MLP
 * (features -> 16 -> 4) and a decision tree, both exported by
 * tools/edgeai/export.py into the generated src/edgeai/model_data.h.
 *
 * Rules of the inference path: integer arithmetic only (int8 weights,
 * int32 accumulators, int64 only for requantisation products), no malloc,
 * no floating point, bounded time. `make nofloat` enforces the float rule
 * by compiling features.c and infer.c with -mgeneral-regs-only.
 *
 * ---- int8 MLP --------------------------------------------------------------
 * Every real value r is represented as q * scale (symmetric, zero point 0
 * after the input standardisation):
 *   input   q_in[j] = clamp_int8(round((x[j] - zp[j]) * in_mult[j] / 2^IN_SHIFT))
 *           (folds the training-set standardisation (x - mean) / std and the
 *            input scale s_in into one integer multiply per feature)
 *   layer 1 acc1[h] = b1[h] + sum_j w1[h][j] * q_in[j]          (int32)
 *           q_h[h]  = clamp_int8(round(max(acc1, 0) * m1 / 2^REQ_SHIFT))
 *           with m1 / 2^REQ_SHIFT = s_in * s_w1 / s_h  (one per-layer scale)
 *   layer 2 logit[c] = b2[c] + sum_h w2[c][h] * q_h[h]          (int32)
 * Class = argmax logit. Confidence = softmax probability of that class,
 * computed in fixed point with a 2^-x table: logits are in units of
 * s_out = s_h * s_w2, and logit_k_q16 = s_out * log2(e) * 2^16 turns a
 * logit difference into a base-2 exponent.
 *
 * ---- decision tree ---------------------------------------------------------
 * Nodes test x[feat] <= thr on the integer features themselves (sklearn's
 * float thresholds lie between integer feature values, so floor() makes
 * the integer test exact). Leaves carry the class and the training purity
 * of the leaf as the confidence.
 */
#ifndef FWSIM_EDGEAI_INFER_H
#define FWSIM_EDGEAI_INFER_H

#include <stdint.h>

#include "edgeai/features.h"

#define EDGEAI_HIDDEN     16
#define EDGEAI_IN_SHIFT   24
#define EDGEAI_REQ_SHIFT  24

typedef enum { EDGEAI_MODEL_MLP = 0, EDGEAI_MODEL_TREE = 1 } edgeai_kind_t;

typedef struct {
    int32_t in_zp[EDGEAI_NFEAT];
    int32_t in_mult[EDGEAI_NFEAT];
    int8_t  w1[EDGEAI_HIDDEN][EDGEAI_NFEAT];
    int32_t b1[EDGEAI_HIDDEN];
    int32_t m1;
    int8_t  w2[EDGEAI_NCLASSES][EDGEAI_HIDDEN];
    int32_t b2[EDGEAI_NCLASSES];
    int32_t logit_k_q16;
} edgeai_mlp_t;

typedef struct {
    int8_t   feat;      /* -1 = leaf */
    uint8_t  cls;       /* leaf: class          */
    uint8_t  conf;      /* leaf: purity, percent */
    uint8_t  pad;
    int32_t  thr;
    uint16_t left, right;
} edgeai_node_t;

typedef struct {
    const edgeai_node_t *nodes;
    uint16_t             nnodes;
} edgeai_tree_t;

/* One window size's models, plus which of the two the firmware deploys. */
typedef struct {
    uint16_t           window;
    edgeai_kind_t      selected;
    const edgeai_mlp_t *mlp;
    edgeai_tree_t       tree;
} edgeai_model_t;

typedef struct {
    uint8_t cls;                         /* env_class_t                    */
    uint8_t conf_pct;                    /* 0..100                         */
    int32_t logit[EDGEAI_NCLASSES];      /* MLP only                       */
} edgeai_out_t;

/* The exported model for a window size, NULL if none was exported. */
const edgeai_model_t *edgeai_model_for(unsigned window);
/* The window size the export selected for deployment (0 if no model). */
unsigned edgeai_default_window(void);
/* Model id written by the export (changes whenever the model does). */
uint32_t edgeai_model_id(void);

int  edgeai_infer_mlp(const edgeai_mlp_t *m, const int32_t x[EDGEAI_NFEAT], edgeai_out_t *o);
int  edgeai_infer_tree(const edgeai_tree_t *t, const int32_t x[EDGEAI_NFEAT], edgeai_out_t *o);
/* Run the deployed model of `m`. 0 / -1. */
int  edgeai_classify(const edgeai_model_t *m, const int32_t x[EDGEAI_NFEAT], edgeai_out_t *o);

/* Bytes of model parameters (what would sit in flash). */
unsigned edgeai_mlp_bytes(void);
unsigned edgeai_tree_bytes(const edgeai_tree_t *t);

#endif
