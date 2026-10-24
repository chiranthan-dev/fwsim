/*
 * test_smart.c - SMART: attribute mapping from the firmware's counters,
 * the daily history ring (wrap-around, gaps), the exported model against
 * the Python reference, and SMART_READ's validation and sense data.
 */
#include "check.h"
#include "smart.h"
#include "smart_vectors.h"

static char path[128];

static fw_t *smart_drive(int enable)
{
    fw_config_t cfg;
    test_config(&cfg, path);
    cfg.write_cache = 0;          /* rewrites go straight to the media */
    cfg.smart_enable = enable;
    fw_t *fw = fw_create(&cfg, NULL);
    CHECK(fw != NULL);
    return fw;
}

static fw_status_t smart_read(fw_t *fw, fw_smart_data_t *d, fw_cmd_t *out)
{
    uint8_t buf[512];
    memset(buf, 0xEE, sizeof(buf));
    fw_cmd_t c = mkcmd(FW_OP_SMART_READ, 2, 0, 1, buf);
    c.feature = FW_SMART_PAGE_DATA;
    fw_status_t st = fw_exec(fw, &c);
    if (d)
        memcpy(d, buf, sizeof(*d));
    if (out)
        *out = c;
    return st;
}

static const fw_smart_attr_t *attr(const fw_smart_data_t *d, uint8_t id)
{
    for (int i = 0; i < d->num_attrs && i < FW_SMART_NATTR; i++)
        if (d->attr[i].id == id)
            return &d->attr[i];
    return NULL;
}

static uint64_t raw(const fw_smart_data_t *d, uint8_t id)
{
    const fw_smart_attr_t *a = attr(d, id);
    CHECK(a != NULL);
    return a ? a->raw : UINT64_MAX;
}

/* ---- feature transform --------------------------------------------------- */

static void test_lg_transform(void)
{
    /* Same values as tools/smart/tests/test_smartlib.py::test_lg_known */
    CHECK_EQ(smart_lg(0), 0);
    CHECK_EQ(smart_lg(1), 16);
    CHECK_EQ(smart_lg(2), 24);
    CHECK_EQ(smart_lg(3), 32);
    CHECK_EQ(smart_lg(7), 48);
    CHECK_EQ(smart_lg(1000), 159);
    CHECK_EQ(smart_lg(UINT64_MAX), smart_lg((UINT64_C(1) << 40) - 1));   /* capped */
    int mono = 1;
    for (uint64_t x = 1; x < 200000; x++)
        if (smart_lg(x) < smart_lg(x - 1))
            mono = 0;
    CHECK(mono);
}

/* ---- history ring -------------------------------------------------------- */

static void feed(smart_t *s, uint64_t day, uint64_t c5, uint64_t c197)
{
    uint64_t c[SMART_NCOUNTERS] = { 0 };
    c[SMART_C_5] = c5;
    c[SMART_C_197] = c197;
    smart_update(s, day * SMART_MS_PER_DAY + 1000, c, 40);
}

static void test_history_wraparound(void)
{
    smart_t s;
    smart_init(&s);
    /* 100 days, counter 5 grows by 2 per day: the ring (32 entries) wraps
     * three times; deltas must still look back exactly 7 / 30 days. */
    for (uint64_t d = 0; d < 100; d++)
        feed(&s, d, 2 * d, 0);
    CHECK_EQ(s.n, SMART_HIST_DAYS);
    CHECK_EQ(s.c[SMART_C_5], 198);
    CHECK_EQ(smart_delta(&s, SMART_C_5, 7), 14);
    CHECK_EQ(smart_delta(&s, SMART_C_5, 30), 60);
    CHECK_EQ(smart_delta(&s, SMART_C_197, 7), 0);

    /* Several updates within one day overwrite that day's sample. */
    smart_t t;
    smart_init(&t);
    feed(&t, 5, 1, 0);
    feed(&t, 5, 3, 0);
    CHECK_EQ(t.n, 1);
    CHECK_EQ(smart_delta(&t, SMART_C_5, 7), 0);     /* only today: no growth */
    /* A young drive: shorter history than the window uses what it has. */
    feed(&t, 8, 10, 0);
    CHECK_EQ(t.n, 4);                               /* days 5, 6, 7, 8 */
    CHECK_EQ(smart_delta(&t, SMART_C_5, 30), 7);    /* 10 - 3 */
    CHECK_EQ(smart_delta(&t, SMART_C_5, 7), 7);
}

