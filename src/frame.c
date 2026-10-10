/*
 * libcari - command/reply frame codec and supervision packets
 */
#include "cari/cari_proto.h"

#include <string.h>

/* ---------------------------------------------------------------- decode */

cari_status_t cari_cmd_decode(const uint8_t *buf, size_t n, cari_cmd_t *cmd)
{
    memset(cmd, 0, sizeof *cmd);
    cmd->cid = n ? buf[0] : 0xFF;

    /* Reply rules: < 3 bytes, or byte count != actual length -> malformed */
    if (n < CARI_HDR_LEN || n > CARI_MAX_FRAME)
        return CARI_S_MALFORMED;
    cmd->len = cari_get_le16(&buf[1]);
    if (cmd->len != n)
        return CARI_S_MALFORMED;

    const cari_cmd_info_t *ci = cari_cmd_info(cmd->cid);
    if (!ci)
        return CARI_S_UNSUPPORTED;      /* unknown CID: status 2 */

    if (n < ci->min_len || (ci->fixed_len && n != ci->fixed_len))
        return CARI_S_MALFORMED;

    const uint8_t *b = buf + CARI_HDR_LEN;
    if (ci->has_addr) {
        cmd->has_addr = true;
        cmd->addr = b[0];
        b++;
    }

    switch (cmd->cid) {
    case CARI_CID_SET_REG:
        cmd->u.set_reg.value = b[0];
        break;
    case CARI_CID_SET_PARAM: {
        const cari_param_info_t *pi = cari_param_info(b[0]);
        cmd->u.set_param.param = b[0];
        if (!pi)
            return CARI_S_UNSUPPORTED;  /* reserved parameter ID */
        size_t sz = cari_type_size(pi->type);
        if (n != 5 + sz)
            return CARI_S_MALFORMED;
        cari_value_decode(pi->type, b + 1, sz, &cmd->u.set_param.value);
        break;
    }
    case CARI_CID_EXEC:
        cmd->u.exec.action = b[0];
        break;
    case CARI_CID_BIND_UL:
    case CARI_CID_BIND_DL:
        cmd->u.bind.port = cari_get_le16(b);
        break;
    case CARI_CID_BIND_SPVN:
        cmd->u.spvn.port = cari_get_le16(b);
        cmd->u.spvn.period_ms = cari_get_le16(b + 2);
        cmd->u.spvn.qids = b + 4;
        cmd->u.spvn.nq = n - 7;
        break;
    case CARI_CID_GET_PARAM:
        cmd->u.get_param.param = b[0];
        break;
    default:
        break;
    }
    return CARI_OK;
}

/* ---------------------------------------------------------------- encode */

int cari_enc_raw(uint8_t *buf, size_t cap, uint8_t cid, const uint8_t *body, size_t body_len)
{
    size_t len = CARI_HDR_LEN + body_len;
    if (len > CARI_MAX_FRAME || len > cap)
        return -1;
    buf[0] = cid;
    cari_put_le16(&buf[1], (uint16_t)len);
    if (body_len)
        memmove(&buf[3], body, body_len);
    return (int)len;
}

int cari_enc_ping(uint8_t *buf, size_t cap)       { return cari_enc_raw(buf, cap, CARI_CID_PING, NULL, 0); }
int cari_enc_get_ident(uint8_t *buf, size_t cap)  { return cari_enc_raw(buf, cap, CARI_CID_GET_IDENT, NULL, 0); }
int cari_enc_get_spvn_list(uint8_t *buf, size_t cap) { return cari_enc_raw(buf, cap, CARI_CID_GET_SPVN_LIST, NULL, 0); }
int cari_enc_get_dev_caps(uint8_t *buf, size_t cap)  { return cari_enc_raw(buf, cap, CARI_CID_GET_DEV_CAPS, NULL, 0); }

int cari_enc_set_reg(uint8_t *buf, size_t cap, uint8_t reg, uint8_t value)
{
    uint8_t b[2] = { reg, value };
    return cari_enc_raw(buf, cap, CARI_CID_SET_REG, b, 2);
}

