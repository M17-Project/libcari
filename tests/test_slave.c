/*
 * libcari slave + master tests, in one process over loopback TCP.
 *
 * A mock device (two subdevices: 0 = full-duplex I/Q transceiver,
 * 1 = receive-only) runs the slave in a thread; the test acts as master.
 */
#include "t.h"
#include <cari/cari_config.h>
#include <cari/cari_flac.h>
#include <cari/cari_log.h>
#include <cari/cari_master.h>
#include <cari/cari_slave.h>

#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <zmq.h>

static int base_port;
/* mock state shared between the test, the slave thread and pump threads */
#define LD(x)    __atomic_load_n(&(x), __ATOMIC_SEQ_CST)
#define ST(x, v) __atomic_store_n(&(x), (v), __ATOMIC_SEQ_CST)
#define ADD(x, v) __atomic_fetch_add(&(x), (v), __ATOMIC_SEQ_CST)
#define P(n) ((uint16_t)(base_port + (n)))

/* ---------------------------------------------------------------- mock */

typedef struct {
    cari_slave_t *s;
    pthread_t     th[2][2];             /* [sub][dir] */
    int           run[2][2];
    uint64_t      ul_samples[2];
    int           underrun_after_ms;    /* UL starvation that trips */
    int           playout_ms;           /* play-out time after the end marker */
    int           ends_seen;
    int           reread_bad;
    uint8_t       reg10;
    int           dev_resets, sub_resets;
    uint32_t      hw_flags;
} mock_t;

typedef struct { mock_t *m; uint8_t sub; } arg_t;
static arg_t args[2][2];

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void *dl_pump(void *p)
{
    arg_t *a = p;
    int16_t buf[2 * 250];
    for (int i = 0; i < 500; i++)
        buf[i] = (int16_t)(i * 37);
    while (LD(a->m->run[a->sub][CARI_DIR_DL])) {
        cari_slave_dl_write(a->m->s, a->sub, buf, 250);
        sleep_ms(5);
    }
    return NULL;
}

static void *ul_pump(void *p)
{
    arg_t *a = p;
    int64_t last = mono_ms();
    bool got_any = false;
    while (LD(a->m->run[a->sub][CARI_DIR_UL])) {
        const void *d;
        size_t n;
        int r = cari_slave_ul_read(a->m->s, a->sub, &d, &n, 10);
        if (r == 1) {
            ADD(a->m->ul_samples[a->sub], n);
            last = mono_ms();
            got_any = true;
        } else if (r == CARI_UL_END) {
            /* play out what is buffered, ramp down, unkey, report */
            ADD(a->m->ends_seen, 1);
            int64_t end = mono_ms() + LD(a->m->playout_ms);
            while (LD(a->m->run[a->sub][CARI_DIR_UL]) && mono_ms() < end)
                sleep_ms(2);
            /* the socket is not read any more after the marker (checked by
             * the main thread: the CHECK counters are not thread-safe) */
            if (cari_slave_ul_read(a->m->s, a->sub, &d, &n, 0) != CARI_UL_END)
                ADD(a->m->reread_bad, 1);
            if (LD(a->m->run[a->sub][CARI_DIR_UL]))
                cari_slave_ul_finished(a->m->s, a->sub);
            break;
        } else if (got_any && mono_ms() - last > LD(a->m->underrun_after_ms)) {
            /* (a real device ramps down and unkeys here) */
            cari_slave_ul_underrun(a->m->s, a->sub);
            break;
        }
    }
    return NULL;
}

static int m_stream_start(void *u, uint8_t sub, cari_dir_t dir)
{
    mock_t *m = u;
    args[sub][dir].m = m;
    args[sub][dir].sub = sub;
    ST(m->run[sub][dir], 1);
    pthread_create(&m->th[sub][dir], NULL, dir == CARI_DIR_DL ? dl_pump : ul_pump, &args[sub][dir]);
    return CARI_OK;
}

static void m_stream_stop(void *u, uint8_t sub, cari_dir_t dir)
{
    mock_t *m = u;
    ST(m->run[sub][dir], 0);
    pthread_join(m->th[sub][dir], NULL);
}

static int m_set_param(void *u, uint8_t sub, uint8_t p, cari_value_t *v)
{
    (void)u; (void)sub;
    if (p == CARI_P_TX_POWER)
        v->v.f32 = floorf(v->v.f32);    /* quantize to 1 dB steps */
    if (p == CARI_P_FREQ_CORR && fabsf(v->v.f32) > 100.0f)
        return CARI_S_OUT_OF_RANGE;
    return CARI_OK;
}

static int m_reg_read(void *u, uint8_t reg, uint8_t *val)
{
    mock_t *m = u;
    if (reg == 0x10) { *val = m->reg10; return CARI_OK; }
    if (reg == 0x11) { *val = 0xA5; return CARI_OK; }
    return CARI_S_INVALID_ADDR;
}

static int m_reg_write(void *u, uint8_t reg, uint8_t val)
{
    mock_t *m = u;
    if (reg == 0x10) { m->reg10 = val; return CARI_OK; }
    if (reg == 0x11) return CARI_S_READ_ONLY;
    return CARI_S_INVALID_ADDR;
}

static uint32_t m_flags(void *u) { return LD(((mock_t *)u)->hw_flags); }
static void m_dev_reset(void *u) { ADD(((mock_t *)u)->dev_resets, 1); }
static int m_sub_reset(void *u, uint8_t sub) { (void)sub; ADD(((mock_t *)u)->sub_resets, 1); return CARI_OK; }

static int m_spvn(void *u, uint8_t q, uint8_t sub, float *v)
{
    (void)u;
    if (q == CARI_Q_TEMPERATURE) { *v = 42.0f; return CARI_OK; }
    if (q == CARI_Q_RETURN_LOSS && sub == 0) { *v = 19.5f; return CARI_OK; }
    return CARI_S_UNSUPPORTED;
}

static const cari_slave_ops_t ops = {
    .set_param = m_set_param, .stream_start = m_stream_start, .stream_stop = m_stream_stop,
    .sub_reset = m_sub_reset, .dev_reset = m_dev_reset, .reg_read = m_reg_read,
    .reg_write = m_reg_write, .error_flags = m_flags, .spvn_read = m_spvn,
};

