/*
 * libcari - CARI 2.1 master (BBU side) over ZeroMQ
 *
 * One cari_master_t talks to one slave's CTRL endpoint through a REQ
 * socket. Every command waits for its reply up to a timeout; on a timeout
 * the REQ socket is closed and reopened (the spec's recommendation), so the
 * next command can be sent. Optionally the command is retried.
 *
 * Return values of the command functions:
 *   >= 0  the slave's reply status (CARI_OK = 0, or Table 9 code)
 *   <  0  a local error (CARI_E_*), the reply could not be obtained
 */
#ifndef CARI_MASTER_H
#define CARI_MASTER_H

#include "cari_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CARI_E_IO       (-1)    /* socket error */
#define CARI_E_TIMEOUT  (-2)    /* no reply within the timeout */
#define CARI_E_PROTO    (-3)    /* reply malformed or not matching the command */
#define CARI_E_ARG      (-4)    /* bad argument / encoding failed */
#define CARI_E_NOMEM    (-5)

typedef struct cari_master cari_master_t;

/* addr: "host", "host:port", "tcp://host:port" or "[v6addr]:port". The CTRL
 * port defaults to 17002. zmq_ctx may be NULL (own context). */
cari_master_t *cari_master_new(void *zmq_ctx, const char *addr, int timeout_ms);
void           cari_master_free(cari_master_t *m);
void           cari_master_set_timeout(cari_master_t *m, int timeout_ms);
/* Number of extra attempts after a timeout (default 0). Only enable for
 * commands that are safe to repeat. */
void           cari_master_set_retries(cari_master_t *m, int retries);
const char    *cari_master_host(const cari_master_t *m);
uint16_t       cari_master_ctrl_port(const cari_master_t *m);
void          *cari_master_zmq_ctx(cari_master_t *m);
/* Number of times the REQ socket was reopened after a timeout. */
unsigned       cari_master_reconnects(const cari_master_t *m);
/* Text for a status or CARI_E_* value. */
const char    *cari_strerror(int rc);

/* Send an encoded command and wait for the validated reply. r->data points
 * into an internal buffer valid until the next call. */
int cari_master_transact(cari_master_t *m, const uint8_t *cmd, size_t len, cari_reply_t *r);

/* ---- command helpers ---- */
int cari_m_ping(cari_master_t *m, uint32_t *flags);
int cari_m_get_ident(cari_master_t *m, char *buf, size_t cap);          /* NUL-terminated */
int cari_m_get_reg(cari_master_t *m, uint8_t reg, uint8_t *val);
int cari_m_set_reg(cari_master_t *m, uint8_t reg, uint8_t val);
int cari_m_reset_device(cari_master_t *m);                              /* write reg 0x00 */
int cari_m_set_param(cari_master_t *m, uint8_t sub, uint8_t param, const cari_value_t *v);
int cari_m_get_param(cari_master_t *m, uint8_t sub, uint8_t param, cari_value_t *v);
int cari_m_exec(cari_master_t *m, uint8_t sub, uint8_t action);
int cari_m_bind_ul(cari_master_t *m, uint8_t sub, uint16_t port);
int cari_m_bind_dl(cari_master_t *m, uint8_t sub, uint16_t port);
int cari_m_bind_spvn(cari_master_t *m, uint16_t port, uint16_t period_ms, const uint8_t *qids, size_t nq);
int cari_m_get_sub_caps(cari_master_t *m, uint8_t sub, cari_caps_t *caps);
int cari_m_get_dev_caps(cari_master_t *m, cari_caps_t *caps);
int cari_m_get_spvn_list(cari_master_t *m, uint8_t *qids, size_t cap, size_t *n);

/* ---- data planes: connect the master's counterpart sockets ----
 * The socket is created in the master's ZMQ context and connected to the
 * slave's host. The caller owns it (zmq_close). */
void *cari_master_connect_dl(cari_master_t *m, uint16_t port, int rcvhwm);    /* SUB, all msgs */
void *cari_master_connect_ul(cari_master_t *m, uint16_t port, int sndhwm, int sndbuf); /* PUSH */
void *cari_master_connect_spvn(cari_master_t *m, uint16_t port);               /* SUB, all msgs */

/* ---- end of transmission (CARI 2.1) ----
 * Send the zero-byte end marker on a UL PUSH socket (after the last
 * samples). Returns 0 or -1 (errno from zmq_send). */
int cari_master_ul_send_end(void *ul_sock);
/* Poll the TX state until it is idle (0), or timeout_ms passes. Returns
 * CARI_OK when idle, CARI_E_TIMEOUT, CARI_S_INVALID_STATE if the
 * subdevice tripped, or another error. */
int cari_m_wait_tx_idle(cari_master_t *m, uint8_t sub, int timeout_ms);
/* Does the slave implement at least CARI major.minor (register 0x00)? */
int cari_m_supports(cari_master_t *m, unsigned major, unsigned minor, bool *yes);

#ifdef __cplusplus
}
#endif
#endif /* CARI_MASTER_H */
