/*
 * libcari - CARI 2.1 slave over ZeroMQ
 *
 * Command handling order (the spec does not mandate one; this is ours):
 *   1. framing (status 1), unknown CID or parameter ID (2), per-CID length (1)
 *   2. address: no such subdevice / register (6)
 *   3. support: capability or callback missing (2)
 *   4. read-only (7)
 *   5. value (5)
 *   6. state (8)
 */
#include "cari/cari_config.h"
#include "cari/cari_slave.h"
#include "cari/cari_log.h"
#ifdef CARI_HAVE_FLAC
#include "cari/cari_flac.h"
#endif

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zmq.h>

#define POLL_SLICE_MS   20      /* max latency for trip processing */

typedef struct {
    cari_caps_t     caps;
    uint8_t        *caps_tlv;
    size_t          caps_len;

    bool            supported[CARI_P_COUNT];
    cari_value_t    param[CARI_P_COUNT];

    void           *ul_sock, *dl_sock;
    uint16_t        ul_port, dl_port;

    bool            rx_running;
    int             tx_state;
    int             pending_underrun;   /* atomic */
    int             pending_finished;   /* atomic: play-out after end marker done */
    int             ul_end;             /* atomic: end marker read, socket no longer read */

    /* stream snapshots, fixed while running */
    uint8_t         dl_fmt, ul_fmt;
    size_t          dl_bps, ul_bps;
    bool            dl_comp, ul_comp;
#ifdef CARI_HAVE_FLAC
    cari_flac_enc_t *enc;
    cari_flac_dec_t *dec;
#endif
    zmq_msg_t       ul_msg;
    bool            ul_msg_valid;

    cari_slave_stats_t st;              /* updated with __atomic builtins */
} sub_t;

struct cari_slave {
    cari_slave_desc_t d;
    cari_slave_ops_t  ops;
    void             *user;

    char             *ident;
    uint8_t          *dev_caps_tlv;
    size_t            dev_caps_len;
    cari_caps_t       dev_caps;
    uint8_t           quantities[256];
    size_t            nquantities;

    void             *ctx;
    bool              own_ctx;
    void             *ctrl;
    char             *ctrl_ep;
    char             *bind_host;

    sub_t            *subs;
    uint8_t           nsubs;
    pthread_mutex_t   mtx;

    void             *spvn_sock;
    uint16_t          spvn_port;
    uint16_t          spvn_period;
    uint8_t           spvn_q[CARI_Q_COUNT];
    size_t            spvn_nq;
    int64_t           spvn_next;

    bool              pending_dev_reset;
    uint8_t           tx[CARI_MAX_FRAME];
    uint8_t           data[CARI_MAX_FRAME];
};

#define STAT_ADD(sb, field, v) __atomic_fetch_add(&(sb)->st.field, (uint64_t)(v), __ATOMIC_RELAXED)

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint8_t *dupmem(const void *p, size_t n)
{
    uint8_t *r = malloc(n ? n : 1);
    if (r && n)
        memcpy(r, p, n);
    return r;
}

/* ---------------------------------------------------------------- setup */

cari_slave_t *cari_slave_new(const cari_slave_desc_t *desc, const cari_slave_ops_t *ops, void *user)
{
    cari_slave_t *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->d = *desc;
    if (ops)
        s->ops = *ops;
    s->user = user;
    pthread_mutex_init(&s->mtx, NULL);

    s->ident = strdup(desc->ident ? desc->ident : "");
    s->dev_caps_tlv = dupmem(desc->dev_caps, desc->dev_caps_len);
    s->dev_caps_len = desc->dev_caps_len;
    s->ctrl_ep = strdup(desc->ctrl_endpoint ? desc->ctrl_endpoint : "tcp://*:17002");
    s->bind_host = strdup(desc->bind_host ? desc->bind_host : "*");
    if (!s->ident || !s->dev_caps_tlv || !s->ctrl_ep || !s->bind_host)
        goto fail;
    if (!s->d.dl_sndhwm)   s->d.dl_sndhwm = 16;
    if (!s->d.ul_rcvhwm)   s->d.ul_rcvhwm = 2;
    if (!s->d.spvn_sndhwm) s->d.spvn_sndhwm = 8;
    if (!s->d.flac_level)  s->d.flac_level = 5;

    if (cari_caps_parse(desc->dev_caps, desc->dev_caps_len, false, &s->dev_caps) < 0) {
        cari_log(CARI_LOG_ERROR, "invalid device capability list");
        goto fail;
    }
#ifndef CARI_HAVE_FLAC
    if (cari_caps_has_flag(&s->dev_caps, CARI_DCAP_COMPRESSION)) {
        cari_log(CARI_LOG_ERROR, "compression advertised but libcari was built without libFLAC");
        goto fail;
    }
#endif
    for (size_t i = 0; i < desc->nquantities && i < 256; i++) {
        if (!cari_quantity_info(desc->quantities[i])) {
            cari_log(CARI_LOG_ERROR, "unknown supervision quantity 0x%02X", desc->quantities[i]);
            goto fail;
        }
        s->quantities[s->nquantities++] = desc->quantities[i];
    }

    s->nsubs = desc->nsubdevs;
    s->subs = calloc(s->nsubs ? s->nsubs : 1, sizeof(sub_t));
    if (!s->subs)
        goto fail;
    for (unsigned i = 0; i < s->nsubs; i++) {
        sub_t *sb = &s->subs[i];
        if (cari_caps_parse(desc->subdevs[i].caps, desc->subdevs[i].caps_len, true, &sb->caps) < 0) {
            cari_log(CARI_LOG_ERROR, "invalid capability list for subdevice %u", i);
            goto fail;
        }
        sb->caps_tlv = dupmem(desc->subdevs[i].caps, desc->subdevs[i].caps_len);
        sb->caps_len = desc->subdevs[i].caps_len;
        if (!sb->caps_tlv)
            goto fail;
        sb->tx_state = CARI_TX_IDLE;
    }

    if (desc->zmq_ctx) {
        s->ctx = desc->zmq_ctx;
    } else {
        s->ctx = zmq_ctx_new();
        s->own_ctx = true;
        if (!s->ctx)
            goto fail;
    }
    return s;
fail:
    cari_slave_free(s);
    return NULL;
}