static mock_t mock;
static int slave_stop;
static pthread_t slave_th;
static uint8_t caps0[256], caps1[256], devcaps[16];

static void *slave_thread(void *p)
{
    cari_slave_run(p, &slave_stop);
    return NULL;
}

static cari_slave_t *make_slave(uint16_t ctrl_port, bool spvn)
{
    static cari_subdev_desc_t subs[2];
    static char ep[64];
    static const uint8_t quantities[] = { CARI_Q_TEMPERATURE, CARI_Q_RETURN_LOSS };
    cari_tlv_writer_t w;

    cari_tlv_init(&w, caps0, sizeof caps0);
    cari_tlv_flag(&w, CARI_SCAP_RESET);
    cari_tlv_flag(&w, CARI_SCAP_IQ);
    cari_tlv_flag(&w, CARI_SCAP_RX);
    cari_tlv_flag(&w, CARI_SCAP_TX);
    cari_tlv_flag(&w, CARI_SCAP_FULL_DUPLEX);
    cari_tlv_flag(&w, CARI_SCAP_FMT_CS16);
    cari_tlv_flag(&w, CARI_SCAP_FMT_S16);
    cari_tlv_range_u64(&w, CARI_SCAP_RX_FREQ, 400000000, 510000000);
    cari_tlv_range_u64(&w, CARI_SCAP_TX_FREQ, 400000000, 510000000);
    cari_tlv_range_f32(&w, CARI_SCAP_LNA_GAIN, -48, 0);
    cari_tlv_range_f32(&w, CARI_SCAP_TX_POWER, -34, 5);
    cari_tlv_value_f32(&w, CARI_SCAP_SAMPLE_RATE, 125000);
    cari_tlv_value_f32(&w, CARI_SCAP_SAMPLE_RATE, 250000);
    subs[0].caps = caps0;
    subs[0].caps_len = w.len;

    cari_tlv_init(&w, caps1, sizeof caps1);
    cari_tlv_flag(&w, CARI_SCAP_IQ);
    cari_tlv_flag(&w, CARI_SCAP_RX);
    cari_tlv_flag(&w, CARI_SCAP_FMT_CS16);
    cari_tlv_range_u64(&w, CARI_SCAP_RX_FREQ, 144000000, 146000000);
    cari_tlv_value_f32(&w, CARI_SCAP_SAMPLE_RATE, 48000);
    subs[1].caps = caps1;
    subs[1].caps_len = w.len;

    cari_tlv_init(&w, devcaps, sizeof devcaps);
#ifdef CARI_HAVE_FLAC
    cari_tlv_flag(&w, CARI_DCAP_COMPRESSION);
#endif
    if (spvn)
        cari_tlv_flag(&w, CARI_DCAP_SUPERVISION);

    snprintf(ep, sizeof ep, "tcp://127.0.0.1:%u", ctrl_port);
    cari_slave_desc_t d = {
        .ident = "libcari test device \xC5\x81\xC3\xB3" "d\xC5\xBA",      /* UTF-8 */
        .dev_caps = devcaps, .dev_caps_len = w.len,
        .quantities = quantities, .nquantities = 2,
        .nsubdevs = 2, .subdevs = subs,
        .ctrl_endpoint = ep, .bind_host = "127.0.0.1",
    };
    cari_slave_t *s = cari_slave_new(&d, &ops, &mock);
    if (!s)
        return NULL;
    cari_slave_param_init(s, 0, CARI_P_RX_FREQ, cari_value_u64(435000000));
    cari_slave_param_init(s, 0, CARI_P_TX_FREQ, cari_value_u64(435000000));
    cari_slave_param_init(s, 0, CARI_P_LNA_GAIN, cari_value_f32(0));
    cari_slave_param_init(s, 0, CARI_P_TX_POWER, cari_value_f32(-10));
    cari_slave_param_init(s, 0, CARI_P_SAMPLE_RATE, cari_value_f32(125000));
    cari_slave_param_init(s, 0, CARI_P_FREQ_CORR, cari_value_f32(0));
    cari_slave_param_init(s, 0, CARI_P_FORMAT, cari_value_u8(CARI_FMT_CS16));
    cari_slave_param_init(s, 0, CARI_P_MODE, cari_value_u8(CARI_MODE_IQ));
    cari_slave_param_init(s, 0, CARI_P_COMPRESSION, cari_value_bool(false));
    cari_slave_param_init(s, 1, CARI_P_RX_FREQ, cari_value_u64(145000000));
    cari_slave_param_init(s, 1, CARI_P_SAMPLE_RATE, cari_value_f32(48000));
    cari_slave_param_init(s, 1, CARI_P_FORMAT, cari_value_u8(CARI_FMT_CS16));
    return s;
}

/* ---------------------------------------------------------------- helpers */

static cari_master_t *M;
static void *raw_req;
static char ctrl_ep[48];

/* Send raw bytes on a fresh REQ socket, return reply length. */
static int raw(const void *msg, size_t n, uint8_t *rep, size_t cap, bool split)
{
    if (split) {
        zmq_send(raw_req, msg, 1, ZMQ_SNDMORE);
        zmq_send(raw_req, (const uint8_t *)msg + 1, n - 1, 0);
    } else {
        zmq_send(raw_req, msg, n, 0);
    }
    zmq_pollitem_t it = { raw_req, 0, ZMQ_POLLIN, 0 };
    if (zmq_poll(&it, 1, 2000) <= 0)
        return -1;
    return zmq_recv(raw_req, rep, cap, 0);
}

