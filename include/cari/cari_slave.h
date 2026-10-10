/*
 * libcari - CARI 2.1 slave (RRU side) over ZeroMQ
 *
 * The library owns everything the specification defines, so every slave
 * built on it behaves the same:
 *   - CTRL REP socket, one reply per message, reply rules and status codes
 *   - registers 0x00/0x01, IDENT, capability lists, supervision list
 *   - parameter rules (capability checks, ranges, read-only, stream locks)
 *   - DL PUB / UL PULL / SPVN PUB sockets: bind, rebind, close, port 0
 *   - stream start/stop rules and the TX state machine with underrun trip
 *   - supervision publishing at the configured period
 *   - FLAC compression of DL and decompression of UL (if built with libFLAC)
 *
 * The device supplies a description (capability TLVs etc.) and a set of
 * callbacks (cari_slave_ops_t) that touch the hardware.
 *
 * Threading model
 *   cari_slave_poll()/cari_slave_run() and all callbacks run in one thread
 *   (the "CTRL thread"). Streams are expected to be pumped by device threads:
 *   between the stream_start and stream_stop callbacks of a subdevice and
 *   direction, exactly one device thread may call cari_slave_dl_write() (DL)
 *   or cari_slave_ul_read() (UL) for it. stream_stop must not return before
 *   that thread has stopped using these functions. cari_slave_ul_underrun(),
 *   cari_slave_ul_finished()
 *   and cari_slave_param_get() may be called from any thread.
 */
#ifndef CARI_SLAVE_H
#define CARI_SLAVE_H

#include "cari_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cari_slave cari_slave_t;

typedef enum {
    CARI_DIR_DL = 0,    /* reception: slave -> master (PUB) */
    CARI_DIR_UL = 1     /* transmission: master -> slave (PULL) */
} cari_dir_t;

typedef struct {
    const uint8_t *caps;        /* capability TLV list (Table 14) */
    size_t         caps_len;
} cari_subdev_desc_t;

typedef struct {
    const char               *ident;        /* UTF-8 IDENT string */
    const uint8_t            *dev_caps;     /* device capability TLV list (Table 13) */
    size_t                    dev_caps_len;
    const uint8_t            *quantities;   /* supported supervision quantities */
    size_t                    nquantities;
    uint8_t                   nsubdevs;
    const cari_subdev_desc_t *subdevs;

    /* Optional (0/NULL selects the default) */
    void       *zmq_ctx;            /* share a ZMQ context; default: own one */
    const char *ctrl_endpoint;      /* default tcp://<any>:17002 (all interfaces) */
    const char *bind_host;          /* data planes, default "*" (all interfaces) */
    int         dl_sndhwm;          /* default 16 messages */
    int         ul_rcvhwm;          /* default 2 messages */
    int         ul_rcvbuf;          /* default 0: OS default */
    int         spvn_sndhwm;        /* default 8 */
    uint32_t    flac_blocksize;     /* default: sample rate / 100 (10 ms) */
    int         flac_level;         /* default 5 */
} cari_slave_desc_t;

/*
 * Device callbacks. All are optional; all run in the CTRL thread.
 * "int" callbacks return a CARI status (CARI_OK or 1..255).
 */
typedef struct {
    /* A parameter write that passed the generic rules. The device may adjust
     * (quantize) *value; the adjusted value is what CID 0x83 reads back. */
    int  (*set_param)(void *user, uint8_t sub, uint8_t param, cari_value_t *value);
    /* Optional live read. Return CARI_OK to override the cached value, a
     * negative number to use the cache. */
    int  (*get_param)(void *user, uint8_t sub, uint8_t param, cari_value_t *value);
    /* Start pumping the stream. The data socket is bound and any codec is
     * set up before this is called. Non-OK refuses the start. */
    int  (*stream_start)(void *user, uint8_t sub, cari_dir_t dir);
    /* Stop pumping; must not return while the pump thread still uses the
     * data-plane functions. Called for TX stop, trip, resets and shutdown.
     * For UL the device ramps down / unkeys (unless already done by a trip). */
    void (*stream_stop)(void *user, uint8_t sub, cari_dir_t dir);
    /* Subdevice reset (action 0x00). Streams are already stopped. */
    int  (*sub_reset)(void *user, uint8_t sub);
    /* Device reset (write to register 0x00). Called after the reply was sent
     * and after all streams were stopped and all sockets closed. */
    void (*dev_reset)(void *user);
    /* User-defined registers 0x02..0xFF. Return CARI_S_INVALID_ADDR for
     * unimplemented, CARI_S_READ_ONLY for read-only registers. */
    int  (*reg_read)(void *user, uint8_t reg, uint8_t *val);
    int  (*reg_write)(void *user, uint8_t reg, uint8_t val);
    /* Device error flags (Table 10). The library adds CARI_EF_UL_TRIP. */
    uint32_t (*error_flags)(void *user);
    /* Supervision measurement. sub is 0xFF for device-level quantities.
     * Return CARI_OK to publish the value, anything else to skip it. */
    int  (*spvn_read)(void *user, uint8_t qid, uint8_t sub, float *val);
} cari_slave_ops_t;

