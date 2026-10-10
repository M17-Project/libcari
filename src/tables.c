/*
 * libcari - protocol tables and typed values
 */
#include "cari/cari_proto.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define N(a) (sizeof(a) / sizeof((a)[0]))

/* ---------------------------------------------------------------- floats */

float cari_get_f32(const uint8_t *p)
{
    uint32_t u = cari_get_le32(p);
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

void cari_put_f32(uint8_t *p, float v)
{
    uint32_t u;
    memcpy(&u, &v, sizeof u);
    cari_put_le32(p, u);
}

/* ---------------------------------------------------------------- values */

size_t cari_type_size(cari_type_t t)
{
    switch (t) {
    case CARI_T_U8:
    case CARI_T_BOOL: return 1;
    case CARI_T_U64:  return 8;
    case CARI_T_F32:  return 4;
    default:          return 0;
    }
}

cari_value_t cari_value_u8(uint8_t v)   { cari_value_t r = { .type = CARI_T_U8 };   r.v.u8 = v;  return r; }
cari_value_t cari_value_bool(bool v)    { cari_value_t r = { .type = CARI_T_BOOL }; r.v.u8 = v ? 1 : 0; return r; }
cari_value_t cari_value_u64(uint64_t v) { cari_value_t r = { .type = CARI_T_U64 };  r.v.u64 = v; return r; }
cari_value_t cari_value_f32(float v)    { cari_value_t r = { .type = CARI_T_F32 };  r.v.f32 = v; return r; }

int cari_value_decode(cari_type_t t, const uint8_t *p, size_t n, cari_value_t *out)
{
    if (t == CARI_T_NONE || n != cari_type_size(t))
        return -1;
    out->type = t;
    switch (t) {
    case CARI_T_U8:
    case CARI_T_BOOL: out->v.u8 = p[0]; break;
    case CARI_T_U64:  out->v.u64 = cari_get_le64(p); break;
    case CARI_T_F32:  out->v.f32 = cari_get_f32(p); break;
    default: return -1;
    }
    return 0;
}

size_t cari_value_encode(const cari_value_t *v, uint8_t *p)
{
    switch (v->type) {
    case CARI_T_U8:
    case CARI_T_BOOL: p[0] = v->v.u8; return 1;
    case CARI_T_U64:  cari_put_le64(p, v->v.u64); return 8;
    case CARI_T_F32:  cari_put_f32(p, v->v.f32); return 4;
    default:          return 0;
    }
}

double cari_value_as_double(const cari_value_t *v)
{
    switch (v->type) {
    case CARI_T_U8:
    case CARI_T_BOOL: return v->v.u8;
    case CARI_T_U64:  return (double)v->v.u64;
    case CARI_T_F32:  return v->v.f32;
    default:          return NAN;
    }
}

int cari_value_parse(cari_type_t t, const char *s, cari_value_t *out)
{
    char *end = NULL;
    if (!s || !*s)
        return -1;
    errno = 0;
    switch (t) {
    case CARI_T_BOOL:
        if (!strcasecmp(s, "on") || !strcasecmp(s, "true") || !strcasecmp(s, "yes")) {
            *out = cari_value_bool(true); return 0;
        }
        if (!strcasecmp(s, "off") || !strcasecmp(s, "false") || !strcasecmp(s, "no")) {
            *out = cari_value_bool(false); return 0;
        }
        /* numeric: any byte value (the slave range-checks) */
        /* fall through */
    case CARI_T_U8: {
        unsigned long u = strtoul(s, &end, 0);
        if (errno || *end || u > 255 || s[0] == '-')
            return -1;
        out->type = t;
        out->v.u8 = (uint8_t)u;
        return 0;
    }
    case CARI_T_U64: {
        /* Accept plain integers and engineering notation (433.475e6). */
        if (s[0] == '-')
            return -1;
        unsigned long long u = strtoull(s, &end, 0);
        if (!errno && !*end) {
            *out = cari_value_u64(u);
            return 0;
        }
        errno = 0;
        double d = strtod(s, &end);
        if (errno || *end || !(d >= 0) || d > 1.8e19 || d != floor(d))
            return -1;
        *out = cari_value_u64((uint64_t)d);
        return 0;
    }
    case CARI_T_F32: {
        double d = strtod(s, &end);
        if (errno || *end)
            return -1;
        *out = cari_value_f32((float)d);
        return 0;
    }
    default:
        return -1;
    }
}

int cari_value_format(const cari_value_t *v, char *buf, size_t cap)
{
    switch (v->type) {
    case CARI_T_U8:   return snprintf(buf, cap, "%u", v->v.u8);
    case CARI_T_BOOL: return snprintf(buf, cap, "%u", v->v.u8);
    case CARI_T_U64:  return snprintf(buf, cap, "%llu", (unsigned long long)v->v.u64);
    case CARI_T_F32:  return snprintf(buf, cap, "%g", (double)v->v.f32);
    default:          return snprintf(buf, cap, "?");
    }
}

/* ---------------------------------------------------------------- tables */

static const cari_param_info_t params[] = {
    { CARI_P_RX_FREQ,     "rx_freq",     "Receive frequency",            CARI_T_U64,  "Hz",  0 },
    { CARI_P_TX_FREQ,     "tx_freq",     "Transmit frequency",           CARI_T_U64,  "Hz",  0 },
    { CARI_P_LNA_GAIN,    "lna_gain",    "LNA gain",                     CARI_T_F32,  "dB",  0 },
    { CARI_P_TX_POWER,    "tx_power",    "Output power",                 CARI_T_F32,  "dBm", 0 },
    { CARI_P_CH_WIDTH,    "ch_width",    "Channel width",                CARI_T_F32,  "Hz",  0 },
    { CARI_P_SAMPLE_RATE, "sample_rate", "Sample rate",                  CARI_T_F32,  "Hz",  CARI_PF_STREAM_LOCKED },
    { CARI_P_FREQ_CORR,   "freq_corr",   "Frequency correction",         CARI_T_F32,  "ppm", 0 },
    { CARI_P_AGC,         "agc",         "Automatic Gain Control",       CARI_T_BOOL, "",    0 },
    { CARI_P_AFC,         "afc",         "Automatic Frequency Control",  CARI_T_BOOL, "",    0 },
    { CARI_P_DC_CORR,     "dc_corr",     "DC offset correction",         CARI_T_BOOL, "",    0 },
    { CARI_P_FORMAT,      "format",      "Signal format",                CARI_T_U8,   "",    CARI_PF_STREAM_LOCKED },
    { CARI_P_MODE,        "mode",        "Modulation mode",              CARI_T_U8,   "",    CARI_PF_STREAM_LOCKED },
    { CARI_P_DEVIATION,   "deviation",   "Deviation",                    CARI_T_F32,  "Hz/rad", 0 },
    { CARI_P_COMPRESSION, "compression", "Baseband compression",         CARI_T_BOOL, "",    CARI_PF_STREAM_LOCKED },
    { CARI_P_TX_STATE,    "tx_state",    "TX state",                     CARI_T_U8,   "",    CARI_PF_READONLY },
};

static const cari_cap_info_t subcaps[] = {
    { CARI_SCAP_RESET,       "reset",       "Subdevice can be reset",               false, CARI_T_NONE, "" },
    { CARI_SCAP_IQ,          "iq",          "I/Q modulation available",             false, CARI_T_NONE, "" },
    { CARI_SCAP_RX,          "rx",          "Receiver",                             false, CARI_T_NONE, "" },
    { CARI_SCAP_TX,          "tx",          "Transmitter",                          false, CARI_T_NONE, "" },
    { CARI_SCAP_FULL_DUPLEX, "duplex",      "Full duplex available",                false, CARI_T_NONE, "" },
    { CARI_SCAP_AGC,         "agc",         "Automatic Gain Control available",     false, CARI_T_NONE, "" },
    { CARI_SCAP_AFC,         "afc",         "Automatic Frequency Control available", false, CARI_T_NONE, "" },
    { CARI_SCAP_FREQ_REF,    "freq_ref",    "Frequency reference available",        false, CARI_T_NONE, "" },
    { CARI_SCAP_AM_DEMOD,    "am_demod",    "Amplitude demodulator available",      false, CARI_T_NONE, "" },
    { CARI_SCAP_FM_DEMOD,    "fm_demod",    "Frequency demodulator available",      false, CARI_T_NONE, "" },
    { CARI_SCAP_PM_DEMOD,    "pm_demod",    "Phase demodulator available",          false, CARI_T_NONE, "" },
    { CARI_SCAP_USB_DEMOD,   "usb_demod",   "Upper sideband demodulator available", false, CARI_T_NONE, "" },
    { CARI_SCAP_LSB_DEMOD,   "lsb_demod",   "Lower sideband demodulator available", false, CARI_T_NONE, "" },
    { CARI_SCAP_AM_MOD,      "am_mod",      "Amplitude modulator available",        false, CARI_T_NONE, "" },
    { CARI_SCAP_FM_MOD,      "fm_mod",      "Frequency modulator available",        false, CARI_T_NONE, "" },
    { CARI_SCAP_PM_MOD,      "pm_mod",      "Phase modulator available",            false, CARI_T_NONE, "" },
    { CARI_SCAP_USB_MOD,     "usb_mod",     "Upper sideband modulator available",   false, CARI_T_NONE, "" },
    { CARI_SCAP_LSB_MOD,     "lsb_mod",     "Lower sideband modulator available",   false, CARI_T_NONE, "" },
    { CARI_SCAP_DC_CORR,     "dc_corr",     "DC offset correction available",       false, CARI_T_NONE, "" },
    { CARI_SCAP_FMT_CF32,    "cf32",        "Signal format CF32 available",         false, CARI_T_NONE, "" },
    { CARI_SCAP_FMT_CS16,    "cs16",        "Signal format CS16 available",         false, CARI_T_NONE, "" },
    { CARI_SCAP_FMT_F32,     "f32",         "Signal format F32 available",          false, CARI_T_NONE, "" },
    { CARI_SCAP_FMT_S16,     "s16",         "Signal format S16 available",          false, CARI_T_NONE, "" },
    { CARI_SCAP_FMT_CS8,     "cs8",         "Signal format CS8 available",          false, CARI_T_NONE, "" },
    { CARI_SCAP_FMT_S8,      "s8",          "Signal format S8 available",           false, CARI_T_NONE, "" },
    { CARI_SCAP_FMT_CU8,     "cu8",         "Signal format CU8 available",          false, CARI_T_NONE, "" },
    { CARI_SCAP_FMT_U8,      "u8",          "Signal format U8 available",           false, CARI_T_NONE, "" },
    { CARI_SCAP_RX_FREQ,     "rx_freq",     "Receive frequency",                    true,  CARI_T_U64,  "Hz" },
    { CARI_SCAP_TX_FREQ,     "tx_freq",     "Transmit frequency",                   true,  CARI_T_U64,  "Hz" },
    { CARI_SCAP_LNA_GAIN,    "lna_gain",    "LNA gain",                             true,  CARI_T_F32,  "dB" },
    { CARI_SCAP_TX_POWER,    "tx_power",    "Output power",                         true,  CARI_T_F32,  "dBm" },
    { CARI_SCAP_CH_WIDTH,    "ch_width",    "Channel width",                        true,  CARI_T_F32,  "Hz" },
    { CARI_SCAP_SAMPLE_RATE, "sample_rate", "Sample rate",                          true,  CARI_T_F32,  "Hz" },
    { CARI_SCAP_FM_DEV,      "fm_dev",      "FM deviation",                         true,  CARI_T_F32,  "Hz" },
    { CARI_SCAP_PM_DEV,      "pm_dev",      "PM deviation",                         true,  CARI_T_F32,  "rad" },
};

static const cari_enum_info_t devcaps[] = {
    { CARI_DCAP_COMPRESSION, "compression", "Baseband stream compression available" },
    { CARI_DCAP_SUPERVISION, "supervision", "Supervision channel available" },
};

static const cari_enum_info_t actions[] = {
    { CARI_ACT_RESET,      "reset",      "Subdevice reset" },
    { CARI_ACT_RX_START,   "rx_start",   "Reception start" },
    { CARI_ACT_RX_STOP,    "rx_stop",    "Reception stop" },
    { CARI_ACT_TX_START,   "tx_start",   "Transmission start" },
    { CARI_ACT_TX_STOP,    "tx_stop",    "Transmission stop" },
    { CARI_ACT_CLEAR_TRIP, "clear_trip", "Clear UL underrun trip" },
};

static const cari_enum_info_t modes[] = {
    { CARI_MODE_IQ,  "iq",  "I/Q" },
    { CARI_MODE_AM,  "am",  "AM" },
    { CARI_MODE_FM,  "fm",  "FM" },
    { CARI_MODE_PM,  "pm",  "PM" },
    { CARI_MODE_USB, "usb", "USB" },
    { CARI_MODE_LSB, "lsb", "LSB" },
};

static const cari_quantity_info_t quantities[] = {
    { CARI_Q_TEMPERATURE,     "temp",      "Temperature",                   "degC", false },
    { CARI_Q_VOLTAGE,         "voltage",   "Voltage",                       "V",    false },
    { CARI_Q_CURRENT,         "current",   "Current",                       "A",    false },
    { CARI_Q_RETURN_LOSS,     "rl",        "Return loss",                   "dB",   true },
    { CARI_Q_POWER_INCIDENT,  "pwr_inc",   "RF power (incident, average)",  "dBm",  true },
    { CARI_Q_POWER_REFLECTED, "pwr_refl",  "RF power (reflected, average)", "dBm",  true },
};

static const cari_format_info_t formats[] = {
    /* id            name    cplx   float  signed bits ch bytes */
    { CARI_FMT_CF32, "CF32", true,  true,  true,  32, 2, 8 },
    { CARI_FMT_CS16, "CS16", true,  false, true,  16, 2, 4 },
    { CARI_FMT_F32,  "F32",  false, true,  true,  32, 1, 4 },
    { CARI_FMT_S16,  "S16",  false, false, true,  16, 1, 2 },
    { CARI_FMT_CS8,  "CS8",  true,  false, true,  8,  2, 2 },
    { CARI_FMT_S8,   "S8",   false, false, true,  8,  1, 1 },
    { CARI_FMT_CU8,  "CU8",  true,  false, false, 8,  2, 2 },
    { CARI_FMT_U8,   "U8",   false, false, false, 8,  1, 1 },
};

static const cari_cmd_info_t cmds[] = {
    /* cid                     name               addr  min fixed */
    { CARI_CID_PING,           "PING",            false, 3, 3 },
    { CARI_CID_SET_REG,        "SET_REG",         true,  5, 5 },
    { CARI_CID_SET_PARAM,      "SET_PARAM",       true,  5, 0 },   /* 5 + param size */
    { CARI_CID_EXEC,           "EXEC",            true,  5, 5 },
    { CARI_CID_BIND_UL,        "BIND_UL",         true,  6, 6 },
    { CARI_CID_BIND_DL,        "BIND_DL",         true,  6, 6 },
    { CARI_CID_BIND_SPVN,      "BIND_SPVN",       false, 7, 0 },   /* 7 + n */
    { CARI_CID_GET_IDENT,      "GET_IDENT",       false, 3, 3 },
    { CARI_CID_GET_REG,        "GET_REG",         true,  4, 4 },
    { CARI_CID_GET_SUB_CAPS,   "GET_SUB_CAPS",    true,  4, 4 },
    { CARI_CID_GET_PARAM,      "GET_PARAM",       true,  5, 5 },
    { CARI_CID_GET_SPVN_LIST,  "GET_SPVN_LIST",   false, 3, 3 },
    { CARI_CID_GET_DEV_CAPS,   "GET_DEV_CAPS",    false, 3, 3 },
};

#define FIND_BY_ID(tab, idv)                         \
    do {                                             \
        for (size_t i_ = 0; i_ < N(tab); i_++)       \
            if ((tab)[i_].id == (idv))               \
                return &(tab)[i_];                   \
        return NULL;                                 \
    } while (0)

#define FIND_BY_KEY(tab, field, s)                   \
    do {                                             \
        if (!(s)) return NULL;                       \
        for (size_t i_ = 0; i_ < N(tab); i_++)       \
            if (!strcasecmp((tab)[i_].field, (s)))   \
                return &(tab)[i_];                   \
        return NULL;                                 \
    } while (0)

const cari_param_info_t *cari_param_info(uint8_t id)            { FIND_BY_ID(params, id); }
const cari_param_info_t *cari_param_by_key(const char *k)       { FIND_BY_KEY(params, key, k); }
const cari_cap_info_t *cari_subcap_info(uint8_t id)             { FIND_BY_ID(subcaps, id); }
const cari_cap_info_t *cari_subcap_by_key(const char *k)        { FIND_BY_KEY(subcaps, key, k); }
const cari_enum_info_t *cari_devcap_info(uint8_t id)            { FIND_BY_ID(devcaps, id); }
const cari_enum_info_t *cari_action_info(uint8_t id)            { FIND_BY_ID(actions, id); }
const cari_enum_info_t *cari_action_by_key(const char *k)       { FIND_BY_KEY(actions, key, k); }
const cari_enum_info_t *cari_mode_info(uint8_t id)              { FIND_BY_ID(modes, id); }
const cari_enum_info_t *cari_mode_by_key(const char *k)         { FIND_BY_KEY(modes, key, k); }
const cari_quantity_info_t *cari_quantity_info(uint8_t id)      { FIND_BY_ID(quantities, id); }
const cari_quantity_info_t *cari_quantity_by_key(const char *k) { FIND_BY_KEY(quantities, key, k); }
const cari_format_info_t *cari_format_info(uint8_t id)          { FIND_BY_ID(formats, id); }
const cari_format_info_t *cari_format_by_name(const char *k)    { FIND_BY_KEY(formats, name, k); }

const cari_cmd_info_t *cari_cmd_info(uint8_t cid)
{
    for (size_t i = 0; i < N(cmds); i++)
        if (cmds[i].cid == cid)
            return &cmds[i];
    return NULL;
}

const char *cari_status_str(int status)
{
    switch (status) {
    case CARI_OK:               return "OK";
    case CARI_S_MALFORMED:      return "malformed frame";
    case CARI_S_UNSUPPORTED:    return "unsupported";
    case CARI_S_BIND_FAILED:    return "ZMQ port bind failed";
    case CARI_S_RESERVED_4:     return "reserved (4)";
    case CARI_S_OUT_OF_RANGE:   return "value out of range";
    case CARI_S_INVALID_ADDR:   return "invalid address";
    case CARI_S_READ_ONLY:      return "read-only";
    case CARI_S_INVALID_STATE:  return "invalid state";
    default:
        if (status > 0 && status < 256)
            return "reserved status";
        return "local error";
    }
}

const char *cari_tx_state_str(uint8_t st)
{
    switch (st) {
    case CARI_TX_IDLE:         return "idle";
    case CARI_TX_TRANSMITTING: return "transmitting";
    case CARI_TX_TRIPPED:      return "tripped";
    default:                   return "?";
    }
}

int cari_param_range_cap(uint8_t param, uint8_t mode)
{
    switch (param) {
    case CARI_P_RX_FREQ:     return CARI_SCAP_RX_FREQ;
    case CARI_P_TX_FREQ:     return CARI_SCAP_TX_FREQ;
    case CARI_P_LNA_GAIN:    return CARI_SCAP_LNA_GAIN;
    case CARI_P_TX_POWER:    return CARI_SCAP_TX_POWER;
    case CARI_P_CH_WIDTH:    return CARI_SCAP_CH_WIDTH;
    case CARI_P_SAMPLE_RATE: return CARI_SCAP_SAMPLE_RATE;
    case CARI_P_DEVIATION:
        if (mode == CARI_MODE_FM) return CARI_SCAP_FM_DEV;
        if (mode == CARI_MODE_PM) return CARI_SCAP_PM_DEV;
        return -1;
    default:                 return -1;
    }
}