static int setf(uint8_t sub, uint8_t p, float v)
{
    cari_value_t x = cari_value_f32(v);
    return cari_m_set_param(M, sub, p, &x);
}
static int setu(uint8_t sub, uint8_t p, uint64_t v)
{
    cari_value_t x = cari_value_u64(v);
    return cari_m_set_param(M, sub, p, &x);
}
static int set8(uint8_t sub, uint8_t p, uint8_t v)
{
    const cari_param_info_t *pi = cari_param_info(p);
    cari_value_t x = { .type = pi->type };
    x.v.u8 = v;
    return cari_m_set_param(M, sub, p, &x);
}
static uint8_t tx_state(uint8_t sub)
{
    cari_value_t v = { 0 };
    CHECK_EQ(cari_m_get_param(M, sub, CARI_P_TX_STATE, &v), CARI_OK);
    return v.v.u8;
}

/* ---------------------------------------------------------------- tests */

static void test_basic(void)
{
    uint32_t flags = 0xFFFFFFFF;
    uint8_t v;
    char ident[128];
    CHECK_EQ(cari_m_ping(M, &flags), CARI_OK);
    CHECK_EQ(flags, 0);
    ST(mock.hw_flags, CARI_EF_PLL_LOCK | CARI_EF_UL_TRIP);    /* device can't fake the trip bit */
    CHECK_EQ(cari_m_ping(M, &flags), CARI_OK);
    CHECK_EQ(flags, CARI_EF_PLL_LOCK);
    ST(mock.hw_flags, 0);
    CHECK_EQ(cari_m_get_ident(M, ident, sizeof ident), CARI_OK);
    CHECK(!strcmp(ident, "libcari test device \xC5\x81\xC3\xB3" "d\xC5\xBA"));
    CHECK_EQ(cari_m_get_reg(M, 0, &v), CARI_OK);
    CHECK_EQ(v, 0x21);
    CHECK_EQ(cari_m_get_reg(M, 1, &v), CARI_OK);
    CHECK_EQ(v, 2);
    CHECK_EQ(cari_m_set_reg(M, 1, 3), CARI_S_READ_ONLY);
    CHECK_EQ(cari_m_set_reg(M, 0x10, 0x5A), CARI_OK);
    CHECK_EQ(cari_m_get_reg(M, 0x10, &v), CARI_OK);
    CHECK_EQ(v, 0x5A);
    CHECK_EQ(cari_m_set_reg(M, 0x11, 1), CARI_S_READ_ONLY);
    CHECK_EQ(cari_m_get_reg(M, 0x11, &v), CARI_OK);
    CHECK_EQ(cari_m_get_reg(M, 0x50, &v), CARI_S_INVALID_ADDR);
    CHECK_EQ(cari_m_set_reg(M, 0x50, 1), CARI_S_INVALID_ADDR);

    cari_caps_t c;
    CHECK_EQ(cari_m_get_dev_caps(M, &c), CARI_OK);
    CHECK(cari_caps_has_flag(&c, CARI_DCAP_SUPERVISION));
    uint8_t q[8];
    size_t nq = 0;
    CHECK_EQ(cari_m_get_spvn_list(M, q, sizeof q, &nq), CARI_OK);
    CHECK(nq == 2 && q[0] == CARI_Q_TEMPERATURE && q[1] == CARI_Q_RETURN_LOSS);
    CHECK_EQ(cari_m_get_sub_caps(M, 0, &c), CARI_OK);
    CHECK(cari_caps_has_flag(&c, CARI_SCAP_TX) && cari_caps_has_flag(&c, CARI_SCAP_FULL_DUPLEX));
    CHECK_EQ(c.nranges, 6);
    CHECK_EQ(cari_m_get_sub_caps(M, 1, &c), CARI_OK);
    CHECK(!cari_caps_has_flag(&c, CARI_SCAP_TX));
    CHECK_EQ(cari_m_get_sub_caps(M, 2, &c), CARI_S_INVALID_ADDR);
    CHECK_EQ(cari_m_get_sub_caps(M, 0xFF, &c), CARI_S_INVALID_ADDR);
}

static void test_reply_rules(void)
{
    uint8_t rep[64];
    int n;
    /* empty message -> CID 0xFF, status 1, 4 bytes */
    n = raw("", 0, rep, sizeof rep, false);
    CHECK_EQ(n, 4);
    CHECK(rep[0] == 0xFF && rep[1] == 4 && rep[2] == 0 && rep[3] == CARI_S_MALFORMED);
    n = raw("\x80\x02", 2, rep, sizeof rep, false);
    CHECK(n == 4 && rep[0] == 0x80 && rep[3] == CARI_S_MALFORMED);
    n = raw("\x00\x05\x00", 3, rep, sizeof rep, false);                    /* count mismatch */
    CHECK(n == 4 && rep[0] == 0x00 && rep[3] == CARI_S_MALFORMED);
    n = raw("\x81\x05\x00\x01\x02", 5, rep, sizeof rep, false);            /* len wrong for CID */
    CHECK(n == 4 && rep[0] == 0x81 && rep[3] == CARI_S_MALFORMED);
    n = raw("\x10\x03\x00", 3, rep, sizeof rep, false);                    /* unknown CID */
    CHECK(n == 4 && rep[0] == 0x10 && rep[3] == CARI_S_UNSUPPORTED);
    n = raw("\xF0\x04\x00\x00", 4, rep, sizeof rep, false);
    CHECK(n == 4 && rep[0] == 0xF0 && rep[3] == CARI_S_UNSUPPORTED);
    n = raw("\x02\x06\x00\x00\x20\x01", 6, rep, sizeof rep, false);        /* reserved param */
    CHECK(n == 4 && rep[3] == CARI_S_UNSUPPORTED);
    n = raw("\x83\x05\x00\x00\x20", 5, rep, sizeof rep, false);            /* get reserved param */
    CHECK(n == 4 && rep[3] == CARI_S_UNSUPPORTED);
    n = raw("\x00\x03\x00", 3, rep, sizeof rep, true);                     /* multipart */
    CHECK(n == 4 && rep[0] == 0x00 && rep[3] == CARI_S_MALFORMED);
    n = raw("\x00\x03\x00", 3, rep, sizeof rep, false);                    /* still alive */
    CHECK(n == 8 && rep[1] == 8 && rep[3] == CARI_OK);
    /* exec unknown action, invalid subdevice */
    CHECK_EQ(cari_m_exec(M, 0, 0x06), CARI_S_UNSUPPORTED);
    CHECK_EQ(cari_m_exec(M, 7, CARI_ACT_RX_START), CARI_S_INVALID_ADDR);
    CHECK_EQ(cari_m_bind_dl(M, 7, P(1)), CARI_S_INVALID_ADDR);
    CHECK_EQ(setu(9, CARI_P_RX_FREQ, 435000000), CARI_S_INVALID_ADDR);
}