int cari_slave_param_init(cari_slave_t *s, uint8_t sub, uint8_t param, cari_value_t v)
{
    const cari_param_info_t *pi = cari_param_info(param);
    if (sub >= s->nsubs || !pi || (pi->flags & CARI_PF_READONLY) || pi->type != v.type)
        return -1;
    pthread_mutex_lock(&s->mtx);
    s->subs[sub].supported[param] = true;
    s->subs[sub].param[param] = v;
    pthread_mutex_unlock(&s->mtx);
    return 0;
}

int cari_slave_param_get(cari_slave_t *s, uint8_t sub, uint8_t param, cari_value_t *v)
{
    if (sub >= s->nsubs || param >= CARI_P_COUNT)
        return -1;
    int r = -1;
    pthread_mutex_lock(&s->mtx);
    if (param == CARI_P_TX_STATE) {
        *v = cari_value_u8((uint8_t)s->subs[sub].tx_state);
        r = 0;
    } else if (s->subs[sub].supported[param]) {
        *v = s->subs[sub].param[param];
        r = 0;
    }
    pthread_mutex_unlock(&s->mtx);
    return r;
}

static void set_opt_int(void *sock, int opt, int v)
{
    zmq_setsockopt(sock, opt, &v, sizeof v);
}

int cari_slave_start(cari_slave_t *s)
{
    s->ctrl = zmq_socket(s->ctx, ZMQ_REP);
    if (!s->ctrl)
        return -1;
    set_opt_int(s->ctrl, ZMQ_LINGER, 0);
    if (zmq_bind(s->ctrl, s->ctrl_ep) != 0) {
        cari_log(CARI_LOG_ERROR, "CTRL bind to %s failed: %s", s->ctrl_ep, zmq_strerror(errno));
        zmq_close(s->ctrl);
        s->ctrl = NULL;
        return -1;
    }
    cari_log(CARI_LOG_INFO, "CTRL listening on %s", s->ctrl_ep);
    return 0;
}

/* ---------------------------------------------------------------- sockets */

static void *bind_data_socket(cari_slave_t *s, int type, uint16_t port, int hwm)
{
    char ep[300];
    void *z = zmq_socket(s->ctx, type);
    if (!z)
        return NULL;
    set_opt_int(z, ZMQ_LINGER, 0);
    if (strchr(s->bind_host, ':'))
        set_opt_int(z, ZMQ_IPV6, 1);
    if (type == ZMQ_PUB) {
        set_opt_int(z, ZMQ_SNDHWM, hwm);
    } else {
        set_opt_int(z, ZMQ_RCVHWM, hwm);
        if (s->d.ul_rcvbuf > 0)
            set_opt_int(z, ZMQ_RCVBUF, s->d.ul_rcvbuf);
    }
    snprintf(ep, sizeof ep, "tcp://%s:%u", s->bind_host, port);
    if (zmq_bind(z, ep) != 0) {
        cari_log(CARI_LOG_WARN, "bind %s failed: %s", ep, zmq_strerror(errno));
        zmq_close(z);
        return NULL;
    }
    return z;
}

static void close_sock(void **z)
{
    if (*z) {
        zmq_close(*z);
        *z = NULL;
    }
}

/* ---------------------------------------------------------------- streams */

static bool has(const sub_t *sb, uint8_t cap) { return cari_caps_has_flag(&sb->caps, cap); }

static void dl_flush_encoder(sub_t *sb)
{
#ifdef CARI_HAVE_FLAC
    if (!sb->enc)
        return;
    size_t len;
    cari_flac_enc_finish(sb->enc);
    const uint8_t *m = cari_flac_enc_take(sb->enc, &len);
    if (m && sb->dl_sock && zmq_send(sb->dl_sock, m, len, ZMQ_DONTWAIT) >= 0)
        STAT_ADD(sb, dl_msgs, 1);
    cari_flac_enc_free(sb->enc);
    sb->enc = NULL;
#else
    (void)sb;
#endif
}

static void stop_rx(cari_slave_t *s, uint8_t idx)
{
    sub_t *sb = &s->subs[idx];
    if (!sb->rx_running)
        return;
    if (s->ops.stream_stop)
        s->ops.stream_stop(s->user, idx, CARI_DIR_DL);
    dl_flush_encoder(sb);
    pthread_mutex_lock(&s->mtx);
    sb->rx_running = false;
    pthread_mutex_unlock(&s->mtx);
    cari_log(CARI_LOG_INFO, "sub %u: reception stopped", idx);
}

