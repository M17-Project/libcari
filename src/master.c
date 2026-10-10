/*
 * libcari - CARI 2.1 master over ZeroMQ
 */
#include "cari/cari_master.h"
#include "cari/cari_log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zmq.h>

struct cari_master {
    void     *ctx;
    bool      own_ctx;
    void     *req;
    char      host[256];
    uint16_t  port;
    char      ep[300];
    int       timeout_ms;
    int       retries;
    unsigned  reconnects;
    uint8_t   cmd[CARI_MAX_FRAME];
    uint8_t   rep[CARI_MAX_FRAME];
};

const char *cari_strerror(int rc)
{
    switch (rc) {
    case CARI_E_IO:      return "socket error";
    case CARI_E_TIMEOUT: return "timeout (no reply)";
    case CARI_E_PROTO:   return "invalid reply";
    case CARI_E_ARG:     return "invalid argument";
    case CARI_E_NOMEM:   return "out of memory";
    default:             return cari_status_str(rc);
    }
}

static int parse_addr(const char *addr, char *host, size_t hcap, uint16_t *port)
{
    const char *p = addr;
    *port = CARI_DEFAULT_CTRL_PORT;
    if (!strncmp(p, "tcp://", 6))
        p += 6;
    const char *colon;
    if (*p == '[') {                    /* [v6]:port */
        const char *end = strchr(p, ']');
        if (!end)
            return -1;
        if ((size_t)(end - p + 1) >= hcap)
            return -1;
        memcpy(host, p, (size_t)(end - p + 1));
        host[end - p + 1] = 0;
        colon = end[1] == ':' ? end + 1 : NULL;
    } else {
        colon = strrchr(p, ':');
        size_t hl = colon ? (size_t)(colon - p) : strlen(p);
        if (!hl || hl >= hcap)
            return -1;
        memcpy(host, p, hl);
        host[hl] = 0;
    }
    if (colon) {
        char *e;
        long v = strtol(colon + 1, &e, 10);
        if (*e || v <= 0 || v > 65535)
            return -1;
        *port = (uint16_t)v;
    }
    return 0;
}

static void set_int(void *z, int opt, int v)
{
    zmq_setsockopt(z, opt, &v, sizeof v);
}

static int open_req(cari_master_t *m)
{
    m->req = zmq_socket(m->ctx, ZMQ_REQ);
    if (!m->req)
        return -1;
    set_int(m->req, ZMQ_LINGER, 0);
    if (m->host[0] == '[')
        set_int(m->req, ZMQ_IPV6, 1);
    if (zmq_connect(m->req, m->ep) != 0) {
        zmq_close(m->req);
        m->req = NULL;
        return -1;
    }
    return 0;
}

static void reopen_req(cari_master_t *m)
{
    if (m->req)
        zmq_close(m->req);
    m->req = NULL;
    m->reconnects++;
    open_req(m);
}

cari_master_t *cari_master_new(void *zmq_ctx, const char *addr, int timeout_ms)
{
    cari_master_t *m = calloc(1, sizeof *m);
    if (!m)
        return NULL;
    if (parse_addr(addr, m->host, sizeof m->host, &m->port) < 0) {
        cari_log(CARI_LOG_ERROR, "bad address '%s'", addr);
        free(m);
        return NULL;
    }
    snprintf(m->ep, sizeof m->ep, "tcp://%s:%u", m->host, m->port);
    m->timeout_ms = timeout_ms > 0 ? timeout_ms : 1000;
    m->ctx = zmq_ctx ? zmq_ctx : zmq_ctx_new();
    m->own_ctx = !zmq_ctx;
    if (!m->ctx || open_req(m) < 0) {
        cari_master_free(m);
        return NULL;
    }
    return m;
}

void cari_master_free(cari_master_t *m)
{
    if (!m)
        return;
    if (m->req)
        zmq_close(m->req);
    if (m->own_ctx && m->ctx)
        zmq_ctx_term(m->ctx);
    free(m);
}

void cari_master_set_timeout(cari_master_t *m, int t) { m->timeout_ms = t > 0 ? t : 1; }
void cari_master_set_retries(cari_master_t *m, int r) { m->retries = r < 0 ? 0 : r; }
const char *cari_master_host(const cari_master_t *m) { return m->host; }
uint16_t cari_master_ctrl_port(const cari_master_t *m) { return m->port; }
void *cari_master_zmq_ctx(cari_master_t *m) { return m->ctx; }
unsigned cari_master_reconnects(const cari_master_t *m) { return m->reconnects; }