static void test_params(void)
{
    cari_value_t v;
    CHECK_EQ(setu(0, CARI_P_RX_FREQ, 433475000), CARI_OK);
    CHECK_EQ(cari_m_get_param(M, 0, CARI_P_RX_FREQ, &v), CARI_OK);
    CHECK_EQ(v.v.u64, 433475000);
    CHECK_EQ(setu(0, CARI_P_RX_FREQ, 399999999), CARI_S_OUT_OF_RANGE);
    CHECK_EQ(setu(1, CARI_P_RX_FREQ, 433000000), CARI_S_OUT_OF_RANGE);   /* sub1 is 2 m */
    CHECK_EQ(setu(1, CARI_P_TX_FREQ, 145000000), CARI_S_UNSUPPORTED);    /* RX-only */
    CHECK_EQ(setf(0, CARI_P_TX_POWER, 3.7f), CARI_OK);
    CHECK_EQ(cari_m_get_param(M, 0, CARI_P_TX_POWER, &v), CARI_OK);
    CHECK(v.v.f32 == 3.0f);                                               /* quantized */
    CHECK_EQ(setf(0, CARI_P_TX_POWER, 6), CARI_S_OUT_OF_RANGE);
    CHECK_EQ(setf(0, CARI_P_LNA_GAIN, NAN), CARI_S_OUT_OF_RANGE);
    CHECK_EQ(setf(0, CARI_P_SAMPLE_RATE, 250000), CARI_OK);
    CHECK_EQ(setf(0, CARI_P_SAMPLE_RATE, 200000), CARI_S_OUT_OF_RANGE);  /* discrete */
    CHECK_EQ(setf(0, CARI_P_FREQ_CORR, 12.5f), CARI_OK);
    CHECK_EQ(setf(0, CARI_P_FREQ_CORR, 150), CARI_S_OUT_OF_RANGE);       /* device decides */
    CHECK_EQ(set8(0, CARI_P_AGC, 1), CARI_S_UNSUPPORTED);
    CHECK_EQ(setf(0, CARI_P_CH_WIDTH, 12500), CARI_S_UNSUPPORTED);       /* not declared */
    CHECK_EQ(set8(0, CARI_P_FORMAT, CARI_FMT_CF32), CARI_S_OUT_OF_RANGE);
    CHECK_EQ(set8(0, CARI_P_MODE, CARI_MODE_FM), CARI_S_OUT_OF_RANGE);
    CHECK_EQ(set8(0, CARI_P_TX_STATE, 0), CARI_S_READ_ONLY);
    CHECK_EQ(set8(1, CARI_P_TX_STATE, 0), CARI_S_UNSUPPORTED);
    CHECK_EQ(cari_m_get_param(M, 1, CARI_P_TX_STATE, &v), CARI_S_UNSUPPORTED);
    CHECK_EQ(cari_m_get_param(M, 1, CARI_P_TX_POWER, &v), CARI_S_UNSUPPORTED);
#ifdef CARI_HAVE_FLAC
    CHECK_EQ(set8(0, CARI_P_COMPRESSION, 2), CARI_S_OUT_OF_RANGE);       /* boolean */
    CHECK_EQ(set8(0, CARI_P_COMPRESSION, 0), CARI_OK);
#endif
    CHECK_EQ(tx_state(0), CARI_TX_IDLE);
    CHECK_EQ(cari_m_exec(M, 1, CARI_ACT_RESET), CARI_S_UNSUPPORTED);     /* no cap 0x00 */
    CHECK_EQ(cari_m_exec(M, 1, CARI_ACT_TX_START), CARI_S_UNSUPPORTED);
    CHECK_EQ(cari_m_exec(M, 1, CARI_ACT_CLEAR_TRIP), CARI_S_UNSUPPORTED);
    CHECK_EQ(cari_m_bind_ul(M, 1, P(9)), CARI_S_UNSUPPORTED);
}

static int recv_msgs(void *sub, int want, int timeout_ms, size_t *bytes, uint8_t *keep, size_t keepcap,
                     size_t *keeplen)
{
    int got = 0;
    int64_t end = mono_ms() + timeout_ms;
    uint8_t b[65536];
    while (got < want && mono_ms() < end) {
        zmq_pollitem_t it = { sub, 0, ZMQ_POLLIN, 0 };
        if (zmq_poll(&it, 1, 50) <= 0)
            continue;
        int n = zmq_recv(sub, b, sizeof b, 0);
        if (n <= 0)
            continue;
        got++;
        if (bytes)
            *bytes += (size_t)n;
        if (keep && *keeplen + (size_t)n <= keepcap) {
            memcpy(keep + *keeplen, b, (size_t)n);
            *keeplen += (size_t)n;
        }
        if (!keep && n % 4)
            return -1;      /* raw CS16 messages carry whole samples */
    }
    return got;
}