static void ul_release(sub_t *sb)
{
    if (sb->ul_msg_valid) {
        zmq_msg_close(&sb->ul_msg);
        sb->ul_msg_valid = false;
    }
#ifdef CARI_HAVE_FLAC
    cari_flac_dec_free(sb->dec);
    sb->dec = NULL;
#endif
}

/* Leave the transmitting state (to idle or tripped). */
static void stop_tx(cari_slave_t *s, uint8_t idx, int new_state)
{
    sub_t *sb = &s->subs[idx];
    if (sb->tx_state == CARI_TX_TRANSMITTING) {
        if (s->ops.stream_stop)
            s->ops.stream_stop(s->user, idx, CARI_DIR_UL);
        ul_release(sb);
    }
    pthread_mutex_lock(&s->mtx);
    sb->tx_state = new_state;
    pthread_mutex_unlock(&s->mtx);
}

static void process_events(cari_slave_t *s)
{
    for (unsigned i = 0; i < s->nsubs; i++) {
        sub_t *sb = &s->subs[i];
        if (__atomic_exchange_n(&sb->pending_finished, 0, __ATOMIC_SEQ_CST) &&
            sb->tx_state == CARI_TX_TRANSMITTING) {
            stop_tx(s, (uint8_t)i, CARI_TX_IDLE);
            STAT_ADD(sb, ends, 1);
            cari_log(CARI_LOG_INFO, "sub %u: transmission ended (end of UL stream)", i);
        }
        if (!__atomic_exchange_n(&sb->pending_underrun, 0, __ATOMIC_SEQ_CST))
            continue;
        if (sb->tx_state != CARI_TX_TRANSMITTING)
            continue;
        stop_tx(s, (uint8_t)i, CARI_TX_TRIPPED);
        STAT_ADD(sb, trips, 1);
        cari_log(CARI_LOG_WARN, "sub %u: UL underrun, transmitter tripped", i);
    }
}

static void stream_params(cari_slave_t *s, sub_t *sb, uint8_t *fmt, uint8_t *mode, bool *comp,
                          double *rate, bool *have_fmt)
{
    pthread_mutex_lock(&s->mtx);
    *have_fmt = sb->supported[CARI_P_FORMAT];
    *fmt = sb->param[CARI_P_FORMAT].v.u8;
    *mode = sb->supported[CARI_P_MODE] ? sb->param[CARI_P_MODE].v.u8 : CARI_MODE_IQ;
    *comp = sb->supported[CARI_P_COMPRESSION] && sb->param[CARI_P_COMPRESSION].v.u8;
    *rate = sb->supported[CARI_P_SAMPLE_RATE] ? sb->param[CARI_P_SAMPLE_RATE].v.f32 : 0.0;
    pthread_mutex_unlock(&s->mtx);
}

static int start_rx(cari_slave_t *s, uint8_t idx)
{
    sub_t *sb = &s->subs[idx];
    uint8_t fmt, mode;
    bool comp, have_fmt;
    double rate;

    if (!has(sb, CARI_SCAP_RX))
        return CARI_S_UNSUPPORTED;
    if (sb->rx_running)
        return CARI_OK;
    if (!sb->dl_sock)
        return CARI_S_INVALID_STATE;
    stream_params(s, sb, &fmt, &mode, &comp, &rate, &have_fmt);
    if (!have_fmt || cari_stream_combo_check(fmt, mode, comp, rate) != CARI_OK)
        return CARI_S_INVALID_STATE;
    sb->dl_fmt = fmt;
    sb->dl_bps = cari_format_bytes(fmt);
    sb->dl_comp = comp;
#ifdef CARI_HAVE_FLAC
    if (comp) {
        uint32_t bs = s->d.flac_blocksize ? s->d.flac_blocksize : (uint32_t)(rate / 100.0);
        if (bs < 16) bs = 16;
        if (bs > 16384) bs = 16384;
        sb->enc = cari_flac_enc_new(fmt, (uint32_t)rate, bs, s->d.flac_level);
        if (!sb->enc)
            return CARI_S_INVALID_STATE;
    }
#endif
    int st = s->ops.stream_start ? s->ops.stream_start(s->user, idx, CARI_DIR_DL) : CARI_OK;
    if (st != CARI_OK) {
#ifdef CARI_HAVE_FLAC
        cari_flac_enc_free(sb->enc);
        sb->enc = NULL;
#endif
        return st;
    }
    pthread_mutex_lock(&s->mtx);
    sb->rx_running = true;
    pthread_mutex_unlock(&s->mtx);
    cari_log(CARI_LOG_INFO, "sub %u: reception started (%s, %.0f Hz%s)", idx,
             cari_format_info(fmt)->name, rate, comp ? ", FLAC" : "");
    return CARI_OK;
}

