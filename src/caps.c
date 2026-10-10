/*
 * libcari - capability TLV records and parameter rules
 */
#include "cari/cari_proto.h"

#include <math.h>
#include <string.h>

/* ---------------------------------------------------------------- writer */

void cari_tlv_init(cari_tlv_writer_t *w, uint8_t *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->overflow = false;
}

int cari_tlv_raw(cari_tlv_writer_t *w, uint8_t id, const uint8_t *val, uint8_t len)
{
    if (w->overflow || w->len + 2 + len > w->cap) {
        w->overflow = true;
        return -1;
    }
    w->buf[w->len++] = id;
    w->buf[w->len++] = len;
    if (len)
        memcpy(&w->buf[w->len], val, len);
    w->len += len;
    return 0;
}

int cari_tlv_flag(cari_tlv_writer_t *w, uint8_t id)
{
    if (CARI_CAP_IS_RANGED(id)) {
        w->overflow = true;
        return -1;
    }
    return cari_tlv_raw(w, id, NULL, 0);
}

static int range_type_ok(cari_tlv_writer_t *w, uint8_t id, cari_type_t t)
{
    const cari_cap_info_t *ci = cari_subcap_info(id);
    /* unknown (reserved) ranged IDs may be written as raw records only */
    if (!CARI_CAP_IS_RANGED(id) || !ci || ci->type != t) {
        w->overflow = true;
        return 0;
    }
    return 1;
}

int cari_tlv_range_u64(cari_tlv_writer_t *w, uint8_t id, uint64_t lo, uint64_t hi)
{
    uint8_t b[8];
    if (!range_type_ok(w, id, CARI_T_U64))
        return -1;
    cari_put_le64(b, lo);
    if (cari_tlv_raw(w, id, b, 8) < 0)
        return -1;
    cari_put_le64(b, hi);
    return cari_tlv_raw(w, id, b, 8);
}

int cari_tlv_range_f32(cari_tlv_writer_t *w, uint8_t id, float lo, float hi)
{
    uint8_t b[4];
    if (!range_type_ok(w, id, CARI_T_F32))
        return -1;
    cari_put_f32(b, lo);
    if (cari_tlv_raw(w, id, b, 4) < 0)
        return -1;
    cari_put_f32(b, hi);
    return cari_tlv_raw(w, id, b, 4);
}

int cari_tlv_value_u64(cari_tlv_writer_t *w, uint8_t id, uint64_t v) { return cari_tlv_range_u64(w, id, v, v); }
int cari_tlv_value_f32(cari_tlv_writer_t *w, uint8_t id, float v)    { return cari_tlv_range_f32(w, id, v, v); }

/* ---------------------------------------------------------------- reader */

void cari_tlv_iter_init(cari_tlv_iter_t *it, const uint8_t *p, size_t n)
{
    it->p = p;
    it->n = n;
    it->off = 0;
}

int cari_tlv_next(cari_tlv_iter_t *it, cari_tlv_rec_t *rec)
{
    if (it->off >= it->n)
        return 0;
    if (it->off + 2 > it->n)
        return -1;
    rec->id = it->p[it->off];
    rec->len = it->p[it->off + 1];
    if (it->off + 2 + rec->len > it->n)
        return -1;
    rec->val = &it->p[it->off + 2];
    it->off += 2u + rec->len;
    return 1;
}

/* ---------------------------------------------------------------- caps set */

void cari_caps_clear(cari_caps_t *c)
{
    memset(c, 0, sizeof *c);
}

void cari_caps_set_flag(cari_caps_t *c, uint8_t id)
{
    if (id < 128)
        c->flags[id >> 3] |= (uint8_t)(1u << (id & 7));
}

bool cari_caps_has_flag(const cari_caps_t *c, uint8_t id)
{
    if (id >= 128)
        return cari_caps_has_range(c, id);
    return (c->flags[id >> 3] >> (id & 7)) & 1;
}

int cari_caps_add_range(cari_caps_t *c, uint8_t id, cari_value_t lo, cari_value_t hi)
{
    if (c->nranges >= CARI_CAPS_MAX_RANGES || !CARI_CAP_IS_RANGED(id) || lo.type != hi.type)
        return -1;
    c->ranges[c->nranges].id = id;
    c->ranges[c->nranges].lo = lo;
    c->ranges[c->nranges].hi = hi;
    c->nranges++;
    return 0;
}