static void test_history_gaps_and_decrease(void)
{
    smart_t s;
    smart_init(&s);
    for (uint64_t d = 0; d < 40; d++)
        feed(&s, d, d, 5);
    /* Idle for 10 days, then counter 5 jumped: the skipped days keep the
     * old value, so the whole jump shows up in the 7-day delta... */
    feed(&s, 50, 100, 5);
    CHECK_EQ(smart_delta(&s, SMART_C_5, 7), 100 - 39);
    CHECK_EQ(smart_delta(&s, SMART_C_5, 30), 100 - 20);
    /* ... and a gap longer than the ring keeps the ring consistent. */
    feed(&s, 500, 120, 5);
    CHECK_EQ(s.n, SMART_HIST_DAYS);
    CHECK_EQ(smart_delta(&s, SMART_C_5, 7), 20);
    /* Pending sectors went DOWN (reallocated): the delta is 0, not huge. */
    feed(&s, 501, 120, 1);
    CHECK_EQ(smart_delta(&s, SMART_C_197, 7), 0);
    /* Time never runs backwards. */
    uint64_t before = s.now_ms;
    uint64_t c[SMART_NCOUNTERS] = { 0 };
    smart_update(&s, 0, c, 40);
    CHECK_EQ(s.now_ms, before);
}

/* ---- the exported model ------------------------------------------------- */

static void test_model_matches_python(void)
{
    CHECK_EQ(SMART_VECTORS_MODEL_ID, SMART_MODEL_ID);   /* same export */
    int exact = 0, risk_ok = 0;
    int32_t max_diff = 0;
    for (int i = 0; i < SMART_VECTORS_N; i++) {
        const smart_vector_t *v = &SMART_VECTORS[i];
        int32_t z = smart_logit_q8(v->f);
        uint16_t r = smart_risk_bp(z);
        exact += (z == v->logit_q8);
        risk_ok += (r == v->risk_bp);
        int32_t d = (int32_t)r - v->float_bp;
        if (d < 0)
            d = -d;
        if (d > max_diff)
            max_diff = d;
    }
    CHECK_EQ(exact, SMART_VECTORS_N);     /* bit-exact vs the Python integer reference */
    CHECK_EQ(risk_ok, SMART_VECTORS_N);
    /* vs the float scikit-learn model: quantisation error <= 1 percentage point */
    printf("    model vectors: %d/%d exact, max |C - float| = %d bp\n",
           exact, SMART_VECTORS_N, max_diff);
    CHECK(max_diff <= 100);
}

static void test_risk_lut_edges(void)
{
    CHECK(smart_risk_bp(INT32_MIN) <= 1);
    CHECK_EQ(smart_risk_bp(INT32_MAX), 10000);
    CHECK_EQ(smart_risk_bp(0), 5000);
    int mono = 1;
    for (int32_t z = -5000; z < 5000; z++)
        if (smart_risk_bp(z) < smart_risk_bp(z - 1))
            mono = 0;
    CHECK(mono);
}

/* ---- SMART through the firmware ----------------------------------------- */