static int start_tx(cari_slave_t *s, uint8_t idx)
{
    sub_t *sb = &s->subs[idx];
    uint8_t fmt, mode;
    bool comp, have_fmt;
    double rate;

    if (!has(sb, CARI_SCAP_TX))
        return CARI_S_UNSUPPORTED;
    if (sb->tx_state == CARI_TX_TRANSMITTING)   /* playing out after the end marker: 8 */
        return __atomic_load_n(&sb->ul_end, __ATOMIC_SEQ_CST) ? CARI_S_INVALID_STATE : CARI_OK;
    if (sb->tx_state == CARI_TX_TRIPPED)
        return CARI_S_INVALID_STATE;
    if (!sb->ul_sock)
        return CARI_S_INVALID_STATE;
    stream_params(s, sb, &fmt, &mode, &comp, &rate, &have_fmt);
    if (!have_fmt || cari_stream_combo_check(fmt, mode, comp, rate) != CARI_OK)
        return CARI_S_INVALID_STATE;
    sb->ul_fmt = fmt;
    sb->ul_bps = cari_format_bytes(fmt);
    sb->ul_comp = comp;
#ifdef CARI_HAVE_FLAC
    if (comp) {
        sb->dec = cari_flac_dec_new(fmt, (uint32_t)rate);
        if (!sb->dec)
            return CARI_S_INVALID_STATE;
    }
#endif
    __atomic_store_n(&sb->pending_underrun, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&sb->pending_finished, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&sb->ul_end, 0, __ATOMIC_SEQ_CST);
    int st = s->ops.stream_start ? s->ops.stream_start(s->user, idx, CARI_DIR_UL) : CARI_OK;
    if (st != CARI_OK) {
        ul_release(sb);
        return st;
    }
    pthread_mutex_lock(&s->mtx);
    sb->tx_state = CARI_TX_TRANSMITTING;
    pthread_mutex_unlock(&s->mtx);
    cari_log(CARI_LOG_INFO, "sub %u: transmission started (%s, %.0f Hz%s)", idx,
             cari_format_info(fmt)->name, rate, comp ? ", FLAC" : "");
    return CARI_OK;
}

/* ---------------------------------------------------------------- reset */

static void spvn_close(cari_slave_t *s)
{
    close_sock(&s->spvn_sock);
    s->spvn_port = 0;
    s->spvn_nq = 0;
    s->spvn_period = 0;
}

static void shutdown_all(cari_slave_t *s)
{
    for (unsigned i = 0; i < s->nsubs; i++) {
        sub_t *sb = &s->subs[i];
        stop_rx(s, (uint8_t)i);
        stop_tx(s, (uint8_t)i, CARI_TX_IDLE);
        __atomic_store_n(&sb->pending_underrun, 0, __ATOMIC_SEQ_CST);
        close_sock(&sb->ul_sock);
        close_sock(&sb->dl_sock);
        sb->ul_port = sb->dl_port = 0;
    }
    spvn_close(s);
}

static void dev_reset(cari_slave_t *s)
{
    cari_log(CARI_LOG_INFO, "device reset");
    shutdown_all(s);
    if (s->ops.dev_reset)
        s->ops.dev_reset(s->user);
}

/* ---------------------------------------------------------------- handlers */

static sub_t *get_sub(cari_slave_t *s, uint8_t addr)
{
    return addr < s->nsubs ? &s->subs[addr] : NULL;
}

static int h_set_param(cari_slave_t *s, const cari_cmd_t *c)
{
    sub_t *sb = get_sub(s, c->addr);
    uint8_t p = c->u.set_param.param;
    cari_value_t v = c->u.set_param.value;
    if (!sb)
        return CARI_S_INVALID_ADDR;
    if (p == CARI_P_TX_STATE)
        return has(sb, CARI_SCAP_TX) ? CARI_S_READ_ONLY : CARI_S_UNSUPPORTED;
    if (!sb->supported[p])
        return CARI_S_UNSUPPORTED;
    uint8_t mode = sb->supported[CARI_P_MODE] ? sb->param[CARI_P_MODE].v.u8 : CARI_MODE_IQ;
    int st = cari_param_check(&sb->caps, &s->dev_caps, p, &v, mode);
    if (st != CARI_OK)
        return st;
    if ((cari_param_info(p)->flags & CARI_PF_STREAM_LOCKED) &&
        (sb->rx_running || sb->tx_state == CARI_TX_TRANSMITTING))
        return CARI_S_INVALID_STATE;
    if (s->ops.set_param) {
        st = s->ops.set_param(s->user, c->addr, p, &v);
        if (st != CARI_OK)
            return st;
        if (v.type != cari_param_info(p)->type)
            return CARI_S_OUT_OF_RANGE;     /* callback bug, keep the cache sane */
    }
    pthread_mutex_lock(&s->mtx);
    sb->param[p] = v;
    pthread_mutex_unlock(&s->mtx);
    return CARI_OK;
}

static int h_get_param(cari_slave_t *s, const cari_cmd_t *c, uint8_t *out, size_t *outlen)
{
    sub_t *sb = get_sub(s, c->addr);
    uint8_t p = c->u.get_param.param;
    cari_value_t v;
    if (!sb)
        return CARI_S_INVALID_ADDR;
    if (!cari_param_info(p))
        return CARI_S_UNSUPPORTED;
    if (p == CARI_P_TX_STATE) {
        if (!has(sb, CARI_SCAP_TX))
            return CARI_S_UNSUPPORTED;
        v = cari_value_u8((uint8_t)sb->tx_state);
    } else {
        if (!sb->supported[p])
            return CARI_S_UNSUPPORTED;
        pthread_mutex_lock(&s->mtx);
        v = sb->param[p];
        pthread_mutex_unlock(&s->mtx);
        cari_value_t live = v;
        if (s->ops.get_param && s->ops.get_param(s->user, c->addr, p, &live) == CARI_OK &&
            live.type == v.type)
            v = live;
    }
    *outlen = cari_value_encode(&v, out);
    return CARI_OK;
}