bool cari_caps_has_range(const cari_caps_t *c, uint8_t id)
{
    for (size_t i = 0; i < c->nranges; i++)
        if (c->ranges[i].id == id)
            return true;
    return false;
}

static bool value_in(const cari_range_t *r, const cari_value_t *v)
{
    if (r->lo.type == CARI_T_U64 && v->type == CARI_T_U64)
        return v->v.u64 >= r->lo.v.u64 && v->v.u64 <= r->hi.v.u64;
    /* Compare in float precision: equal-bound pairs (discrete values) were
     * transmitted as floats, so an exact comparison is meaningful. */
    if (v->type == CARI_T_F32 && r->lo.type == CARI_T_F32)
        return v->v.f32 >= r->lo.v.f32 && v->v.f32 <= r->hi.v.f32;
    double x = cari_value_as_double(v);
    return x >= cari_value_as_double(&r->lo) && x <= cari_value_as_double(&r->hi);
}

bool cari_caps_in_range(const cari_caps_t *c, uint8_t id, const cari_value_t *v, bool if_none)
{
    bool any = false;
    for (size_t i = 0; i < c->nranges; i++) {
        if (c->ranges[i].id != id)
            continue;
        any = true;
        if (value_in(&c->ranges[i], v))
            return true;
    }
    return any ? false : if_none;
}

int cari_caps_parse(const uint8_t *p, size_t n, bool subdevice, cari_caps_t *out)
{
    cari_tlv_iter_t it;
    cari_tlv_rec_t rec;
    int r;
    bool pending = false;
    cari_value_t pend_lo = { 0 };
    uint8_t pend_id = 0;

    cari_caps_clear(out);
    cari_tlv_iter_init(&it, p, n);
    while ((r = cari_tlv_next(&it, &rec)) == 1) {
        if (!subdevice) {
            if (cari_devcap_info(rec.id)) {
                if (rec.len != 0)
                    return -1;
                cari_caps_set_flag(out, rec.id);
            } else {
                out->unknown_skipped++;
            }
            continue;
        }
        const cari_cap_info_t *ci = cari_subcap_info(rec.id);
        if (!ci) {
            out->unknown_skipped++;
            continue;
        }
        if (!ci->ranged) {
            if (rec.len != 0)
                return -1;
            cari_caps_set_flag(out, rec.id);
            continue;
        }
        cari_value_t v;
        if (cari_value_decode(ci->type, rec.val, rec.len, &v) < 0)
            return -1;
        if (!pending) {
            pending = true;
            pend_id = rec.id;
            pend_lo = v;
        } else {
            if (rec.id != pend_id)
                return -1;              /* lower bound without its upper bound */
            if (cari_value_as_double(&v) < cari_value_as_double(&pend_lo))
                return -1;              /* inverted range */
            if (cari_caps_add_range(out, rec.id, pend_lo, v) < 0)
                return -1;
            pending = false;
        }
    }
    if (r < 0 || pending)
        return -1;
    return 0;
}

int cari_caps_encode(const cari_caps_t *c, uint8_t *buf, size_t cap)
{
    cari_tlv_writer_t w;
    cari_tlv_init(&w, buf, cap);
    for (unsigned id = 0; id < 128; id++)
        if (cari_caps_has_flag(c, (uint8_t)id))
            cari_tlv_flag(&w, (uint8_t)id);
    for (size_t i = 0; i < c->nranges; i++) {
        const cari_range_t *r = &c->ranges[i];
        if (r->lo.type == CARI_T_U64)
            cari_tlv_range_u64(&w, r->id, r->lo.v.u64, r->hi.v.u64);
        else
            cari_tlv_range_f32(&w, r->id, r->lo.v.f32, r->hi.v.f32);
    }
    return w.overflow ? -1 : (int)w.len;
}

/* ---------------------------------------------------------------- rules */