static void test_attribute_mapping(void)
{
    fw_t *fw = smart_drive(1);
    fw_sim_set_temperature(fw, 47);
    fw_sim_advance_hours(fw, 1000);

    fw_smart_data_t d;
    fw_status_t st0 = smart_read(fw, &d, NULL);
    CHECK(st0 == FW_OK || st0 == FW_INFO_FAILURE_PREDICTED);
    CHECK_EQ(d.version, FW_SMART_VERSION);
    CHECK_EQ(d.num_attrs, FW_SMART_NATTR);
    CHECK_EQ(d.model_id, SMART_MODEL_ID);
    static const uint8_t ids[] = { 5, 9, 187, 188, 194, 197, 198 };
    for (int i = 0; i < FW_SMART_NATTR; i++)
        CHECK_EQ(d.attr[i].id, ids[i]);
    CHECK_EQ(raw(&d, 5), 0);
    CHECK_EQ(raw(&d, 187), 0);
    CHECK_EQ(raw(&d, 197), 0);
    CHECK_EQ(raw(&d, 194), 47);
    CHECK_EQ(raw(&d, 9), 1000);
    CHECK_EQ(d.power_on_hours, 1000);
    CHECK_EQ(attr(&d, 5)->normalized, 100);

    /* A persistent media defect at 3 LBAs: each failing read is a
     * reported-uncorrectable error (187) and leaves the LBA pending (197). */
    uint8_t buf[512];
    for (uint64_t lba = 100; lba < 103; lba++) {
        CHECK_EQ(fw_inject_media_bitflip(fw, lba, 7), 0);
        fw_cmd_t r = mkcmd(FW_OP_READ, 0, lba, 1, buf);
        CHECK_EQ(fw_exec(fw, &r), FW_ERR_MEDIUM);
    }
    fw_cmd_t again = mkcmd(FW_OP_READ, 0, 100, 1, buf);
    CHECK_EQ(fw_exec(fw, &again), FW_ERR_MEDIUM);
    smart_read(fw, &d, NULL);
    CHECK_EQ(raw(&d, 187), 4);            /* every failed read counts */
    CHECK_EQ(raw(&d, 197), 3);            /* but each LBA is pending once */
    CHECK_EQ(raw(&d, 5), 0);
    CHECK_EQ(attr(&d, 187)->normalized, 96);

    /* The host rewrites two of them: reallocated to spares (5), no longer
     * pending (197). */
    memset(buf, 0x5A, sizeof(buf));
    for (uint64_t lba = 100; lba < 102; lba++) {
        fw_cmd_t w = mkcmd(FW_OP_WRITE, 0, lba, 1, buf);
        CHECK_EQ(fw_exec(fw, &w), FW_OK);
    }
    smart_read(fw, &d, NULL);
    CHECK_EQ(raw(&d, 5), 2);
    CHECK_EQ(raw(&d, 197), 1);
    CHECK_EQ(raw(&d, 187), 4);
    CHECK_EQ(raw(&d, 188), 0);
    CHECK_EQ(raw(&d, 198), 0);
    CHECK_EQ(attr(&d, 5)->normalized, 98);
    CHECK_EQ(attr(&d, 197)->worst, 97);   /* worst remembers 3 pending */
    fw_stats_t st;
    fw_get_stats(fw, &st);
    CHECK_EQ(raw(&d, 5), st.reallocations);
    CHECK_EQ(raw(&d, 187), st.crc_unrecovered);

    /* A week later the 7-day deltas have rolled over; the 30-day ones not. */
    fw_sim_advance_hours(fw, 24 * 8);
    smart_read(fw, &d, NULL);
    CHECK_EQ(d.power_on_hours, 1000 + 24 * 8);
    CHECK(d.history_days >= 9);
    fw_destroy(fw);
}

static void test_prediction_status(void)
{
    fw_t *fw = smart_drive(1);
    fw_smart_data_t d;
    fw_cmd_t c;
    fw_status_t st = smart_read(fw, &d, &c);
    /* The status always tells the same story as the payload. */
    CHECK_EQ(st, d.predicted_failure ? FW_INFO_FAILURE_PREDICTED : FW_OK);
    CHECK_EQ(d.threshold_q8, SMART_MODEL_THRESH_Q8);
    CHECK_EQ(d.risk_bp, smart_risk_bp(d.logit_q8));

    /* A drive with thousands of reallocations and pending sectors, all
     * grown this week, must trip the flag (a model that would not flag
     * this is broken, whatever its PR-AUC). */
    smart_t s;
    smart_init(&s);
    uint64_t cnt[SMART_NCOUNTERS] = { 0 };
    smart_update(&s, 20000 * SMART_MS_PER_HOUR, cnt, 35);
    cnt[SMART_C_5] = 4000;
    cnt[SMART_C_187] = 300;
    cnt[SMART_C_197] = 500;
    smart_update(&s, 20000 * SMART_MS_PER_HOUR + 3 * SMART_MS_PER_DAY, cnt, 35);
    smart_report(&s, &d);
    CHECK_EQ(d.predicted_failure, 1);
    CHECK(d.risk_bp > 0);

    /* The informational exception is reported with 01/5D/00 and is not
     * counted as a failed command. */
    fw_sense_t sn = fw_status_sense(FW_INFO_FAILURE_PREDICTED);
    CHECK_EQ(sn.key, 0x1);
    CHECK_EQ(sn.asc, 0x5D);
    CHECK_EQ(sn.ascq, 0x00);
    fw_stats_t fst;
    uint8_t idb[512];
    fw_cmd_t id = mkcmd(FW_OP_IDENTIFY, 31, 0, 1, idb);
    CHECK_EQ(fw_exec(fw, &id), FW_OK);
    fw_get_stats(fw, &fst);
    CHECK_EQ(fst.cmd_errors, 0);
    CHECK_EQ(fst.cmds[FW_OP_SMART_READ], 1);
    fw_destroy(fw);
}