static int h_exec(cari_slave_t *s, const cari_cmd_t *c)
{
    sub_t *sb = get_sub(s, c->addr);
    uint8_t idx = c->addr;
    if (!sb)
        return CARI_S_INVALID_ADDR;
    switch (c->u.exec.action) {
    case CARI_ACT_RESET:
        if (!has(sb, CARI_SCAP_RESET))
            return CARI_S_UNSUPPORTED;
        stop_rx(s, idx);
        stop_tx(s, idx, CARI_TX_IDLE);          /* also clears a trip */
        __atomic_store_n(&sb->pending_underrun, 0, __ATOMIC_SEQ_CST);
        cari_log(CARI_LOG_INFO, "sub %u: reset", idx);
        return s->ops.sub_reset ? s->ops.sub_reset(s->user, idx) : CARI_OK;
    case CARI_ACT_RX_START:
        return start_rx(s, idx);
    case CARI_ACT_RX_STOP:
        if (!has(sb, CARI_SCAP_RX))
            return CARI_S_UNSUPPORTED;
        stop_rx(s, idx);
        return CARI_OK;
    case CARI_ACT_TX_START:
        return start_tx(s, idx);
    case CARI_ACT_TX_STOP:
        if (!has(sb, CARI_SCAP_TX))
            return CARI_S_UNSUPPORTED;
        if (sb->tx_state == CARI_TX_TRANSMITTING) {
            stop_tx(s, idx, CARI_TX_IDLE);
            cari_log(CARI_LOG_INFO, "sub %u: transmission stopped", idx);
        }
        return CARI_OK;                         /* a tripped subdevice stays tripped */
    case CARI_ACT_CLEAR_TRIP:
        if (!has(sb, CARI_SCAP_TX))
            return CARI_S_UNSUPPORTED;
        if (sb->tx_state == CARI_TX_TRIPPED) {
            stop_tx(s, idx, CARI_TX_IDLE);
            cari_log(CARI_LOG_INFO, "sub %u: trip cleared", idx);
        }
        return CARI_OK;
    default:
        return CARI_S_UNSUPPORTED;
    }
}

static int h_bind(cari_slave_t *s, const cari_cmd_t *c)
{
    sub_t *sb = get_sub(s, c->addr);
    bool ul = c->cid == CARI_CID_BIND_UL;
    uint16_t port = c->u.bind.port;
    if (!sb)
        return CARI_S_INVALID_ADDR;
    if (!has(sb, ul ? CARI_SCAP_TX : CARI_SCAP_RX))
        return CARI_S_UNSUPPORTED;
    if (ul ? sb->tx_state == CARI_TX_TRANSMITTING : sb->rx_running)
        return CARI_S_INVALID_STATE;
    void **sock = ul ? &sb->ul_sock : &sb->dl_sock;
    uint16_t *cur = ul ? &sb->ul_port : &sb->dl_port;
    if (port == *cur && (port == 0 || *sock))
        return CARI_OK;                         /* already bound there */
    if (port == 0) {
        close_sock(sock);
        *cur = 0;
        cari_log(CARI_LOG_INFO, "sub %u: %s socket closed", c->addr, ul ? "UL" : "DL");
        return CARI_OK;
    }
    void *z = bind_data_socket(s, ul ? ZMQ_PULL : ZMQ_PUB, port,
                               ul ? s->d.ul_rcvhwm : s->d.dl_sndhwm);
    if (!z)
        return CARI_S_BIND_FAILED;              /* previous socket stays bound */
    close_sock(sock);
    *sock = z;
    *cur = port;
    cari_log(CARI_LOG_INFO, "sub %u: %s %s socket bound on port %u", c->addr, ul ? "UL" : "DL",
             ul ? "PULL" : "PUB", port);
    return CARI_OK;
}

static bool quantity_supported(cari_slave_t *s, uint8_t q)
{
    for (size_t i = 0; i < s->nquantities; i++)
        if (s->quantities[i] == q)
            return true;
    return false;
}

static int h_spvn(cari_slave_t *s, const cari_cmd_t *c)
{
    if (!cari_caps_has_flag(&s->dev_caps, CARI_DCAP_SUPERVISION))
        return CARI_S_UNSUPPORTED;
    if (c->u.spvn.port == 0 || c->u.spvn.nq == 0) {
        spvn_close(s);
        cari_log(CARI_LOG_INFO, "supervision stopped");
        return CARI_OK;
    }
    if (c->u.spvn.period_ms == 0)
        return CARI_S_OUT_OF_RANGE;
    uint8_t q[CARI_Q_COUNT];
    size_t nq = 0;
    for (size_t i = 0; i < c->u.spvn.nq; i++) {
        uint8_t id = c->u.spvn.qids[i];
        if (!cari_quantity_info(id) || !quantity_supported(s, id))
            return CARI_S_UNSUPPORTED;
        bool dup = false;                       /* never repeat a quantity */
        for (size_t j = 0; j < nq; j++)
            dup |= q[j] == id;
        if (!dup)
            q[nq++] = id;
    }
    if (c->u.spvn.port != s->spvn_port || !s->spvn_sock) {
        void *z = bind_data_socket(s, ZMQ_PUB, c->u.spvn.port, s->d.spvn_sndhwm);
        if (!z)
            return CARI_S_BIND_FAILED;
        close_sock(&s->spvn_sock);
        s->spvn_sock = z;
        s->spvn_port = c->u.spvn.port;
    }
    memcpy(s->spvn_q, q, nq);
    s->spvn_nq = nq;
    s->spvn_period = c->u.spvn.period_ms;
    s->spvn_next = now_ms() + s->spvn_period;
    cari_log(CARI_LOG_INFO, "supervision on port %u every %u ms (%zu quantities)", s->spvn_port,
             s->spvn_period, nq);
    return CARI_OK;
}