static int transact_once(cari_master_t *m, const uint8_t *cmd, size_t len, cari_reply_t *r)
{
    if (!m->req && open_req(m) < 0)
        return CARI_E_IO;
    if (zmq_send(m->req, cmd, len, 0) < 0) {
        reopen_req(m);
        return CARI_E_IO;
    }
    zmq_pollitem_t it = { .socket = m->req, .events = ZMQ_POLLIN };
    int pr;
    do {
        pr = zmq_poll(&it, 1, m->timeout_ms);
    } while (pr < 0 && errno == EINTR);
    if (pr <= 0) {
        reopen_req(m);          /* REQ is stuck waiting for a reply */
        return pr == 0 ? CARI_E_TIMEOUT : CARI_E_IO;
    }
    int n = zmq_recv(m->req, m->rep, sizeof m->rep, ZMQ_DONTWAIT);
    if (n < 0) {
        reopen_req(m);
        return CARI_E_IO;
    }
    if ((size_t)n > sizeof m->rep)
        return CARI_E_PROTO;    /* truncated: cannot be a valid CARI frame */
    if (cari_reply_validate(cmd, len, m->rep, (size_t)n, r) < 0)
        return CARI_E_PROTO;
    return r->status;
}

int cari_master_transact(cari_master_t *m, const uint8_t *cmd, size_t len, cari_reply_t *r)
{
    cari_reply_t dummy;
    if (!r)
        r = &dummy;
    if (len < CARI_HDR_LEN || len > CARI_MAX_FRAME)
        return CARI_E_ARG;
    int rc = CARI_E_TIMEOUT;
    for (int a = 0; a <= m->retries; a++) {
        rc = transact_once(m, cmd, len, r);
        if (rc != CARI_E_TIMEOUT)
            break;
        cari_log(CARI_LOG_DEBUG, "CID 0x%02X: no reply from %s (attempt %d)", cmd[0], m->ep, a + 1);
    }
    return rc;
}

/* ---------------------------------------------------------------- helpers */

#define DO(enc_call)                                   \
    int n_ = (enc_call);                               \
    if (n_ < 0)                                        \
        return CARI_E_ARG;                             \
    cari_reply_t r_;                                   \
    int rc_ = cari_master_transact(m, m->cmd, (size_t)n_, &r_)

int cari_m_ping(cari_master_t *m, uint32_t *flags)
{
    DO(cari_enc_ping(m->cmd, sizeof m->cmd));
    if (rc_ == CARI_OK && flags)
        *flags = cari_get_le32(r_.data);
    return rc_;
}

int cari_m_get_ident(cari_master_t *m, char *buf, size_t cap)
{
    DO(cari_enc_get_ident(m->cmd, sizeof m->cmd));
    if (rc_ == CARI_OK && cap) {
        size_t l = r_.data_len < cap - 1 ? r_.data_len : cap - 1;
        if (l)
            memcpy(buf, r_.data, l);
        buf[l] = 0;
    }
    return rc_;
}

int cari_m_get_reg(cari_master_t *m, uint8_t reg, uint8_t *val)
{
    DO(cari_enc_get_reg(m->cmd, sizeof m->cmd, reg));
    if (rc_ == CARI_OK && val)
        *val = r_.data[0];
    return rc_;
}

int cari_m_set_reg(cari_master_t *m, uint8_t reg, uint8_t val)
{
    DO(cari_enc_set_reg(m->cmd, sizeof m->cmd, reg, val));
    return rc_;
}

int cari_m_reset_device(cari_master_t *m)
{
    return cari_m_set_reg(m, CARI_REG_VERSION, 0);
}

int cari_m_set_param(cari_master_t *m, uint8_t sub, uint8_t param, const cari_value_t *v)
{
    const cari_param_info_t *pi = cari_param_info(param);
    if (pi && v->type != pi->type)
        return CARI_E_ARG;
    DO(cari_enc_set_param(m->cmd, sizeof m->cmd, sub, param, v));
    return rc_;
}

int cari_m_get_param(cari_master_t *m, uint8_t sub, uint8_t param, cari_value_t *v)
{
    const cari_param_info_t *pi = cari_param_info(param);
    if (!pi)
        return CARI_E_ARG;      /* reply size would be unknown */
    DO(cari_enc_get_param(m->cmd, sizeof m->cmd, sub, param));
    if (rc_ == CARI_OK && v && cari_value_decode(pi->type, r_.data, r_.data_len, v) < 0)
        return CARI_E_PROTO;
    return rc_;
}

int cari_m_exec(cari_master_t *m, uint8_t sub, uint8_t action)
{
    DO(cari_enc_exec(m->cmd, sizeof m->cmd, sub, action));
    return rc_;
}

int cari_m_bind_ul(cari_master_t *m, uint8_t sub, uint16_t port)
{
    DO(cari_enc_bind(m->cmd, sizeof m->cmd, CARI_CID_BIND_UL, sub, port));
    return rc_;
}

