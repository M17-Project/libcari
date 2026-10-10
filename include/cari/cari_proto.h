/*
 * libcari - Common Amateur Radio Interface (CARI 2.1)
 *
 * cari_proto.h - socket-free codec layer
 *
 * Everything in this header is pure data manipulation: no sockets, no
 * threads, no allocation. It can be used on its own (e.g. on a
 * microcontroller, or in a GNU Radio block) and is what the ZMQ slave and
 * master layers are built on.
 *
 * Spec: M17-Project/CARI, docs/commands.md, revision 2.1.
 */
#ifndef CARI_PROTO_H
#define CARI_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Protocol constants                                                       */
/* ------------------------------------------------------------------------ */

#define CARI_VERSION_MAJOR      2
#define CARI_VERSION_MINOR      1
#define CARI_VERSION_BYTE       ((CARI_VERSION_MAJOR << 4) | CARI_VERSION_MINOR) /* 0x21 */

#define CARI_HDR_LEN            3       /* CID + 16-bit byte count */
#define CARI_MAX_FRAME          65535   /* byte count field is 16-bit */
#define CARI_ERR_REPLY_LEN      4       /* CID, count, status */
#define CARI_ADDR_RESERVED      0xFF
#define CARI_MAX_SUBDEVICES     255
#define CARI_DEFAULT_CTRL_PORT  17002

/* Command identifiers (Tables 7 and 8) */
typedef enum {
    CARI_CID_PING           = 0x00,
    CARI_CID_SET_REG        = 0x01,
    CARI_CID_SET_PARAM      = 0x02,
    CARI_CID_EXEC           = 0x03,
    CARI_CID_BIND_UL        = 0x04,
    CARI_CID_BIND_DL        = 0x05,
    CARI_CID_BIND_SPVN      = 0x06,

    CARI_CID_GET_IDENT      = 0x80,
    CARI_CID_GET_REG        = 0x81,
    CARI_CID_GET_SUB_CAPS   = 0x82,
    CARI_CID_GET_PARAM      = 0x83,
    CARI_CID_GET_SPVN_LIST  = 0x84,
    CARI_CID_GET_DEV_CAPS   = 0x85
} cari_cid_t;

#define CARI_CID_IS_READ(cid)   (((cid) & 0x80) != 0)

/* Reply status codes (Table 9) */
typedef enum {
    CARI_OK                 = 0,
    CARI_S_MALFORMED        = 1,
    CARI_S_UNSUPPORTED      = 2,
    CARI_S_BIND_FAILED      = 3,
    CARI_S_RESERVED_4       = 4,
    CARI_S_OUT_OF_RANGE     = 5,
    CARI_S_INVALID_ADDR     = 6,
    CARI_S_READ_ONLY        = 7,
    CARI_S_INVALID_STATE    = 8
} cari_status_t;

/* Device error flags, Pong payload (Table 10) */
#define CARI_EF_PLL_LOCK        (1u << 0)
#define CARI_EF_SUBDEV_COMM     (1u << 1)
#define CARI_EF_TEMPERATURE     (1u << 2)
#define CARI_EF_FREQ_REF        (1u << 3)
#define CARI_EF_UL_TRIP         (1u << 4)

/* Registers (Table 11) */
#define CARI_REG_VERSION        0x00    /* R, write = device reset */
#define CARI_REG_NUM_SUBDEVS    0x01    /* R */
#define CARI_REG_USER_FIRST     0x02

/* Device capabilities (Table 13) */
#define CARI_DCAP_COMPRESSION   0x00
#define CARI_DCAP_SUPERVISION   0x01