bool cari_mode_supported(const cari_caps_t *s, uint8_t mode)
{
    switch (mode) {
    case CARI_MODE_IQ:  return cari_caps_has_flag(s, CARI_SCAP_IQ);
    case CARI_MODE_AM:  return cari_caps_has_flag(s, CARI_SCAP_AM_DEMOD)  || cari_caps_has_flag(s, CARI_SCAP_AM_MOD);
    case CARI_MODE_FM:  return cari_caps_has_flag(s, CARI_SCAP_FM_DEMOD)  || cari_caps_has_flag(s, CARI_SCAP_FM_MOD);
    case CARI_MODE_PM:  return cari_caps_has_flag(s, CARI_SCAP_PM_DEMOD)  || cari_caps_has_flag(s, CARI_SCAP_PM_MOD);
    case CARI_MODE_USB: return cari_caps_has_flag(s, CARI_SCAP_USB_DEMOD) || cari_caps_has_flag(s, CARI_SCAP_USB_MOD);
    case CARI_MODE_LSB: return cari_caps_has_flag(s, CARI_SCAP_LSB_DEMOD) || cari_caps_has_flag(s, CARI_SCAP_LSB_MOD);
    default:            return false;
    }
}

cari_status_t cari_param_check(const cari_caps_t *s, const cari_caps_t *dev,
                               uint8_t param, const cari_value_t *v, uint8_t cur_mode)
{
    const cari_param_info_t *pi = cari_param_info(param);
    if (!pi)
        return CARI_S_UNSUPPORTED;
    if (pi->flags & CARI_PF_READONLY)
        return CARI_S_READ_ONLY;
    if (v->type != pi->type)
        return CARI_S_MALFORMED;

    /* capability prerequisites -> status 2 */
    bool rx = cari_caps_has_flag(s, CARI_SCAP_RX), tx = cari_caps_has_flag(s, CARI_SCAP_TX);
    switch (param) {
    case CARI_P_RX_FREQ:
    case CARI_P_LNA_GAIN:
        if (!rx) return CARI_S_UNSUPPORTED;
        break;
    case CARI_P_TX_FREQ:
    case CARI_P_TX_POWER:
        if (!tx) return CARI_S_UNSUPPORTED;
        break;
    case CARI_P_AGC:
        if (!cari_caps_has_flag(s, CARI_SCAP_AGC)) return CARI_S_UNSUPPORTED;
        break;
    case CARI_P_AFC:
        if (!cari_caps_has_flag(s, CARI_SCAP_AFC)) return CARI_S_UNSUPPORTED;
        break;
    case CARI_P_DC_CORR:
        if (!cari_caps_has_flag(s, CARI_SCAP_DC_CORR)) return CARI_S_UNSUPPORTED;
        break;
    case CARI_P_COMPRESSION:
        if (dev && !cari_caps_has_flag(dev, CARI_DCAP_COMPRESSION)) return CARI_S_UNSUPPORTED;
        break;
    default:
        break;
    }

    /* value checks -> status 5 */
    if (v->type == CARI_T_BOOL && v->v.u8 > 1)
        return CARI_S_OUT_OF_RANGE;
    if (v->type == CARI_T_F32 && !isfinite(v->v.f32))
        return CARI_S_OUT_OF_RANGE;
    if (param == CARI_P_FORMAT) {
        if (!cari_format_info(v->v.u8) || !cari_caps_has_flag(s, v->v.u8))
            return CARI_S_OUT_OF_RANGE;
    }
    if (param == CARI_P_MODE && !cari_mode_supported(s, v->v.u8))
        return CARI_S_OUT_OF_RANGE;

    int rc = cari_param_range_cap(param, cur_mode);
    if (rc >= 0 && !cari_caps_in_range(s, (uint8_t)rc, v, true))
        return CARI_S_OUT_OF_RANGE;

    /* state -> status 8 (CARI 2.1: deviation only in FM/PM mode) */
    if (param == CARI_P_DEVIATION && cur_mode != CARI_MODE_FM && cur_mode != CARI_MODE_PM)
        return CARI_S_INVALID_STATE;
    return CARI_OK;
}

cari_status_t cari_stream_combo_check(uint8_t format, uint8_t mode, bool compression,
                                      double sample_rate)
{
    const cari_format_info_t *fi = cari_format_info(format);
    if (!fi)
        return CARI_S_INVALID_STATE;
    if ((mode == CARI_MODE_IQ) != fi->complex)
        return CARI_S_INVALID_STATE;
    if (compression && (fi->is_float || !cari_flac_rate_ok(sample_rate)))
        return CARI_S_INVALID_STATE;
    return CARI_OK;
}