int cari_m_bind_dl(cari_master_t *m, uint8_t sub, uint16_t port)
{
    DO(cari_enc_bind(m->cmd, sizeof m->cmd, CARI_CID_BIND_DL, sub, port));
    return rc_;
}

int cari_m_bind_spvn(cari_master_t *m, uint16_t port, uint16_t period_ms, const uint8_t *qids, size_t nq)
{
    DO(cari_enc_bind_spvn(m->cmd, sizeof m->cmd, port, period_ms, qids, nq));
    return rc_;
}

int cari_m_get_sub_caps(cari_master_t *m, uint8_t sub, cari_caps_t *caps)
{
    DO(cari_enc_get_sub_caps(m->cmd, sizeof m->cmd, sub));
    if (rc_ == CARI_OK && caps && cari_caps_parse(r_.data, r_.data_len, true, caps) < 0)
        return CARI_E_PROTO;
    return rc_;
}

int cari_m_get_dev_caps(cari_master_t *m, cari_caps_t *caps)
{
    DO(cari_enc_get_dev_caps(m->cmd, sizeof m->cmd));
    if (rc_ == CARI_OK && caps && cari_caps_parse(r_.data, r_.data_len, false, caps) < 0)
        return CARI_E_PROTO;
    return rc_;
}

int cari_m_get_spvn_list(cari_master_t *m, uint8_t *qids, size_t cap, size_t *n)
{
    DO(cari_enc_get_spvn_list(m->cmd, sizeof m->cmd));
    if (rc_ == CARI_OK) {
        size_t c = r_.data_len < cap ? r_.data_len : cap;
        if (c)
            memcpy(qids, r_.data, c);
        if (n)
            *n = r_.data_len;
    }
    return rc_;
}

/* ---------------------------------------------------------------- data planes */

static void *new_sock(cari_master_t *m, int type)
{
    void *z = zmq_socket(m->ctx, type);
    if (!z)
        return NULL;
    set_int(z, ZMQ_LINGER, 0);
    if (m->host[0] == '[')
        set_int(z, ZMQ_IPV6, 1);
    return z;
}

static void *finish_connect(cari_master_t *m, void *z, uint16_t port)
{
    char ep[300];
    snprintf(ep, sizeof ep, "tcp://%s:%u", m->host, port);
    if (zmq_connect(z, ep) != 0) {
        zmq_close(z);
        return NULL;
    }
    return z;
}

void *cari_master_connect_dl(cari_master_t *m, uint16_t port, int rcvhwm)
{
    void *z = new_sock(m, ZMQ_SUB);
    if (!z)
        return NULL;
    if (rcvhwm > 0)
        set_int(z, ZMQ_RCVHWM, rcvhwm);
    zmq_setsockopt(z, ZMQ_SUBSCRIBE, "", 0);    /* empty filter: everything */
    return finish_connect(m, z, port);
}

void *cari_master_connect_ul(cari_master_t *m, uint16_t port, int sndhwm, int sndbuf)
{
    void *z = new_sock(m, ZMQ_PUSH);
    if (!z)
        return NULL;
    if (sndhwm > 0)
        set_int(z, ZMQ_SNDHWM, sndhwm);
    if (sndbuf > 0)
        set_int(z, ZMQ_SNDBUF, sndbuf);
    return finish_connect(m, z, port);
}

void *cari_master_connect_spvn(cari_master_t *m, uint16_t port)
{
    void *z = new_sock(m, ZMQ_SUB);
    if (!z)
        return NULL;
    zmq_setsockopt(z, ZMQ_SUBSCRIBE, "", 0);
    return finish_connect(m, z, port);
}

/* ---------------------------------------------------------------- CARI 2.1 */

int cari_master_ul_send_end(void *ul_sock)
{
    return zmq_send(ul_sock, "", 0, 0) < 0 ? -1 : 0;
}

int cari_m_wait_tx_idle(cari_master_t *m, uint8_t sub, int timeout_ms)
{
    struct timespec t0, now, nap = { 0, 5 * 1000000L };
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        cari_value_t v;
        int rc = cari_m_get_param(m, sub, CARI_P_TX_STATE, &v);
        if (rc != CARI_OK)
            return rc;
        if (v.v.u8 == CARI_TX_IDLE)
            return CARI_OK;
        if (v.v.u8 == CARI_TX_TRIPPED)
            return CARI_S_INVALID_STATE;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000 >= timeout_ms)
            return CARI_E_TIMEOUT;
        nanosleep(&nap, NULL);
    }
}

int cari_m_supports(cari_master_t *m, unsigned major, unsigned minor, bool *yes)
{
    uint8_t v;
    int rc = cari_m_get_reg(m, CARI_REG_VERSION, &v);
    if (rc == CARI_OK)
        *yes = (unsigned)(v >> 4) == major && (unsigned)(v & 15) >= minor;
    return rc;
}