int cari_enc_set_param(uint8_t *buf, size_t cap, uint8_t sub, uint8_t param, const cari_value_t *v)
{
    uint8_t b[2 + 8];
    b[0] = sub;
    b[1] = param;
    size_t sz = cari_value_encode(v, &b[2]);
    if (!sz)
        return -1;
    return cari_enc_raw(buf, cap, CARI_CID_SET_PARAM, b, 2 + sz);
}

int cari_enc_exec(uint8_t *buf, size_t cap, uint8_t sub, uint8_t action)
{
    uint8_t b[2] = { sub, action };
    return cari_enc_raw(buf, cap, CARI_CID_EXEC, b, 2);
}

int cari_enc_bind(uint8_t *buf, size_t cap, uint8_t cid, uint8_t sub, uint16_t port)
{
    if (cid != CARI_CID_BIND_UL && cid != CARI_CID_BIND_DL)
        return -1;
    uint8_t b[3] = { sub };
    cari_put_le16(&b[1], port);
    return cari_enc_raw(buf, cap, cid, b, 3);
}

int cari_enc_bind_spvn(uint8_t *buf, size_t cap, uint16_t port, uint16_t period_ms,
                       const uint8_t *qids, size_t nq)
{
    if (7 + nq > cap || 7 + nq > CARI_MAX_FRAME)
        return -1;
    buf[0] = CARI_CID_BIND_SPVN;
    cari_put_le16(&buf[1], (uint16_t)(7 + nq));
    cari_put_le16(&buf[3], port);
    cari_put_le16(&buf[5], period_ms);
    if (nq)
        memmove(&buf[7], qids, nq);
    return (int)(7 + nq);
}

int cari_enc_get_reg(uint8_t *buf, size_t cap, uint8_t reg)
{
    return cari_enc_raw(buf, cap, CARI_CID_GET_REG, &reg, 1);
}

int cari_enc_get_sub_caps(uint8_t *buf, size_t cap, uint8_t sub)
{
    return cari_enc_raw(buf, cap, CARI_CID_GET_SUB_CAPS, &sub, 1);
}

int cari_enc_get_param(uint8_t *buf, size_t cap, uint8_t sub, uint8_t param)
{
    uint8_t b[2] = { sub, param };
    return cari_enc_raw(buf, cap, CARI_CID_GET_PARAM, b, 2);
}

int cari_cmd_encode(uint8_t *buf, size_t cap, const cari_cmd_t *c)
{
    switch (c->cid) {
    case CARI_CID_PING:          return cari_enc_ping(buf, cap);
    case CARI_CID_SET_REG:       return cari_enc_set_reg(buf, cap, c->addr, c->u.set_reg.value);
    case CARI_CID_SET_PARAM:     return cari_enc_set_param(buf, cap, c->addr, c->u.set_param.param,
                                                           &c->u.set_param.value);
    case CARI_CID_EXEC:          return cari_enc_exec(buf, cap, c->addr, c->u.exec.action);
    case CARI_CID_BIND_UL:
    case CARI_CID_BIND_DL:       return cari_enc_bind(buf, cap, c->cid, c->addr, c->u.bind.port);
    case CARI_CID_BIND_SPVN:     return cari_enc_bind_spvn(buf, cap, c->u.spvn.port, c->u.spvn.period_ms,
                                                           c->u.spvn.qids, c->u.spvn.nq);
    case CARI_CID_GET_IDENT:     return cari_enc_get_ident(buf, cap);
    case CARI_CID_GET_REG:       return cari_enc_get_reg(buf, cap, c->addr);
    case CARI_CID_GET_SUB_CAPS:  return cari_enc_get_sub_caps(buf, cap, c->addr);
    case CARI_CID_GET_PARAM:     return cari_enc_get_param(buf, cap, c->addr, c->u.get_param.param);
    case CARI_CID_GET_SPVN_LIST: return cari_enc_get_spvn_list(buf, cap);
    case CARI_CID_GET_DEV_CAPS:  return cari_enc_get_dev_caps(buf, cap);
    default:                     return -1;
    }
}