/* Subdevice explicit capabilities (Table 14a) */
#define CARI_SCAP_RESET         0x00
#define CARI_SCAP_IQ            0x01
#define CARI_SCAP_RX            0x02
#define CARI_SCAP_TX            0x03
#define CARI_SCAP_FULL_DUPLEX   0x04
#define CARI_SCAP_AGC           0x05
#define CARI_SCAP_AFC           0x06
#define CARI_SCAP_FREQ_REF      0x07
#define CARI_SCAP_AM_DEMOD      0x08
#define CARI_SCAP_FM_DEMOD      0x09
#define CARI_SCAP_PM_DEMOD      0x0A
#define CARI_SCAP_USB_DEMOD     0x0B
#define CARI_SCAP_LSB_DEMOD     0x0C
#define CARI_SCAP_AM_MOD        0x0D
#define CARI_SCAP_FM_MOD        0x0E
#define CARI_SCAP_PM_MOD        0x0F
#define CARI_SCAP_USB_MOD       0x10
#define CARI_SCAP_LSB_MOD       0x11
#define CARI_SCAP_DC_CORR       0x12
#define CARI_SCAP_FMT_CF32      0x40
#define CARI_SCAP_FMT_CS16      0x41
#define CARI_SCAP_FMT_F32       0x42
#define CARI_SCAP_FMT_S16       0x43
#define CARI_SCAP_FMT_CS8       0x44
#define CARI_SCAP_FMT_S8        0x45
#define CARI_SCAP_FMT_CU8       0x46
#define CARI_SCAP_FMT_U8        0x47

/* Subdevice ranged capabilities (Table 14b) */
#define CARI_SCAP_RX_FREQ       0x80    /* u64, Hz */
#define CARI_SCAP_TX_FREQ       0x81    /* u64, Hz */
#define CARI_SCAP_LNA_GAIN      0x82    /* f32, dB */
#define CARI_SCAP_TX_POWER      0x83    /* f32, dBm */
#define CARI_SCAP_CH_WIDTH      0x84    /* f32, Hz */
#define CARI_SCAP_SAMPLE_RATE   0x85    /* f32, Hz */
#define CARI_SCAP_FM_DEV        0x86    /* f32, Hz */
#define CARI_SCAP_PM_DEV        0x87    /* f32, rad */

#define CARI_CAP_IS_RANGED(id)  (((id) & 0x80) != 0)

/* Subdevice parameters (Table 15) */
#define CARI_P_RX_FREQ          0x00
#define CARI_P_TX_FREQ          0x01
#define CARI_P_LNA_GAIN         0x02
#define CARI_P_TX_POWER         0x03
#define CARI_P_CH_WIDTH         0x04
#define CARI_P_SAMPLE_RATE      0x05
#define CARI_P_FREQ_CORR        0x06
#define CARI_P_AGC              0x07
#define CARI_P_AFC              0x08
#define CARI_P_DC_CORR          0x09
#define CARI_P_FORMAT           0x0A
#define CARI_P_MODE             0x0B
#define CARI_P_DEVIATION        0x0C
#define CARI_P_COMPRESSION      0x0D
#define CARI_P_TX_STATE         0x0E
#define CARI_P_COUNT            0x0F

/* Modulation modes (Table 16) */
#define CARI_MODE_IQ            0x00
#define CARI_MODE_AM            0x01
#define CARI_MODE_FM            0x02
#define CARI_MODE_PM            0x03
#define CARI_MODE_USB           0x04
#define CARI_MODE_LSB           0x05
#define CARI_MODE_COUNT         0x06

/* Subdevice actions (Table 17a) */
#define CARI_ACT_RESET          0x00
#define CARI_ACT_RX_START       0x01
#define CARI_ACT_RX_STOP        0x02
#define CARI_ACT_TX_START       0x03
#define CARI_ACT_TX_STOP        0x04
#define CARI_ACT_CLEAR_TRIP     0x05
#define CARI_ACT_COUNT          0x06

/* TX states (Table 17b) */
#define CARI_TX_IDLE            0
#define CARI_TX_TRANSMITTING    1
#define CARI_TX_TRIPPED         2