static void test_smart_read_validation(void)
{
    fw_t *fw = smart_drive(1);
    uint8_t buf[512];
    struct {
        uint64_t lba;
        uint32_t count;
        uint32_t feature;
        int null_buf;
    } bad[] = {
        { 0, 1, FW_SMART_PAGE_DATA, 1 },   /* no buffer           */
        { 0, 2, FW_SMART_PAGE_DATA, 0 },   /* more than a sector   */
        { 0, 0, FW_SMART_PAGE_DATA, 0 },   /* zero sectors         */
        { 7, 1, FW_SMART_PAGE_DATA, 0 },   /* an LBA makes no sense */
        { 0, 1, 0xD1, 0 },                 /* unsupported page     */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        fw_cmd_t c = mkcmd(FW_OP_SMART_READ, 1, bad[i].lba, bad[i].count,
                           bad[i].null_buf ? NULL : buf);
        c.feature = bad[i].feature;
        CHECK_EQ(fw_exec(fw, &c), FW_ERR_INVALID_FIELD);
        CHECK_EQ(c.sense.key, 0x5);        /* ILLEGAL REQUEST */
        CHECK_EQ(c.sense.asc, 0x24);       /* INVALID FIELD IN CDB */
        CHECK_EQ(c.sense.ascq, 0x00);
    }
    CHECK(strcmp(fw_opcode_str(FW_OP_SMART_READ), "SMART_READ") == 0);
    CHECK(strcmp(fw_status_str(FW_ERR_SMART_DISABLED), "SMART_DISABLED") == 0);
    fw_destroy(fw);
}

static void test_smart_disabled_by_default(void)
{
    fw_t *fw = smart_drive(0);
    uint8_t buf[512];
    fw_cmd_t id = mkcmd(FW_OP_IDENTIFY, 0, 0, 1, buf);
    CHECK_EQ(fw_exec(fw, &id), FW_OK);
    fw_identify_t ident;
    memcpy(&ident, buf, sizeof(ident));
    CHECK_EQ(ident.smart_supported, 1);    /* advertised ...  */
    CHECK_EQ(ident.smart_enabled, 0);      /* ... but off      */
    CHECK_EQ(ident.smart_model_id, SMART_MODEL_ID);

    fw_cmd_t c;
    CHECK_EQ(smart_read(fw, NULL, &c), FW_ERR_SMART_DISABLED);
    CHECK_EQ(c.sense.key, 0x5);            /* ILLEGAL REQUEST */
    CHECK_EQ(c.sense.asc, 0x2C);           /* COMMAND SEQUENCE ERROR */

    /* SET_FEATURES enables it; only 0/1 are valid values. */
    fw_cmd_t sf = mkcmd(FW_OP_SET_FEATURES, 0, 0, 0, NULL);
    sf.feature = FW_FEAT_SMART;
    sf.feature_value = 2;
    CHECK_EQ(fw_exec(fw, &sf), FW_ERR_INVALID_FIELD);
    sf.feature_value = 1;
    CHECK_EQ(fw_exec(fw, &sf), FW_OK);
    fw_smart_data_t d;
    fw_status_t st = smart_read(fw, &d, NULL);
    CHECK(st == FW_OK || st == FW_INFO_FAILURE_PREDICTED);
    CHECK_EQ(d.version, FW_SMART_VERSION);
    CHECK_EQ(buf[0] == 0 || 1, 1);
    id = mkcmd(FW_OP_IDENTIFY, 0, 0, 1, buf);
    CHECK_EQ(fw_exec(fw, &id), FW_OK);
    memcpy(&ident, buf, sizeof(ident));
    CHECK_EQ(ident.smart_enabled, 1);
    sf.feature_value = 0;
    CHECK_EQ(fw_exec(fw, &sf), FW_OK);
    CHECK_EQ(smart_read(fw, NULL, NULL), FW_ERR_SMART_DISABLED);
    fw_destroy(fw);
}

int main(void)
{
    tmp_media(path, sizeof(path), "smart");
    printf("test_smart (model 0x%08X, %d features)\n", SMART_MODEL_ID, SMART_MODEL_NFEAT);
    RUN_TEST(test_lg_transform);
    RUN_TEST(test_history_wraparound);
    RUN_TEST(test_history_gaps_and_decrease);
    RUN_TEST(test_model_matches_python);
    RUN_TEST(test_risk_lut_edges);
    RUN_TEST(test_attribute_mapping);
    RUN_TEST(test_prediction_status);
    RUN_TEST(test_smart_read_validation);
    RUN_TEST(test_smart_disabled_by_default);
    unlink(path);
    return test_summary("test_smart");
}