static void spvn_publish(cari_slave_t *s)
{
    uint8_t pkt[1024];
    int len = 0;
    float v;
    for (size_t i = 0; i < s->spvn_nq; i++) {
        uint8_t q = s->spvn_q[i];
        if (!s->ops.spvn_read)
            break;
        if (cari_quantity_info(q)->per_subdevice) {
            for (unsigned sub = 0; sub < s->nsubs; sub++)
                if (s->ops.spvn_read(s->user, q, (uint8_t)sub, &v) == CARI_OK) {
                    int n = cari_spvn_append(pkt, sizeof pkt, (size_t)len, q, (uint8_t)sub, v);
                    if (n > 0) len = n;
                }
        } else if (s->ops.spvn_read(s->user, q, 0xFF, &v) == CARI_OK) {
            int n = cari_spvn_append(pkt, sizeof pkt, (size_t)len, q, 0xFF, v);
            if (n > 0) len = n;
        }
    }
    if (len > 0)
        zmq_send(s->spvn_sock, pkt, (size_t)len, ZMQ_DONTWAIT);
}

static void send_reply(cari_slave_t *s, uint8_t cid, int status, const uint8_t *data, size_t len)
{
    int n = cari_reply_encode(s->tx, sizeof s->tx, cid, (uint8_t)status, data, len);
    if (n < 0)
        n = cari_reply_encode(s->tx, sizeof s->tx, cid, CARI_S_MALFORMED, NULL, 0);
    if (zmq_send(s->ctrl, s->tx, (size_t)n, 0) < 0)
        cari_log(CARI_LOG_ERROR, "CTRL reply failed: %s", zmq_strerror(errno));
}

static void handle(cari_slave_t *s, const uint8_t *buf, size_t n, bool multipart)
{
    cari_cmd_t c;
    int st = cari_cmd_decode(buf, n, &c);
    uint8_t *data = s->data;
    size_t dlen = 0;

    if (multipart)
        st = CARI_S_MALFORMED;
    if (st != CARI_OK) {
        cari_log(CARI_LOG_DEBUG, "CID 0x%02X: %s", c.cid, cari_status_str(st));
        send_reply(s, c.cid, st, NULL, 0);
        return;
    }
    process_events(s);

    switch (c.cid) {
    case CARI_CID_PING: {
        uint32_t f = s->ops.error_flags ? s->ops.error_flags(s->user) : 0;
        f &= ~CARI_EF_UL_TRIP;
        for (unsigned i = 0; i < s->nsubs; i++)
            if (s->subs[i].tx_state == CARI_TX_TRIPPED)
                f |= CARI_EF_UL_TRIP;
        cari_put_le32(data, f);
        dlen = 4;
        st = CARI_OK;
        break;
    }
    case CARI_CID_SET_REG:
        if (c.addr == CARI_REG_VERSION) {
            st = CARI_OK;
            s->pending_dev_reset = true;        /* reply first, then reset */
        } else if (c.addr == CARI_REG_NUM_SUBDEVS) {
            st = CARI_S_READ_ONLY;
        } else {
            st = s->ops.reg_write ? s->ops.reg_write(s->user, c.addr, c.u.set_reg.value)
                                  : CARI_S_INVALID_ADDR;
        }
        break;
    case CARI_CID_SET_PARAM:
        st = h_set_param(s, &c);
        break;
    case CARI_CID_EXEC:
        st = h_exec(s, &c);
        break;
    case CARI_CID_BIND_UL:
    case CARI_CID_BIND_DL:
        st = h_bind(s, &c);
        break;
    case CARI_CID_BIND_SPVN:
        st = h_spvn(s, &c);
        break;
    case CARI_CID_GET_IDENT:
        dlen = strlen(s->ident);
        if (dlen > CARI_MAX_FRAME - 4)
            dlen = CARI_MAX_FRAME - 4;
        memcpy(data, s->ident, dlen);
        st = CARI_OK;
        break;
    case CARI_CID_GET_REG:
        if (c.addr == CARI_REG_VERSION) {
            data[0] = CARI_VERSION_BYTE;
            st = CARI_OK;
        } else if (c.addr == CARI_REG_NUM_SUBDEVS) {
            data[0] = s->nsubs;
            st = CARI_OK;
        } else {
            st = s->ops.reg_read ? s->ops.reg_read(s->user, c.addr, data) : CARI_S_INVALID_ADDR;
        }
        dlen = 1;
        break;
    case CARI_CID_GET_SUB_CAPS: {
        sub_t *sb = get_sub(s, c.addr);
        if (!sb) {
            st = CARI_S_INVALID_ADDR;
            break;
        }
        dlen = sb->caps_len;
        memcpy(data, sb->caps_tlv, dlen);
        st = CARI_OK;
        break;
    }
    case CARI_CID_GET_PARAM:
        st = h_get_param(s, &c, data, &dlen);
        break;
    case CARI_CID_GET_SPVN_LIST:
        if (!cari_caps_has_flag(&s->dev_caps, CARI_DCAP_SUPERVISION)) {
            st = CARI_S_UNSUPPORTED;
            break;
        }
        dlen = s->nquantities;
        memcpy(data, s->quantities, dlen);
        st = CARI_OK;
        break;
    case CARI_CID_GET_DEV_CAPS:
        dlen = s->dev_caps_len;
        memcpy(data, s->dev_caps_tlv, dlen);
        st = CARI_OK;
        break;
    default:
        st = CARI_S_UNSUPPORTED;
        break;
    }
    cari_log(CARI_LOG_DEBUG, "%s addr=%u -> %s", cari_cmd_info(c.cid)->name, c.addr, cari_status_str(st));
    send_reply(s, c.cid, st, data, dlen);

    if (s->pending_dev_reset) {
        s->pending_dev_reset = false;
        dev_reset(s);
    }
}