/* Signal formats (Table 18) - the ID equals the capability ID */
#define CARI_FMT_CF32           0x40
#define CARI_FMT_CS16           0x41
#define CARI_FMT_F32            0x42
#define CARI_FMT_S16            0x43
#define CARI_FMT_CS8            0x44
#define CARI_FMT_S8             0x45
#define CARI_FMT_CU8            0x46
#define CARI_FMT_U8             0x47

/* Supervision quantities (Table 20) */
#define CARI_Q_TEMPERATURE      0x00
#define CARI_Q_VOLTAGE          0x01
#define CARI_Q_CURRENT          0x02
#define CARI_Q_RETURN_LOSS      0x03    /* per subdevice */
#define CARI_Q_POWER_INCIDENT   0x04    /* per subdevice */
#define CARI_Q_POWER_REFLECTED  0x05    /* per subdevice */
#define CARI_Q_COUNT            0x06

/* ------------------------------------------------------------------------ */
/* Little-endian helpers                                                     */
/* ------------------------------------------------------------------------ */

static inline uint16_t cari_get_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static inline uint32_t cari_get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t cari_get_le64(const uint8_t *p)
{
    return (uint64_t)cari_get_le32(p) | ((uint64_t)cari_get_le32(p + 4) << 32);
}
static inline void cari_put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static inline void cari_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline void cari_put_le64(uint8_t *p, uint64_t v)
{
    cari_put_le32(p, (uint32_t)v); cari_put_le32(p + 4, (uint32_t)(v >> 32));
}
float    cari_get_f32(const uint8_t *p);
void     cari_put_f32(uint8_t *p, float v);

/* ------------------------------------------------------------------------ */
/* Typed values                                                              */
/* ------------------------------------------------------------------------ */

typedef enum {
    CARI_T_NONE = 0,
    CARI_T_U8,          /* 1 byte unsigned */
    CARI_T_BOOL,        /* 1 byte, 0 or 1 */
    CARI_T_U64,         /* 8 bytes unsigned */
    CARI_T_F32          /* 4 bytes IEEE-754 */
} cari_type_t;

typedef struct {
    cari_type_t type;
    union {
        uint8_t  u8;    /* CARI_T_U8, CARI_T_BOOL */
        uint64_t u64;
        float    f32;
    } v;
} cari_value_t;

size_t cari_type_size(cari_type_t t);
cari_value_t cari_value_u8(uint8_t v);
cari_value_t cari_value_bool(bool v);
cari_value_t cari_value_u64(uint64_t v);
cari_value_t cari_value_f32(float v);
/* Decode exactly cari_type_size(t) bytes. Returns 0, or -1 on size mismatch. */
int    cari_value_decode(cari_type_t t, const uint8_t *p, size_t n, cari_value_t *out);
/* Encodes into p (must hold cari_type_size bytes). Returns bytes written. */
size_t cari_value_encode(const cari_value_t *v, uint8_t *p);
/* Value as double (for range comparisons and printing). */
double cari_value_as_double(const cari_value_t *v);
/* Parse text into a value of type t ("433.475e6", "1", "on", "-3.5"). 0 or -1. */
int    cari_value_parse(cari_type_t t, const char *s, cari_value_t *out);
/* Format a value as text. Returns snprintf-style length. */
int    cari_value_format(const cari_value_t *v, char *buf, size_t cap);

/* ------------------------------------------------------------------------ */
/* Tables                                                                    */
/* ------------------------------------------------------------------------ */

#define CARI_PF_READONLY        0x01    /* writing returns status 7 */
#define CARI_PF_STREAM_LOCKED   0x02    /* not while DL/UL running (status 8) */

typedef struct {
    uint8_t     id;
    const char *key;        /* short CLI-friendly name, e.g. "rx_freq" */
    const char *name;       /* human readable */
    cari_type_t type;
    const char *unit;
    uint8_t     flags;      /* CARI_PF_* */
} cari_param_info_t;