static void test_rx(void)
{
    CHECK_EQ(cari_m_exec(M, 0, CARI_ACT_RX_START), CARI_S_INVALID_STATE);     /* not bound */
    CHECK_EQ(cari_m_bind_dl(M, 0, P(1)), CARI_OK);
    CHECK_EQ(cari_m_bind_dl(M, 0, P(1)), CARI_OK);                           /* idempotent */
    CHECK_EQ(cari_m_bind_dl(M, 1, P(1)), CARI_S_BIND_FAILED);                /* port in use */

    /* combination check: I/Q needs a complex format */
    CHECK_EQ(set8(0, CARI_P_FORMAT, CARI_FMT_S16), CARI_OK);
    CHECK_EQ(cari_m_exec(M, 0, CARI_ACT_RX_START), CARI_S_INVALID_STATE);
    CHECK_EQ(set8(0, CARI_P_FORMAT, CARI_FMT_CS16), CARI_OK);

    void *sub = cari_master_connect_dl(M, P(1), 100);
    sleep_ms(100);
    CHECK_EQ(cari_m_exec(M, 0, CARI_ACT_RX_START), CARI_OK);
    CHECK_EQ(cari_m_exec(M, 0, CARI_ACT_RX_START), CARI_OK);                 /* running: not an error */
    size_t bytes = 0;
    CHECK_EQ(recv_msgs(sub, 5, 2000, &bytes, NULL, 0, NULL), 5);
    CHECK_EQ(bytes, 5 * 250 * 4);

    /* stream locks */
    CHECK_EQ(setf(0, CARI_P_SAMPLE_RATE, 125000), CARI_S_INVALID_STATE);
    CHECK_EQ(set8(0, CARI_P_FORMAT, CARI_FMT_CS16), CARI_S_INVALID_STATE);
    CHECK_EQ(set8(0, CARI_P_MODE, CARI_MODE_IQ), CARI_S_INVALID_STATE);
    CHECK_EQ(setu(0, CARI_P_RX_FREQ, 434000000), CARI_OK);                   /* allowed */
    CHECK_EQ(setf(0, CARI_P_LNA_GAIN, -12), CARI_OK);
    CHECK_EQ(cari_m_bind_dl(M, 0, P(2)), CARI_S_INVALID_STATE);
    CHECK_EQ(cari_m_bind_dl(M, 0, 0), CARI_S_INVALID_STATE);
    /* sub 1 is independent */
    CHECK_EQ(cari_m_bind_dl(M, 1, P(3)), CARI_OK);
    CHECK_EQ(cari_m_exec(M, 1, CARI_ACT_RX_START), CARI_OK);
    CHECK_EQ(cari_m_exec(M, 1, CARI_ACT_RX_STOP), CARI_OK);
    CHECK_EQ(cari_m_bind_dl(M, 1, 0), CARI_OK);

    CHECK_EQ(cari_m_exec(M, 0, CARI_ACT_RX_STOP), CARI_OK);
    CHECK_EQ(cari_m_exec(M, 0, CARI_ACT_RX_STOP), CARI_OK);                  /* stopped: not an error */
    CHECK_EQ(cari_m_bind_dl(M, 0, P(2)), CARI_OK);                           /* rebind when stopped */
    zmq_close(sub);

#ifdef CARI_HAVE_FLAC
    /* compressed DL */
    CHECK_EQ(set8(0, CARI_P_COMPRESSION, 1), CARI_OK);
    sub = cari_master_connect_dl(M, P(2), 100);
    sleep_ms(100);
    CHECK_EQ(cari_m_exec(M, 0, CARI_ACT_RX_START), CARI_OK);
    static uint8_t keep[1 << 20];
    size_t kl = 0;
    CHECK(recv_msgs(sub, 6, 3000, NULL, keep, sizeof keep, &kl) == 6);
    CHECK_EQ(cari_m_exec(M, 0, CARI_ACT_RX_STOP), CARI_OK);
    CHECK_EQ(set8(0, CARI_P_COMPRESSION, 0), CARI_OK);
    cari_flac_span_t sp[64];
    size_t skipped;
    size_t nf = cari_flac_split(keep, kl, sp, 64, &skipped);
    CHECK(nf >= 6);
    CHECK_EQ(skipped, 0);
    CHECK(cari_flac_hdr_explicit(&sp[0].hdr) && sp[0].hdr.sample_rate == 250000 && sp[0].hdr.bps == 16);
    cari_flac_dec_t *d = cari_flac_dec_new(CARI_FMT_CS16, 250000);
    CHECK_EQ(cari_flac_dec_feed(d, keep, kl), (int)nf);
    size_t dl;
    const uint8_t *o = cari_flac_dec_take(d, &dl);
    CHECK(o && dl == nf * 2500 * 4);            /* blocksize = rate / 100 */
    /* the mock sends 250 samples i*37 repeated: check the payload survived */
    if (o) {
        const int16_t *s16 = (const int16_t *)o;
        CHECK_EQ(s16[0], 0);
        CHECK_EQ(s16[1], 37);
        CHECK_EQ(s16[499], 499 * 37);
        CHECK_EQ(s16[500], 0);
    }
    cari_flac_dec_free(d);
    zmq_close(sub);
#endif
    CHECK_EQ(cari_m_bind_dl(M, 0, 0), CARI_OK);
}

static void push_n(void *push, int msgs, size_t samples_per_msg, bool partial)
{
    static int16_t buf[2 * 4096];
    for (int i = 0; i < msgs; i++)
        zmq_send(push, buf, samples_per_msg * 4 + (partial ? 2 : 0), 0);
}

static bool wait_trip_flag(bool set, int timeout_ms)
{
    int64_t end = mono_ms() + timeout_ms;
    uint32_t f = 0;
    while (mono_ms() < end) {
        cari_m_ping(M, &f);
        if (!!(f & CARI_EF_UL_TRIP) == set)
            return true;
        sleep_ms(10);
    }
    return false;
}