/* ---------------------------------------------------------------- loop */

int cari_slave_poll(cari_slave_t *s, int timeout_ms)
{
    if (!s->ctrl)
        return -1;
    int64_t deadline = now_ms() + (timeout_ms < 0 ? 0 : timeout_ms);
    for (;;) {
        process_events(s);
        int64_t now = now_ms();
        if (s->spvn_sock && s->spvn_nq && now >= s->spvn_next) {
            spvn_publish(s);
            s->spvn_next += s->spvn_period;
            if (s->spvn_next <= now)
                s->spvn_next = now + s->spvn_period;
        }
        int64_t wait = deadline - now;
        if (wait > POLL_SLICE_MS)
            wait = POLL_SLICE_MS;
        if (s->spvn_sock && s->spvn_nq && s->spvn_next - now < wait)
            wait = s->spvn_next - now;
        if (wait < 0)
            wait = 0;

        zmq_pollitem_t it = { .socket = s->ctrl, .events = ZMQ_POLLIN };
        int r = zmq_poll(&it, 1, (long)wait);
        if (r < 0) {
            if (errno == EINTR)
                return 0;
            cari_log(CARI_LOG_ERROR, "zmq_poll: %s", zmq_strerror(errno));
            return -1;
        }
        if (r > 0 && (it.revents & ZMQ_POLLIN)) {
            zmq_msg_t m;
            zmq_msg_init(&m);
            if (zmq_msg_recv(&m, s->ctrl, ZMQ_DONTWAIT) >= 0) {
                bool more = zmq_msg_more(&m);
                if (more) {             /* drain the remaining parts */
                    zmq_msg_t x;
                    zmq_msg_init(&x);
                    while (zmq_msg_recv(&x, s->ctrl, ZMQ_DONTWAIT) >= 0 && zmq_msg_more(&x))
                        ;
                    zmq_msg_close(&x);
                }
                handle(s, zmq_msg_data(&m), zmq_msg_size(&m), more);
            }
            zmq_msg_close(&m);
        }
        if (now_ms() >= deadline)
            return 0;
    }
}

int cari_slave_run(cari_slave_t *s, volatile int *stop)
{
    while (!__atomic_load_n(stop, __ATOMIC_ACQUIRE))
        if (cari_slave_poll(s, 100) < 0)
            return -1;
    return 0;
}

/* ---------------------------------------------------------------- queries */

uint8_t cari_slave_tx_state(cari_slave_t *s, uint8_t sub)
{
    if (sub >= s->nsubs)
        return 0;
    pthread_mutex_lock(&s->mtx);
    uint8_t r = (uint8_t)s->subs[sub].tx_state;
    pthread_mutex_unlock(&s->mtx);
    return r;
}

bool cari_slave_rx_running(cari_slave_t *s, uint8_t sub)
{
    if (sub >= s->nsubs)
        return false;
    pthread_mutex_lock(&s->mtx);
    bool r = s->subs[sub].rx_running;
    pthread_mutex_unlock(&s->mtx);
    return r;
}

const cari_caps_t *cari_slave_sub_caps(cari_slave_t *s, uint8_t sub)
{
    return sub < s->nsubs ? &s->subs[sub].caps : NULL;
}

void cari_slave_stats(cari_slave_t *s, uint8_t sub, cari_slave_stats_t *st)
{
    memset(st, 0, sizeof *st);
    if (sub >= s->nsubs)
        return;
    const cari_slave_stats_t *x = &s->subs[sub].st;
    st->dl_msgs    = __atomic_load_n(&x->dl_msgs, __ATOMIC_RELAXED);
    st->dl_dropped = __atomic_load_n(&x->dl_dropped, __ATOMIC_RELAXED);
    st->dl_samples = __atomic_load_n(&x->dl_samples, __ATOMIC_RELAXED);
    st->ul_msgs    = __atomic_load_n(&x->ul_msgs, __ATOMIC_RELAXED);
    st->ul_samples = __atomic_load_n(&x->ul_samples, __ATOMIC_RELAXED);
    st->ul_bad     = __atomic_load_n(&x->ul_bad, __ATOMIC_RELAXED);
    st->trips      = __atomic_load_n(&x->trips, __ATOMIC_RELAXED);
    st->ends       = __atomic_load_n(&x->ends, __ATOMIC_RELAXED);
}

/* ---------------------------------------------------------------- data plane */