typedef struct {
    uint8_t     id;
    const char *key;
    const char *name;
    bool        ranged;
    cari_type_t type;       /* CARI_T_NONE for explicit capabilities */
    const char *unit;
} cari_cap_info_t;

typedef struct {
    uint8_t     id;
    const char *key;
    const char *name;
} cari_enum_info_t;         /* actions, modes, device capabilities */

typedef struct {
    uint8_t     id;
    const char *key;
    const char *name;
    const char *unit;
    bool        per_subdevice;  /* value is prefixed with a subdevice byte */
} cari_quantity_info_t;

typedef struct {
    uint8_t     id;         /* == capability ID */
    const char *name;       /* "CS16" */
    bool        complex;
    bool        is_float;
    bool        is_signed;  /* false only for U8/CU8 */
    uint8_t     bits;       /* component size */
    uint8_t     channels;   /* 2 for complex */
    uint8_t     bytes;      /* bytes per sample (all components) */
} cari_format_info_t;

typedef struct {
    uint8_t     cid;
    const char *name;
    bool        has_addr;
    uint16_t    min_len;    /* minimum byte count */
    uint16_t    fixed_len;  /* exact byte count, 0 if variable */
} cari_cmd_info_t;

const cari_param_info_t    *cari_param_info(uint8_t id);
const cari_param_info_t    *cari_param_by_key(const char *key);
const cari_cap_info_t      *cari_subcap_info(uint8_t id);
const cari_cap_info_t      *cari_subcap_by_key(const char *key);
const cari_enum_info_t     *cari_devcap_info(uint8_t id);
const cari_enum_info_t     *cari_action_info(uint8_t id);
const cari_enum_info_t     *cari_action_by_key(const char *key);
const cari_enum_info_t     *cari_mode_info(uint8_t id);
const cari_enum_info_t     *cari_mode_by_key(const char *key);
const cari_quantity_info_t *cari_quantity_info(uint8_t id);
const cari_quantity_info_t *cari_quantity_by_key(const char *key);
const cari_format_info_t   *cari_format_info(uint8_t id);
const cari_format_info_t   *cari_format_by_name(const char *name);
const cari_cmd_info_t      *cari_cmd_info(uint8_t cid);
const char                 *cari_status_str(int status);
const char                 *cari_tx_state_str(uint8_t st);

/* Ranged capability that bounds a parameter (CARI_P_DEVIATION depends on the
 * mode: FM -> 0x86, PM -> 0x87). Returns -1 if none. */
int cari_param_range_cap(uint8_t param, uint8_t mode);

/* ------------------------------------------------------------------------ */
/* Commands                                                                  */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint8_t     cid;            /* always valid, even on error (reply CID) */
    uint16_t    len;            /* byte count */
    uint8_t     addr;           /* register or subdevice, if has_addr */
    bool        has_addr;
    union {
        struct { uint8_t value; }                           set_reg;
        struct { uint8_t param; cari_value_t value; }       set_param;
        struct { uint8_t action; }                          exec;
        struct { uint16_t port; }                           bind;
        struct { uint16_t port; uint16_t period_ms;
                 const uint8_t *qids; size_t nq; }          spvn;
        struct { uint8_t param; }                           get_param;
    } u;
} cari_cmd_t;

/*
 * Decode a CTRL message received by a slave.
 *
 * Returns CARI_OK, CARI_S_MALFORMED or CARI_S_UNSUPPORTED (unknown CID or
 * unknown parameter ID - the length of a SET_PARAM with a reserved parameter
 * cannot be checked). cmd->cid is always set to the CID to use in the reply
 * (0xFF for an empty message), as required by the reply rules.
 *
 * Only structural checks are done here. Semantic checks (address, support,
 * read-only, state, range) belong to the dispatcher.
 */