static void test_tx(void)
{
    cari_slave_stats_t st;
    ST(mock.underrun_after_ms, 150);
    CHECK_EQ(cari_m_exec(M, 0, CARI_ACT_TX_START), CARI_S_INVALID_STATE);     /* not bound */
    CHECK_EQ(cari_m_exec(M, 0, CARI_ACT_TX_STOP), CARI_OK);
    CHECK_EQ(cari_m_exec(M, 0, CARI_ACT_CLEAR_TRIP), CARI_OK);                /* not tripped: OK */
    CHECK_EQ(cari_m_bind_ul(M, 0, P(4)), CARI_OK);
    void *push = cari_master_connect_ul(M, P(4), 4, 0);
    CHECK_EQ(cari_m_exec(M, 0, CARI_ACT_TX_START), CARI_OK);
    CHECK_EQ(tx_state(0), CARI_TX_TRANSMITTING);
    CHECK_EQ(cari_m_bind_ul(M, 0, P(5)), CARI_S_INVALID_STATE);
    CHECK_EQ(setf(0, CARI_P_SAMPLE_RATE, 125000), CARI_S_INVALID_STATE);
    CHECK_EQ(setu(0, CARI_P_TX_FREQ, 436000000), CARI_OK);

    push_n(push, 10, 500, false);
    push_n(push, 1, 500, true);             /* partial sample at the end */
    CHECK(wait_trip_flag(true, 3000));      /* mock starves -> trip */
    CHECK_EQ(tx_state(0), CARI_TX_TRIPPED);
    CHECK_EQ(LD(mock.ul_samples[0]), 11 * 500);
    cari_slave_stats(mock.s, 0, &st);
    CHECK_EQ(st.trips, 1);
    CHECK_EQ(st.ul_bad, 1);
    CHECK_EQ(st.ul_msgs, 11);

    CHECK_EQ(cari_m_exec(M, 0, CARI_ACT_TX_START), CARI_S_INVALID_STATE);     /* tripped */
    CHECK_EQ(cari_m_exec(M, 0, CARI_ACT_TX_STOP), CARI_OK);
    CHECK_EQ(tx_state(0), CARI_TX_TRIPPED);                                   /* stays tripped */
    CHECK(wait_trip_flag(true, 100));                                         /* reading doesn't clear */
    CHECK_EQ(cari_m_exec(M, 0, CARI_ACT_CLEAR_TRIP), CARI_OK);
    CHECK_EQ(tx_state(0), CARI_TX_IDLE);
    CHECK(wait_trip_flag(false, 100));

    /* restart, trip again, recover with a subdevice reset this time */
    ST(mock.ul_samples[0], 0);
    CHECK_EQ(cari_m_exec(M, 0, CARI_ACT_TX_START), CARI_OK);
    push_n(push, 3, 1000, false);
    CHECK(wait_trip_flag(true, 3000));
    int resets = LD(mock.sub_resets);
    CHECK_EQ(cari_m_exec(M, 0, CARI_ACT_RESET), CARI_OK);
    CHECK_EQ(LD(mock.sub_resets), resets + 1);
    CHECK_EQ(tx_state(0), CARI_TX_IDLE);
    CHECK(wait_trip_flag(false, 100));
    CHECK_EQ(cari_m_bind_ul(M, 0, P(5)), CARI_OK);    /* bound sockets survive a reset; rebind */
    zmq_close(push);
    push = cari_master_connect_ul(M, P(5), 4, 0);

    /* explicit stop never trips */
    ST(mock.ul_samples[0], 0);
    CHECK_EQ(cari_m_exec(M, 0, CARI_ACT_TX_START), CARI_OK);
    push_n(push, 4, 250, false);
    int64_t end = mono_ms() + 2000;
    while (LD(mock.ul_samples[0]) < 1000 && mono_ms() < end)
        sleep_ms(5);
    CHECK_EQ(LD(mock.ul_samples[0]), 1000);
    CHECK_EQ(cari_m_exec(M, 0, CARI_ACT_TX_STOP), CARI_OK);
    CHECK_EQ(tx_state(0), CARI_TX_IDLE);
    sleep_ms(300);
    CHECK_EQ(tx_state(0), CARI_TX_IDLE);

#ifdef CARI_HAVE_FLAC
    /* compressed UL */
    CHECK_EQ(set8(0, CARI_P_COMPRESSION, 1), CARI_OK);
    CHECK_EQ(setf(0, CARI_P_SAMPLE_RATE, 125000), CARI_OK);
    ST(mock.ul_samples[0], 0);
    ST(mock.underrun_after_ms, 1000);
    CHECK_EQ(cari_m_exec(M, 0, CARI_ACT_TX_START), CARI_OK);
    cari_flac_enc_t *e = cari_flac_enc_new(CARI_FMT_CS16, 125000, 1250, 5);
    static int16_t iq[2 * 1250];
    for (int i = 0; i < 2 * 1250; i++)
        iq[i] = (int16_t)(8000 * sin(i * 0.01));
    for (int i = 0; i < 8; i++) {
        cari_flac_enc_push(e, iq, 1250);
        if (i == 7)
            cari_flac_enc_finish(e);
        size_t len;
        const uint8_t *m = cari_flac_enc_take(e, &len);
        if (m)
            zmq_send(push, m, len, 0);
    }
    cari_flac_enc_free(e);
    end = mono_ms() + 2000;
    while (LD(mock.ul_samples[0]) < 8 * 1250 && mono_ms() < end)
        sleep_ms(5);
    CHECK_EQ(LD(mock.ul_samples[0]), 8 * 1250);
    CHECK_EQ(cari_m_exec(M, 0, CARI_ACT_TX_STOP), CARI_OK);
    CHECK_EQ(set8(0, CARI_P_COMPRESSION, 0), CARI_OK);
#endif
    zmq_close(push);
}