int cari_slave_dl_write(cari_slave_t *s, uint8_t idx, const void *samples, size_t n)
{
    if (idx >= s->nsubs)
        return -1;
    sub_t *sb = &s->subs[idx];
    if (!sb->dl_sock || !n)
        return -1;
    const void *msg = samples;
    size_t len = n * sb->dl_bps;
#ifdef CARI_HAVE_FLAC
    if (sb->dl_comp) {
        if (!sb->enc || cari_flac_enc_push(sb->enc, samples, n) < 0)
            return -1;
        msg = cari_flac_enc_take(sb->enc, &len);
        STAT_ADD(sb, dl_samples, n);
        if (!msg)
            return 1;                   /* block not complete yet */
    } else
#endif
    {
        STAT_ADD(sb, dl_samples, n);
    }
    if (zmq_send(sb->dl_sock, msg, len, ZMQ_DONTWAIT) < 0) {
        if (errno == EAGAIN) {
            STAT_ADD(sb, dl_dropped, 1);
            return 1;
        }
        return -1;
    }
    STAT_ADD(sb, dl_msgs, 1);
    return 0;
}

int cari_slave_ul_read(cari_slave_t *s, uint8_t idx, const void **samples, size_t *n, int timeout_ms)
{
    if (idx >= s->nsubs)
        return -1;
    sub_t *sb = &s->subs[idx];
    if (!sb->ul_sock)
        return -1;
    if (sb->ul_msg_valid) {
        zmq_msg_close(&sb->ul_msg);
        sb->ul_msg_valid = false;
    }
    /* After the end marker the socket is not read until the next start. */
    if (__atomic_load_n(&sb->ul_end, __ATOMIC_SEQ_CST))
        return CARI_UL_END;
    zmq_pollitem_t it = { .socket = sb->ul_sock, .events = ZMQ_POLLIN };
    int r = zmq_poll(&it, 1, timeout_ms);
    if (r < 0)
        return errno == EINTR ? 0 : -1;
    if (r == 0)
        return 0;

    zmq_msg_init(&sb->ul_msg);
    if (zmq_msg_recv(&sb->ul_msg, sb->ul_sock, ZMQ_DONTWAIT) < 0) {
        zmq_msg_close(&sb->ul_msg);
        return errno == EAGAIN ? 0 : -1;
    }
    sb->ul_msg_valid = true;
    STAT_ADD(sb, ul_msgs, 1);
    if (zmq_msg_more(&sb->ul_msg)) {    /* a CARI message is a single part */
        zmq_msg_t x;
        zmq_msg_init(&x);
        while (zmq_msg_recv(&x, sb->ul_sock, ZMQ_DONTWAIT) >= 0 && zmq_msg_more(&x))
            ;
        zmq_msg_close(&x);
        STAT_ADD(sb, ul_bad, 1);
    }
    const uint8_t *d = zmq_msg_data(&sb->ul_msg);
    size_t len = zmq_msg_size(&sb->ul_msg);
    if (len == 0) {                     /* end of transmission (CARI 2.1), also with FLAC */
        __atomic_store_n(&sb->ul_end, 1, __ATOMIC_SEQ_CST);
        cari_log(CARI_LOG_DEBUG, "sub %u: end of UL stream", idx);
        return CARI_UL_END;
    }
#ifdef CARI_HAVE_FLAC
    if (sb->ul_comp) {
        if (cari_flac_dec_feed(sb->dec, d, len) < 0)
            STAT_ADD(sb, ul_bad, 1);
        size_t out;
        const uint8_t *p = cari_flac_dec_take(sb->dec, &out);
        if (!p || out < sb->ul_bps)
            return 0;
        *samples = p;
        *n = out / sb->ul_bps;
        STAT_ADD(sb, ul_samples, *n);
        return 1;
    }
#endif
    if (len % sb->ul_bps)
        STAT_ADD(sb, ul_bad, 1);        /* partial sample: dropped */
    if (len < sb->ul_bps)
        return 0;
    *samples = d;
    *n = len / sb->ul_bps;
    STAT_ADD(sb, ul_samples, *n);
    return 1;
}

void cari_slave_ul_underrun(cari_slave_t *s, uint8_t sub)
{
    if (sub < s->nsubs)
        __atomic_store_n(&s->subs[sub].pending_underrun, 1, __ATOMIC_SEQ_CST);
}

void cari_slave_ul_finished(cari_slave_t *s, uint8_t sub)
{
    if (sub < s->nsubs)
        __atomic_store_n(&s->subs[sub].pending_finished, 1, __ATOMIC_SEQ_CST);
}

bool cari_slave_ul_ended(cari_slave_t *s, uint8_t sub)
{
    return sub < s->nsubs && __atomic_load_n(&s->subs[sub].ul_end, __ATOMIC_SEQ_CST);
}

/* ---------------------------------------------------------------- free */

void cari_slave_free(cari_slave_t *s)
{
    if (!s)
        return;
    if (s->subs) {
        shutdown_all(s);
        for (unsigned i = 0; i < s->nsubs; i++)
            free(s->subs[i].caps_tlv);
        free(s->subs);
    }
    close_sock(&s->ctrl);
    if (s->own_ctx && s->ctx)
        zmq_ctx_term(s->ctx);
    pthread_mutex_destroy(&s->mtx);
    free(s->ident);
    free(s->dev_caps_tlv);
    free(s->ctrl_ep);
    free(s->bind_host);
    free(s);
}