cari_status_t cari_cmd_decode(const uint8_t *buf, size_t n, cari_cmd_t *cmd);

/* Generic command encoder: CID, byte count, then body. Returns total length
 * or -1 if it does not fit. */
int cari_enc_raw(uint8_t *buf, size_t cap, uint8_t cid, const uint8_t *body, size_t body_len);
/* Encode a cari_cmd_t (inverse of cari_cmd_decode). Returns length or -1. */
int cari_cmd_encode(uint8_t *buf, size_t cap, const cari_cmd_t *cmd);

int cari_enc_ping(uint8_t *buf, size_t cap);
int cari_enc_set_reg(uint8_t *buf, size_t cap, uint8_t reg, uint8_t value);
int cari_enc_set_param(uint8_t *buf, size_t cap, uint8_t sub, uint8_t param, const cari_value_t *v);
int cari_enc_exec(uint8_t *buf, size_t cap, uint8_t sub, uint8_t action);
int cari_enc_bind(uint8_t *buf, size_t cap, uint8_t cid, uint8_t sub, uint16_t port); /* CID 0x04/0x05 */
int cari_enc_bind_spvn(uint8_t *buf, size_t cap, uint16_t port, uint16_t period_ms,
                       const uint8_t *qids, size_t nq);
int cari_enc_get_ident(uint8_t *buf, size_t cap);
int cari_enc_get_reg(uint8_t *buf, size_t cap, uint8_t reg);
int cari_enc_get_sub_caps(uint8_t *buf, size_t cap, uint8_t sub);
int cari_enc_get_param(uint8_t *buf, size_t cap, uint8_t sub, uint8_t param);
int cari_enc_get_spvn_list(uint8_t *buf, size_t cap);
int cari_enc_get_dev_caps(uint8_t *buf, size_t cap);

/* ------------------------------------------------------------------------ */
/* Replies                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint8_t        cid;
    uint8_t        status;
    const uint8_t *data;
    size_t         data_len;
} cari_reply_t;

/* Encode a reply. A non-zero status always yields a 4-byte reply (data is
 * ignored). Returns length or -1 if it does not fit. */
int cari_reply_encode(uint8_t *buf, size_t cap, uint8_t cid, uint8_t status,
                      const uint8_t *data, size_t data_len);

/* Structural decode of a reply: byte count, error replies are exactly 4
 * bytes. Returns 0 or -1. */
int cari_reply_decode(const uint8_t *buf, size_t n, cari_reply_t *r);

/* Expected data length of a successful reply to the given command, or -1 if
 * variable. */
int cari_reply_expected_data_len(const uint8_t *cmd, size_t cmd_len);

/* Full master-side check of a reply against the command that caused it:
 * structure, CID echo and data length. Returns 0 or -1. */
int cari_reply_validate(const uint8_t *cmd, size_t cmd_len, const uint8_t *buf, size_t n,
                        cari_reply_t *r);

/* ------------------------------------------------------------------------ */
/* Capability TLV records (Table 12)                                         */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    bool     overflow;      /* sticky error: record did not fit / bad ID */
} cari_tlv_writer_t;

void cari_tlv_init(cari_tlv_writer_t *w, uint8_t *buf, size_t cap);
int  cari_tlv_flag(cari_tlv_writer_t *w, uint8_t id);                      /* explicit, len 0 */
int  cari_tlv_range_u64(cari_tlv_writer_t *w, uint8_t id, uint64_t lo, uint64_t hi);
int  cari_tlv_range_f32(cari_tlv_writer_t *w, uint8_t id, float lo, float hi);
int  cari_tlv_value_u64(cari_tlv_writer_t *w, uint8_t id, uint64_t v);    /* equal bounds */
int  cari_tlv_value_f32(cari_tlv_writer_t *w, uint8_t id, float v);       /* equal bounds */
int  cari_tlv_raw(cari_tlv_writer_t *w, uint8_t id, const uint8_t *val, uint8_t len);