static void test_end_of_transmission(void)
{
    cari_slave_stats_t st0, st;
    cari_slave_stats(mock.s, 0, &st0);
    ST(mock.underrun_after_ms, 2000);
    ST(mock.playout_ms, 300);
    CHECK_EQ(cari_m_bind_ul(M, 0, P(13)), CARI_OK);
    void *push = cari_master_connect_ul(M, P(13), 4, 0);
    bool v21 = false;
    CHECK_EQ(cari_m_supports(M, 2, 1, &v21), CARI_OK);
    CHECK(v21);

    /* samples, then the zero-byte marker */
    ST(mock.ul_samples[0], 0);
    CHECK_EQ(cari_m_exec(M, 0, CARI_ACT_TX_START), CARI_OK);
    push_n(push, 5, 500, false);
    CHECK_EQ(cari_master_ul_send_end(push), 0);
    int64_t end = mono_ms() + 2000;
    while (!LD(mock.ends_seen) && mono_ms() < end)
        sleep_ms(2);
    CHECK_EQ(LD(mock.ends_seen), 1);
    /* playing out: still transmitting, start and rebind refused, no trip */
    CHECK_EQ(tx_state(0), CARI_TX_TRANSMITTING);
    CHECK_EQ(cari_m_exec(M, 0, CARI_ACT_TX_START), CARI_S_INVALID_STATE);
    CHECK_EQ(cari_m_bind_ul(M, 0, P(14)), CARI_S_INVALID_STATE);
    CHECK_EQ(cari_m_wait_tx_idle(M, 0, 2000), CARI_OK);
    CHECK_EQ(LD(mock.ul_samples[0]), 5 * 500);
    uint32_t f = 1;
    CHECK_EQ(cari_m_ping(M, &f), CARI_OK);
    CHECK_EQ(f, 0);
    cari_slave_stats(mock.s, 0, &st);
    CHECK_EQ(st.ends - st0.ends, 1);
    CHECK_EQ(st.trips - st0.trips, 0);

    /* a new transmission reads the socket again */
    ST(mock.ends_seen, 0);
    ST(mock.ul_samples[0], 0);
    CHECK_EQ(cari_m_exec(M, 0, CARI_ACT_TX_START), CARI_OK);
    push_n(push, 2, 500, false);
    end = mono_ms() + 2000;
    while (LD(mock.ul_samples[0]) < 1000 && mono_ms() < end)
        sleep_ms(2);
    CHECK_EQ(LD(mock.ul_samples[0]), 1000);
    /* TX stop aborts the play-out */
    ST(mock.playout_ms, 5000);
    CHECK_EQ(cari_master_ul_send_end(push), 0);
    end = mono_ms() + 2000;
    while (!LD(mock.ends_seen) && mono_ms() < end)
        sleep_ms(2);
    int64_t t0 = mono_ms();
    CHECK_EQ(cari_m_exec(M, 0, CARI_ACT_TX_STOP), CARI_OK);
    CHECK(mono_ms() - t0 < 500);
    CHECK_EQ(tx_state(0), CARI_TX_IDLE);

#ifdef CARI_HAVE_FLAC
    /* the marker also ends a compressed stream */
    ST(mock.ends_seen, 0);
    ST(mock.ul_samples[0], 0);
    ST(mock.playout_ms, 50);
    CHECK_EQ(set8(0, CARI_P_COMPRESSION, 1), CARI_OK);
    CHECK_EQ(cari_m_exec(M, 0, CARI_ACT_TX_START), CARI_OK);
    cari_value_t rate;
    cari_m_get_param(M, 0, CARI_P_SAMPLE_RATE, &rate);
    cari_flac_enc_t *e = cari_flac_enc_new(CARI_FMT_CS16, (uint32_t)rate.v.f32, 1000, 5);
    static int16_t iq[2 * 1000];
    for (int i = 0; i < 3; i++) {
        cari_flac_enc_push(e, iq, 1000);
        if (i == 2)
            cari_flac_enc_finish(e);
        size_t len;
        const uint8_t *m = cari_flac_enc_take(e, &len);
        if (m)
            zmq_send(push, m, len, 0);
    }
    cari_flac_enc_free(e);
    CHECK_EQ(cari_master_ul_send_end(push), 0);
    CHECK_EQ(cari_m_wait_tx_idle(M, 0, 2000), CARI_OK);
    CHECK_EQ(LD(mock.ul_samples[0]), 3000);
    CHECK_EQ(set8(0, CARI_P_COMPRESSION, 0), CARI_OK);
#endif
    CHECK_EQ(LD(mock.reread_bad), 0);
    zmq_close(push);
    CHECK_EQ(cari_m_bind_ul(M, 0, 0), CARI_OK);
}

static void test_spvn(void)
{
    uint8_t q[4] = { CARI_Q_TEMPERATURE, CARI_Q_RETURN_LOSS, CARI_Q_RETURN_LOSS, CARI_Q_TEMPERATURE };
    uint8_t bad1[1] = { CARI_Q_VOLTAGE }, bad2[1] = { 0x10 };
    CHECK_EQ(cari_m_bind_spvn(M, P(6), 0, q, 2), CARI_S_OUT_OF_RANGE);
    CHECK_EQ(cari_m_bind_spvn(M, P(6), 50, bad1, 1), CARI_S_UNSUPPORTED);
    CHECK_EQ(cari_m_bind_spvn(M, P(6), 50, bad2, 1), CARI_S_UNSUPPORTED);
    CHECK_EQ(cari_m_bind_spvn(M, P(6), 50, q, 4), CARI_OK);
    void *sub = cari_master_connect_spvn(M, P(6));
    int64_t t0 = mono_ms();
    int pk = 0;
    uint8_t b[256];
    sleep_ms(100);
    while (pk < 5 && mono_ms() - t0 < 3000) {
        zmq_pollitem_t it = { sub, 0, ZMQ_POLLIN, 0 };
        if (zmq_poll(&it, 1, 100) <= 0)
            continue;
        int n = zmq_recv(sub, b, sizeof b, 0);
        cari_spvn_entry_t e[8];
        int ne = cari_spvn_parse(b, (size_t)n, e, 8);
        CHECK_EQ(ne, 2);                        /* duplicates collapsed, RL only for sub 0 */
        if (ne == 2) {
            CHECK(e[0].qid == CARI_Q_TEMPERATURE && e[0].sub == 0xFF && e[0].value == 42.0f);
            CHECK(e[1].qid == CARI_Q_RETURN_LOSS && e[1].sub == 0 && e[1].value == 19.5f);
        }
        pk++;
    }
    int64_t dt = mono_ms() - t0;
    CHECK_EQ(pk, 5);
    CHECK(dt < 1000);                           /* 50 ms period */
    /* replace the configuration on the same port, then stop */
    CHECK_EQ(cari_m_bind_spvn(M, P(6), 1000, q, 1), CARI_OK);
    CHECK_EQ(cari_m_bind_spvn(M, P(6), 0, q, 0), CARI_OK);  /* empty list: stop, period ignored */
    CHECK_EQ(cari_m_bind_spvn(M, 0, 50, q, 1), CARI_OK);
    zmq_close(sub);
}