/* ---------------------------------------------------------------- replies */

int cari_reply_encode(uint8_t *buf, size_t cap, uint8_t cid, uint8_t status,
                      const uint8_t *data, size_t data_len)
{
    if (status != CARI_OK)
        data_len = 0;           /* an error reply is always 4 bytes */
    size_t len = CARI_ERR_REPLY_LEN + data_len;
    if (len > cap || len > CARI_MAX_FRAME)
        return -1;
    buf[0] = cid;
    cari_put_le16(&buf[1], (uint16_t)len);
    buf[3] = status;
    if (data_len)
        memmove(&buf[4], data, data_len);
    return (int)len;
}

int cari_reply_decode(const uint8_t *buf, size_t n, cari_reply_t *r)
{
    memset(r, 0, sizeof *r);
    if (n < CARI_ERR_REPLY_LEN || n > CARI_MAX_FRAME)
        return -1;
    if (cari_get_le16(&buf[1]) != n)
        return -1;
    r->cid = buf[0];
    r->status = buf[3];
    if (r->status != CARI_OK && n != CARI_ERR_REPLY_LEN)
        return -1;
    r->data = n > 4 ? &buf[4] : NULL;
    r->data_len = n - 4;
    return 0;
}

int cari_reply_expected_data_len(const uint8_t *cmd, size_t cmd_len)
{
    if (cmd_len < CARI_HDR_LEN)
        return -1;
    switch (cmd[0]) {
    case CARI_CID_PING:
        return 4;
    case CARI_CID_SET_REG:
    case CARI_CID_SET_PARAM:
    case CARI_CID_EXEC:
    case CARI_CID_BIND_UL:
    case CARI_CID_BIND_DL:
    case CARI_CID_BIND_SPVN:
        return 0;
    case CARI_CID_GET_REG:
        return 1;
    case CARI_CID_GET_PARAM: {
        if (cmd_len < 5)
            return -1;
        const cari_param_info_t *pi = cari_param_info(cmd[4]);
        return pi ? (int)cari_type_size(pi->type) : -1;
    }
    default:
        return -1;
    }
}

int cari_reply_validate(const uint8_t *cmd, size_t cmd_len, const uint8_t *buf, size_t n,
                        cari_reply_t *r)
{
    if (cari_reply_decode(buf, n, r) < 0)
        return -1;
    uint8_t exp_cid = cmd_len ? cmd[0] : 0xFF;
    if (r->cid != exp_cid)
        return -1;
    if (r->status != CARI_OK)
        return 0;
    int exp = cari_reply_expected_data_len(cmd, cmd_len);
    if (exp >= 0 && (size_t)exp != r->data_len)
        return -1;
    return 0;
}

/* ---------------------------------------------------------------- SPVN */

int cari_spvn_append(uint8_t *buf, size_t cap, size_t len, uint8_t qid, uint8_t sub, float v)
{
    const cari_quantity_info_t *qi = cari_quantity_info(qid);
    if (!qi)
        return -1;
    size_t need = 1 + (qi->per_subdevice ? 1 : 0) + 4;
    if (len + need > cap)
        return -1;
    buf[len++] = qid;
    if (qi->per_subdevice)
        buf[len++] = sub;
    cari_put_f32(&buf[len], v);
    return (int)(len + 4);
}

int cari_spvn_parse(const uint8_t *p, size_t n, cari_spvn_entry_t *out, size_t max)
{
    size_t off = 0;
    int cnt = 0;
    while (off < n) {
        const cari_quantity_info_t *qi = cari_quantity_info(p[off]);
        if (!qi)
            return -1;
        size_t need = 1 + (qi->per_subdevice ? 1 : 0) + 4;
        if (off + need > n)
            return -1;
        if ((size_t)cnt < max) {
            out[cnt].qid = p[off];
            out[cnt].sub = qi->per_subdevice ? p[off + 1] : 0xFF;
            out[cnt].value = cari_get_f32(&p[off + need - 4]);
        }
        cnt++;
        off += need;
    }
    return cnt;
}