typedef struct {
    uint8_t        id;
    uint8_t        len;
    const uint8_t *val;
} cari_tlv_rec_t;

typedef struct {
    const uint8_t *p;
    size_t         n;
    size_t         off;
} cari_tlv_iter_t;

void cari_tlv_iter_init(cari_tlv_iter_t *it, const uint8_t *p, size_t n);
/* 1 = record returned, 0 = end, -1 = truncated record */
int  cari_tlv_next(cari_tlv_iter_t *it, cari_tlv_rec_t *rec);

#define CARI_CAPS_MAX_RANGES 128

typedef struct {
    uint8_t      id;
    cari_value_t lo;
    cari_value_t hi;
} cari_range_t;

/* A parsed (or to-be-encoded) capability list. Works for both device and
 * subdevice lists; for device lists only `flags` is used. */
typedef struct {
    uint8_t      flags[16];                     /* bitmap of explicit IDs 0..127 */
    cari_range_t ranges[CARI_CAPS_MAX_RANGES];
    size_t       nranges;
    size_t       unknown_skipped;               /* records skipped (unknown ID) */
} cari_caps_t;

void cari_caps_clear(cari_caps_t *c);
void cari_caps_set_flag(cari_caps_t *c, uint8_t id);
bool cari_caps_has_flag(const cari_caps_t *c, uint8_t id);
int  cari_caps_add_range(cari_caps_t *c, uint8_t id, cari_value_t lo, cari_value_t hi);
bool cari_caps_has_range(const cari_caps_t *c, uint8_t id);
/* True if v lies within at least one range with this ID. If no range with
 * this ID exists, returns `if_none`. */
bool cari_caps_in_range(const cari_caps_t *c, uint8_t id, const cari_value_t *v, bool if_none);
/* Parse a TLV list. Unknown IDs are skipped (by length). Known explicit IDs
 * must have length 0, known ranged IDs must have the size of Table 14b and
 * come in lo/hi pairs. `subdevice` selects Table 14 (true) or 13 (false).
 * Returns 0 or -1. */
int  cari_caps_parse(const uint8_t *p, size_t n, bool subdevice, cari_caps_t *out);
/* Encode back to TLV (flags first in ID order, then ranges in insertion
 * order). Returns length or -1. */
int  cari_caps_encode(const cari_caps_t *c, uint8_t *buf, size_t cap);

/* ------------------------------------------------------------------------ */
/* Parameter rules                                                           */
/* ------------------------------------------------------------------------ */

/*
 * Check a parameter write against a subdevice's capability list
 * (Table 15 "Parameter rules"). Returns:
 *   CARI_S_READ_ONLY     TX state
 *   CARI_S_UNSUPPORTED   capability missing (e.g. TX frequency without TX,
 *                        AGC without capability 0x05, compression without
 *                        device capability 0x00)
 *   CARI_S_OUT_OF_RANGE  boolean other than 0/1, NaN/inf, value outside every
 *                        declared range, unsupported format or mode
 *   CARI_S_INVALID_STATE deviation while the mode is neither FM nor PM
 *   CARI_OK
 * `dev_caps` may be NULL (compression is then not checked), `cur_mode` is
 * the current modulation mode (used for the deviation range).
 */
cari_status_t cari_param_check(const cari_caps_t *sub, const cari_caps_t *dev_caps,
                               uint8_t param, const cari_value_t *v, uint8_t cur_mode);

/* Is the mode supported by the subdevice (any modulator or demodulator)? */
bool cari_mode_supported(const cari_caps_t *sub, uint8_t mode);

/*
 * Combination check done when a stream starts (actions 0x01/0x03):
 * I/Q needs a complex format, other modes a real one; compression needs an
 * integer format and a FLAC-representable sample rate.
 * Returns CARI_OK or CARI_S_INVALID_STATE.
 */