typedef struct {
    uint64_t dl_msgs;       /* DL messages sent */
    uint64_t dl_dropped;    /* DL messages dropped (HWM reached) */
    uint64_t dl_samples;
    uint64_t ul_msgs;       /* UL messages received */
    uint64_t ul_samples;
    uint64_t ul_bad;        /* UL messages with a partial sample / bad frames */
    uint64_t trips;         /* UL underrun trips */
    uint64_t ends;          /* transmissions ended by the UL end marker */
} cari_slave_stats_t;

cari_slave_t *cari_slave_new(const cari_slave_desc_t *desc, const cari_slave_ops_t *ops, void *user);
/* Binds the CTRL socket. Returns 0 or -1. */
int  cari_slave_start(cari_slave_t *s);
/* Stops all streams (stream_stop callbacks), closes sockets, frees. */
void cari_slave_free(cari_slave_t *s);

/* Declare a parameter as supported and set its initial value (CTRL thread,
 * typically before cari_slave_start or from a reset callback). TX state
 * (0x0E) is handled by the library and must not be declared. */
int  cari_slave_param_init(cari_slave_t *s, uint8_t sub, uint8_t param, cari_value_t v);
/* Cached parameter value. Thread-safe. Returns 0, or -1 if unsupported. */
int  cari_slave_param_get(cari_slave_t *s, uint8_t sub, uint8_t param, cari_value_t *v);

/* Handle CTRL messages, pending trips and supervision publishing for up to
 * timeout_ms. Returns 0, or -1 on a fatal socket error. */
int  cari_slave_poll(cari_slave_t *s, int timeout_ms);
/* Loop on cari_slave_poll() until *stop becomes non-zero. */
int  cari_slave_run(cari_slave_t *s, volatile int *stop);

uint8_t cari_slave_tx_state(cari_slave_t *s, uint8_t sub);
bool    cari_slave_rx_running(cari_slave_t *s, uint8_t sub);
const cari_caps_t *cari_slave_sub_caps(cari_slave_t *s, uint8_t sub);
void    cari_slave_stats(cari_slave_t *s, uint8_t sub, cari_slave_stats_t *st);

/* ---- data plane (stream threads) ---- */

/* Publish n DL samples in the stream's signal format (compressed if
 * enabled). Never blocks: if the HWM is reached the message is dropped.
 * Returns 0 (sent), 1 (dropped / still buffered in the encoder), -1 error. */
int  cari_slave_dl_write(cari_slave_t *s, uint8_t sub, const void *samples, size_t n);

#define CARI_UL_END 2

/* Receive one UL message (decompressed if enabled). Call it only as fast as
 * samples are transmitted - that is what applies backpressure to the
 * master. Returns 1 with samples and n set (n > 0, valid until the next call
 * or stream stop), 0 on timeout, -1 on error, or CARI_UL_END when the
 * zero-byte end-of-transmission marker has been read (CARI 2.1). After
 * CARI_UL_END the socket is not read again until the next stream start; the
 * device plays out what it has buffered, ramps down, unkeys and then calls
 * cari_slave_ul_finished(). Until then the TX state stays "transmitting" and
 * the library refuses a TX start or a UL rebind with status 8. */
int  cari_slave_ul_read(cari_slave_t *s, uint8_t sub, const void **samples, size_t *n, int timeout_ms);

/* The play-out after the end marker is complete (output ramped down and
 * unkeyed). The library returns the subdevice to idle from the CTRL thread
 * (calling stream_stop for UL). Safe to call from any thread. */
void cari_slave_ul_finished(cari_slave_t *s, uint8_t sub);

/* True once the end marker has been read in the current transmission. */
bool cari_slave_ul_ended(cari_slave_t *s, uint8_t sub);

/* Report a UL underrun: the device has already ramped the output down over
 * the remaining samples and unkeyed. The library moves the subdevice to the
 * tripped state from the CTRL thread (calling stream_stop for UL) and sets
 * error flag bit 4. Safe to call from any thread. */
void cari_slave_ul_underrun(cari_slave_t *s, uint8_t sub);

#ifdef __cplusplus
}
#endif
#endif /* CARI_SLAVE_H */