static void test_dev_reset(void)
{
    CHECK_EQ(cari_m_bind_dl(M, 0, P(7)), CARI_OK);
    CHECK_EQ(cari_m_bind_ul(M, 0, P(8)), CARI_OK);
    CHECK_EQ(cari_m_exec(M, 0, CARI_ACT_RX_START), CARI_OK);
    CHECK_EQ(cari_m_exec(M, 0, CARI_ACT_TX_START), CARI_OK);
    int r = LD(mock.dev_resets);
    CHECK_EQ(cari_m_reset_device(M), CARI_OK);
    CHECK_EQ(tx_state(0), CARI_TX_IDLE);   /* processed after the reset */
    CHECK_EQ(LD(mock.dev_resets), r + 1);      /* the reply came first, then the reset */
    /* sockets were closed: starting needs a bind again */
    CHECK_EQ(cari_m_exec(M, 0, CARI_ACT_RX_START), CARI_S_INVALID_STATE);
    CHECK_EQ(cari_m_exec(M, 0, CARI_ACT_TX_START), CARI_S_INVALID_STATE);
    CHECK_EQ(cari_m_bind_dl(M, 0, P(7)), CARI_OK);         /* same port binds again */
    CHECK_EQ(cari_m_bind_dl(M, 0, 0), CARI_OK);
    CHECK(!cari_slave_rx_running(mock.s, 0));
}

typedef struct {
    cari_slave_t *s;
    int           stop;
    int           delay_ms;     /* bind CTRL only after this delay */
    pthread_t     th;
} runner_t;

static void *runner(void *p)
{
    runner_t *r = p;
    sleep_ms(r->delay_ms);
    if (cari_slave_start(r->s) == 0)
        cari_slave_run(r->s, &r->stop);
    return NULL;
}

static void test_master_timeout(void)
{
    /* Nobody listens: the command times out, the REQ socket is reopened. */
    char ep[64];
    snprintf(ep, sizeof ep, "127.0.0.1:%u", P(10));
    cari_master_t *m = cari_master_new(NULL, ep, 200);
    uint32_t f;
    int64_t t0 = mono_ms();
    CHECK_EQ(cari_m_ping(m, &f), CARI_E_TIMEOUT);
    CHECK(mono_ms() - t0 >= 190 && mono_ms() - t0 < 1000);
    CHECK_EQ(cari_master_reconnects(m), 1);
    CHECK_EQ(cari_m_ping(m, &f), CARI_E_TIMEOUT);       /* REQ not stuck in "await reply" */
    CHECK_EQ(cari_master_reconnects(m), 2);

    /* The slave appears: the next command goes through. This one has no
     * supervision capability. */
    runner_t r = { .s = make_slave(P(10), false) };
    CHECK(r.s != NULL);
    pthread_create(&r.th, NULL, runner, &r);
    cari_master_set_timeout(m, 1000);
    CHECK_EQ(cari_m_ping(m, &f), CARI_OK);
    cari_caps_t c;
    CHECK_EQ(cari_m_get_dev_caps(m, &c), CARI_OK);
    CHECK(!cari_caps_has_flag(&c, CARI_DCAP_SUPERVISION));
    uint8_t q[1] = { 0 };
    size_t nq;
    CHECK_EQ(cari_m_bind_spvn(m, P(11), 100, q, 1), CARI_S_UNSUPPORTED);
    CHECK_EQ(cari_m_get_spvn_list(m, q, 1, &nq), CARI_S_UNSUPPORTED);
    cari_master_free(m);
    ST(r.stop, 1);
    pthread_join(r.th, NULL);
    cari_slave_free(r.s);
}

static void test_retry(void)
{
    /* With retries, a command whose first attempts time out still succeeds
     * once the slave comes up (here: 400 ms late, 150 ms timeout). */
    char ep[64];
    snprintf(ep, sizeof ep, "127.0.0.1:%u", P(12));
    cari_master_t *m = cari_master_new(NULL, ep, 150);
    cari_master_set_retries(m, 5);
    runner_t r = { .s = make_slave(P(12), false), .delay_ms = 400 };
    pthread_create(&r.th, NULL, runner, &r);
    uint32_t f = 1;
    CHECK_EQ(cari_m_ping(m, &f), CARI_OK);
    CHECK_EQ(f, 0);
    CHECK(cari_master_reconnects(m) >= 2);
    cari_master_free(m);
    ST(r.stop, 1);
    pthread_join(r.th, NULL);
    cari_slave_free(r.s);
}

int main(void)
{
    /* below the Linux ephemeral range (32768+) so a bind never collides with
     * the local port of an outgoing connection */
    base_port = 10000 + (getpid() % 1800) * 12;
    if (getenv("CARI_TEST_VERBOSE"))
        cari_log_set_handler(NULL, NULL, CARI_LOG_DEBUG);
    else
        cari_log_set_handler(NULL, NULL, CARI_LOG_ERROR);

    mock.s = make_slave(P(0), true);
    CHECK(mock.s != NULL);
    if (!mock.s)
        return 1;
    CHECK_EQ(cari_slave_start(mock.s), 0);
    pthread_create(&slave_th, NULL, slave_thread, mock.s);

    snprintf(ctrl_ep, sizeof ctrl_ep, "127.0.0.1:%u", P(0));
    M = cari_master_new(NULL, ctrl_ep, 2000);
    CHECK(M != NULL);
    raw_req = zmq_socket(cari_master_zmq_ctx(M), ZMQ_REQ);
    char ep[64];
    snprintf(ep, sizeof ep, "tcp://%s", ctrl_ep);
    int z = 0;
    zmq_setsockopt(raw_req, ZMQ_LINGER, &z, sizeof z);
    zmq_connect(raw_req, ep);

    RUN(test_basic);
    RUN(test_reply_rules);
    RUN(test_params);
    RUN(test_rx);
    RUN(test_tx);
    RUN(test_end_of_transmission);
    RUN(test_spvn);
    RUN(test_dev_reset);

    zmq_close(raw_req);
    ST(slave_stop, 1);
    pthread_join(slave_th, NULL);

    RUN(test_master_timeout);
    RUN(test_retry);

    cari_master_free(M);
    cari_slave_free(mock.s);
    T_DONE();
}