cari_status_t cari_stream_combo_check(uint8_t format, uint8_t mode, bool compression,
                                      double sample_rate);

/* ------------------------------------------------------------------------ */
/* Supervision packets                                                       */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint8_t qid;
    uint8_t sub;        /* 0xFF for device-level quantities */
    float   value;
} cari_spvn_entry_t;

/* Append one entry to a packet. Returns new length or -1. */
int cari_spvn_append(uint8_t *buf, size_t cap, size_t len, uint8_t qid, uint8_t sub, float v);
/* Parse a packet. Returns number of entries, or -1 on unknown quantity /
 * truncation (unknown quantities have no known size and cannot be skipped). */
int cari_spvn_parse(const uint8_t *p, size_t n, cari_spvn_entry_t *out, size_t max);

/* ------------------------------------------------------------------------ */
/* Sample scaling (Baseband data format)                                     */
/* ------------------------------------------------------------------------ */

/* Bytes per sample, or 0 for an unknown format. */
size_t cari_format_bytes(uint8_t fmt);
/* Decode n samples to normalized floats in [-1, 1). Complex formats produce
 * interleaved I/Q, so dst holds n*channels floats. Returns 0 or -1. */
int cari_samples_to_float(uint8_t fmt, const void *src, size_t n, float *dst);
/* Encode normalized floats (clipped to the representable range, rounded). */
int cari_samples_from_float(uint8_t fmt, const float *src, size_t n, void *dst);
/* Integer formats only: de-interleave into per-channel signed integers, with
 * the -2^(n-1) offset for unsigned formats (FLAC input). */
int cari_samples_to_int32(uint8_t fmt, const void *src, size_t n, int32_t *const ch[]);
/* Inverse of the above (FLAC output). Values are clipped to the format. */
int cari_samples_from_int32(uint8_t fmt, const int32_t *const ch[], size_t n, void *dst);

/* ------------------------------------------------------------------------ */
/* FLAC framing helpers (Compressed baseband) - no libFLAC needed            */
/* ------------------------------------------------------------------------ */

/* Sample rate representable in a FLAC frame header: an integer number of
 * Hz, either <= 65535 or a multiple of 10 up to 655350. */
bool     cari_flac_rate_ok(double rate);
uint8_t  cari_flac_crc8(const uint8_t *p, size_t n);
uint16_t cari_flac_crc16(const uint8_t *p, size_t n);

typedef struct {
    bool     variable_blocksize;
    uint32_t block_size;
    uint32_t sample_rate;       /* 0 = "get from STREAMINFO" */
    uint8_t  channels;
    uint8_t  chan_assign;       /* raw 4-bit code */
    uint8_t  bps;               /* 0 = "get from STREAMINFO" */
    uint64_t number;            /* frame or sample number */
    size_t   header_len;        /* incl. CRC-8 */
} cari_flac_hdr_t;

/* Parse and CRC-check a frame header at p. 0 = ok, -1 = not a valid header,
 * -2 = need more bytes. */
int  cari_flac_parse_header(const uint8_t *p, size_t n, cari_flac_hdr_t *h);
/* Header encodes sample rate and bit depth explicitly (CARI requirement). */
bool cari_flac_hdr_explicit(const cari_flac_hdr_t *h);

typedef struct {
    size_t          off;
    size_t          len;
    cari_flac_hdr_t hdr;
} cari_flac_span_t;

/*
 * Split a message into complete FLAC frames. Data before the first frame sync
 * code (a receiver joining an ongoing stream) and anything that does not
 * verify (header CRC-8 and frame CRC-16) is skipped and counted in *skipped.
 * Returns the number of frames stored in out (at most max).
 */
size_t cari_flac_split(const uint8_t *msg, size_t n, cari_flac_span_t *out, size_t max,
                       size_t *skipped);

#ifdef __cplusplus
}
#endif
#endif /* CARI_PROTO_H */
