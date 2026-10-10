/* libcari codec layer tests: frames, replies, TLV capabilities, parameter
 * rules, supervision packets, tables, values. */
#include "t.h"
#include <cari/cari_proto.h>

static uint8_t buf[256];

static void test_le(void)
{
    uint8_t b[8];
    cari_put_le16(b, 0x1234);
    CHECK(b[0] == 0x34 && b[1] == 0x12);
    cari_put_le64(b, 433475000ull);
    CHECK_EQ(cari_get_le64(b), 433475000ull);
    cari_put_f32(b, -3.5f);
    CHECK(cari_get_f32(b) == -3.5f);
    /* IEEE-754 little-endian: 1.0f = 0x3F800000 */
    cari_put_f32(b, 1.0f);
    CHECK(b[0] == 0 && b[1] == 0 && b[2] == 0x80 && b[3] == 0x3F);
}

static void test_encode_lengths(void)
{
    cari_value_t f = cari_value_f32(1.0f), u = cari_value_u64(1), b = cari_value_bool(true);
    uint8_t q[3] = { 0, 1, 2 };
    CHECK_EQ(cari_enc_ping(buf, sizeof buf), 3);
    CHECK_EQ(cari_enc_set_reg(buf, sizeof buf, 0, 0), 5);
    CHECK_EQ(cari_enc_set_param(buf, sizeof buf, 0, CARI_P_RX_FREQ, &u), 13);
    CHECK_EQ(cari_enc_set_param(buf, sizeof buf, 0, CARI_P_LNA_GAIN, &f), 9);
    CHECK_EQ(cari_enc_set_param(buf, sizeof buf, 0, CARI_P_AGC, &b), 6);
    CHECK_EQ(cari_enc_exec(buf, sizeof buf, 0, 1), 5);
    CHECK_EQ(cari_enc_bind(buf, sizeof buf, CARI_CID_BIND_UL, 0, 17004), 6);
    CHECK_EQ(cari_enc_bind(buf, sizeof buf, CARI_CID_BIND_DL, 0, 17005), 6);
    CHECK_EQ(cari_enc_bind(buf, sizeof buf, CARI_CID_PING, 0, 17005), -1);
    CHECK_EQ(cari_enc_bind_spvn(buf, sizeof buf, 17003, 1000, q, 3), 10);
    CHECK_EQ(cari_enc_get_ident(buf, sizeof buf), 3);
    CHECK_EQ(cari_enc_get_reg(buf, sizeof buf, 1), 4);
    CHECK_EQ(cari_enc_get_sub_caps(buf, sizeof buf, 0), 4);
    CHECK_EQ(cari_enc_get_param(buf, sizeof buf, 0, 5), 5);
    CHECK_EQ(cari_enc_get_spvn_list(buf, sizeof buf), 3);
    CHECK_EQ(cari_enc_get_dev_caps(buf, sizeof buf), 3);
    /* byte count field is little-endian and counts everything */
    cari_enc_bind_spvn(buf, sizeof buf, 0x1122, 0x3344, q, 3);
    CHECK(buf[0] == 0x06 && buf[1] == 10 && buf[2] == 0);
    CHECK(buf[3] == 0x22 && buf[4] == 0x11 && buf[5] == 0x44 && buf[6] == 0x33);
    /* buffer too small */
    CHECK_EQ(cari_enc_set_param(buf, 12, 0, CARI_P_RX_FREQ, &u), -1);
}

static void test_decode_roundtrip(void)
{
    cari_cmd_t c;
    cari_value_t u = cari_value_u64(435000000ull);
    int n = cari_enc_set_param(buf, sizeof buf, 3, CARI_P_TX_FREQ, &u);
    CHECK_EQ(cari_cmd_decode(buf, n, &c), CARI_OK);
    CHECK(c.cid == CARI_CID_SET_PARAM && c.has_addr && c.addr == 3);
    CHECK(c.u.set_param.param == CARI_P_TX_FREQ);
    CHECK(c.u.set_param.value.type == CARI_T_U64 && c.u.set_param.value.v.u64 == 435000000ull);

    uint8_t again[32];
    CHECK_EQ(cari_cmd_encode(again, sizeof again, &c), n);
    CHECK(!memcmp(again, buf, n));

    uint8_t q[2] = { CARI_Q_TEMPERATURE, CARI_Q_RETURN_LOSS };
    n = cari_enc_bind_spvn(buf, sizeof buf, 17003, 250, q, 2);
    CHECK_EQ(cari_cmd_decode(buf, n, &c), CARI_OK);
    CHECK(!c.has_addr && c.u.spvn.port == 17003 && c.u.spvn.period_ms == 250 && c.u.spvn.nq == 2);
    CHECK(c.u.spvn.qids[1] == CARI_Q_RETURN_LOSS);

    n = cari_enc_bind(buf, sizeof buf, CARI_CID_BIND_DL, 0, 0);
    CHECK_EQ(cari_cmd_decode(buf, n, &c), CARI_OK);
    CHECK(c.u.bind.port == 0 && c.addr == 0);

    n = cari_enc_get_param(buf, sizeof buf, 1, CARI_P_TX_STATE);
    CHECK_EQ(cari_cmd_decode(buf, n, &c), CARI_OK);
    CHECK(c.u.get_param.param == CARI_P_TX_STATE && c.addr == 1);
}

static void test_decode_malformed(void)
{
    cari_cmd_t c;
    /* empty message: reply CID 0xFF */
    CHECK_EQ(cari_cmd_decode(buf, 0, &c), CARI_S_MALFORMED);
    CHECK_EQ(c.cid, 0xFF);
    /* < 3 bytes */
    uint8_t m1[2] = { 0x80, 0x02 };
    CHECK_EQ(cari_cmd_decode(m1, 2, &c), CARI_S_MALFORMED);
    CHECK_EQ(c.cid, 0x80);
    /* byte count mismatch */
    uint8_t m2[4] = { 0x00, 0x03, 0x00, 0xAA };
    CHECK_EQ(cari_cmd_decode(m2, 4, &c), CARI_S_MALFORMED);
    uint8_t m3[3] = { 0x00, 0x04, 0x00 };
    CHECK_EQ(cari_cmd_decode(m3, 3, &c), CARI_S_MALFORMED);
    /* big-endian count is wrong */
    uint8_t m4[5] = { 0x81, 0x00, 0x05, 0x00, 0x00 };
    CHECK_EQ(cari_cmd_decode(m4, 5, &c), CARI_S_MALFORMED);
    /* length wrong for the CID (valid byte count) */
    uint8_t m5[4] = { 0x00, 0x04, 0x00, 0x00 };          /* ping with 1 extra byte */
    CHECK_EQ(cari_cmd_decode(m5, 4, &c), CARI_S_MALFORMED);
    CHECK_EQ(c.cid, 0x00);
    uint8_t m6[3] = { 0x81, 0x03, 0x00 };                /* get_reg missing reg */
    CHECK_EQ(cari_cmd_decode(m6, 3, &c), CARI_S_MALFORMED);
    uint8_t m7[6] = { 0x06, 0x06, 0x00, 1, 2, 3 };       /* spvn too short */
    CHECK_EQ(cari_cmd_decode(m7, 6, &c), CARI_S_MALFORMED);
    /* set_param: known param, wrong value size */
    uint8_t m8[9] = { 0x02, 0x09, 0x00, 0x00, CARI_P_RX_FREQ, 1, 2, 3, 4 };
    CHECK_EQ(cari_cmd_decode(m8, 9, &c), CARI_S_MALFORMED);
    uint8_t m9[4] = { 0x02, 0x04, 0x00, 0x00 };          /* no param ID */
    CHECK_EQ(cari_cmd_decode(m9, 4, &c), CARI_S_MALFORMED);
    /* set_param: reserved parameter -> unsupported */
    uint8_t m10[6] = { 0x02, 0x06, 0x00, 0x00, 0x0F, 0x00 };
    CHECK_EQ(cari_cmd_decode(m10, 6, &c), CARI_S_UNSUPPORTED);
    /* unknown CIDs -> unsupported (status 2), reply CID echoes */
    uint8_t m11[3] = { 0x07, 0x03, 0x00 };
    CHECK_EQ(cari_cmd_decode(m11, 3, &c), CARI_S_UNSUPPORTED);
    CHECK_EQ(c.cid, 0x07);
    uint8_t m12[3] = { 0xFF, 0x03, 0x00 };
    CHECK_EQ(cari_cmd_decode(m12, 3, &c), CARI_S_UNSUPPORTED);
    uint8_t m13[3] = { 0x86, 0x03, 0x00 };
    CHECK_EQ(cari_cmd_decode(m13, 3, &c), CARI_S_UNSUPPORTED);
    /* an unknown CID with a bad count is still malformed first */
    uint8_t m14[3] = { 0x07, 0x09, 0x00 };
    CHECK_EQ(cari_cmd_decode(m14, 3, &c), CARI_S_MALFORMED);
}

static void test_replies(void)
{
    cari_reply_t r;
    uint8_t data[4] = { 1, 2, 3, 4 };
    int n = cari_reply_encode(buf, sizeof buf, 0x00, CARI_OK, data, 4);
    CHECK_EQ(n, 8);                                      /* Pong is 8 bytes */
    CHECK(buf[1] == 8 && buf[2] == 0 && buf[3] == 0);
    /* error replies are always 4 bytes even if data is given */
    n = cari_reply_encode(buf, sizeof buf, 0x83, CARI_S_OUT_OF_RANGE, data, 4);
    CHECK_EQ(n, 4);
    CHECK_EQ(cari_reply_decode(buf, n, &r), 0);
    CHECK(r.cid == 0x83 && r.status == 5 && r.data_len == 0);

    /* error reply with trailing data is invalid */
    uint8_t bad[5] = { 0x83, 5, 0, 5, 9 };
    CHECK_EQ(cari_reply_decode(bad, 5, &r), -1);

    /* validation against the command */
    uint8_t cmd[16];
    int cl = cari_enc_get_param(cmd, sizeof cmd, 0, CARI_P_RX_FREQ);
    uint8_t v[8] = { 0 };
    n = cari_reply_encode(buf, sizeof buf, CARI_CID_GET_PARAM, CARI_OK, v, 8);
    CHECK_EQ(cari_reply_validate(cmd, cl, buf, n, &r), 0);
    n = cari_reply_encode(buf, sizeof buf, CARI_CID_GET_PARAM, CARI_OK, v, 4);
    CHECK_EQ(cari_reply_validate(cmd, cl, buf, n, &r), -1);       /* wrong size */
    n = cari_reply_encode(buf, sizeof buf, CARI_CID_GET_REG, CARI_OK, v, 8);
    CHECK_EQ(cari_reply_validate(cmd, cl, buf, n, &r), -1);       /* wrong CID */
    n = cari_reply_encode(buf, sizeof buf, CARI_CID_GET_PARAM, CARI_S_UNSUPPORTED, NULL, 0);
    CHECK_EQ(cari_reply_validate(cmd, cl, buf, n, &r), 0);
    CHECK_EQ(r.status, CARI_S_UNSUPPORTED);

    cl = cari_enc_ping(cmd, sizeof cmd);
    n = cari_reply_encode(buf, sizeof buf, 0, CARI_OK, v, 4);
    CHECK_EQ(cari_reply_validate(cmd, cl, buf, n, &r), 0);
    n = cari_reply_encode(buf, sizeof buf, 0, CARI_OK, NULL, 0);
    CHECK_EQ(cari_reply_validate(cmd, cl, buf, n, &r), -1);       /* Pong w/o flags */

    cl = cari_enc_get_ident(cmd, sizeof cmd);
    n = cari_reply_encode(buf, sizeof buf, 0x80, CARI_OK, (const uint8_t *)"abc", 3);
    CHECK_EQ(cari_reply_validate(cmd, cl, buf, n, &r), 0);
    CHECK_EQ(r.data_len, 3);
}

static void test_tlv(void)
{
    cari_tlv_writer_t w;
    cari_caps_t c;
    cari_tlv_init(&w, buf, sizeof buf);
    cari_tlv_flag(&w, CARI_SCAP_IQ);
    cari_tlv_flag(&w, CARI_SCAP_RX);
    cari_tlv_flag(&w, CARI_SCAP_FMT_CS16);
    cari_tlv_range_u64(&w, CARI_SCAP_RX_FREQ, 400000000ull, 510000000ull);
    cari_tlv_value_f32(&w, CARI_SCAP_SAMPLE_RATE, 125000.0f);
    cari_tlv_value_f32(&w, CARI_SCAP_SAMPLE_RATE, 250000.0f);
    cari_tlv_raw(&w, 0x13, (const uint8_t *)"\x01\x02", 2);   /* reserved explicit: skipped */
    cari_tlv_raw(&w, 0x99, (const uint8_t *)"\x01\x02\x03", 3); /* reserved ranged: skipped */
    CHECK(!w.overflow);
    CHECK_EQ(w.len, 3 * 2 + 2 * 10 + 4 * 6 + 4 + 5);
    /* the record layout: ID, length, value */
    CHECK(buf[6] == CARI_SCAP_RX_FREQ && buf[7] == 8);

    CHECK_EQ(cari_caps_parse(buf, w.len, true, &c), 0);
    CHECK(cari_caps_has_flag(&c, CARI_SCAP_IQ));
    CHECK(cari_caps_has_flag(&c, CARI_SCAP_FMT_CS16));
    CHECK(!cari_caps_has_flag(&c, CARI_SCAP_TX));
    CHECK_EQ(c.nranges, 3);
    CHECK_EQ(c.unknown_skipped, 2);
    cari_value_t v = cari_value_u64(433000000ull);
    CHECK(cari_caps_in_range(&c, CARI_SCAP_RX_FREQ, &v, false));
    v = cari_value_u64(399999999ull);
    CHECK(!cari_caps_in_range(&c, CARI_SCAP_RX_FREQ, &v, false));
    v = cari_value_f32(250000.0f);
    CHECK(cari_caps_in_range(&c, CARI_SCAP_SAMPLE_RATE, &v, false));
    v = cari_value_f32(200000.0f);
    CHECK(!cari_caps_in_range(&c, CARI_SCAP_SAMPLE_RATE, &v, false));
    CHECK(cari_caps_in_range(&c, CARI_SCAP_LNA_GAIN, &v, true));    /* none declared */

    /* round trip through cari_caps_encode */
    uint8_t b2[256];
    int n2 = cari_caps_encode(&c, b2, sizeof b2);
    cari_caps_t c2;
    CHECK(n2 > 0);
    CHECK_EQ(cari_caps_parse(b2, n2, true, &c2), 0);
    CHECK_EQ(c2.nranges, 3);
    CHECK(!memcmp(c.flags, c2.flags, sizeof c.flags));

    /* errors: unpaired range, wrong size, truncated record, explicit with value */
    cari_tlv_init(&w, buf, sizeof buf);
    uint8_t lo[8] = { 0 };
    cari_tlv_raw(&w, CARI_SCAP_RX_FREQ, lo, 8);
    CHECK_EQ(cari_caps_parse(buf, w.len, true, &c), -1);
    cari_tlv_init(&w, buf, sizeof buf);
    cari_tlv_raw(&w, CARI_SCAP_LNA_GAIN, lo, 8);
    cari_tlv_raw(&w, CARI_SCAP_LNA_GAIN, lo, 8);
    CHECK_EQ(cari_caps_parse(buf, w.len, true, &c), -1);
    uint8_t trunc[3] = { CARI_SCAP_LNA_GAIN, 4, 0 };
    CHECK_EQ(cari_caps_parse(trunc, 3, true, &c), -1);
    uint8_t expl[3] = { CARI_SCAP_RX, 1, 0 };
    CHECK_EQ(cari_caps_parse(expl, 3, true, &c), -1);
    /* mixed IDs in a pair */
    cari_tlv_init(&w, buf, sizeof buf);
    cari_put_f32(lo, 1.0f);
    cari_tlv_raw(&w, CARI_SCAP_LNA_GAIN, lo, 4);
    cari_tlv_raw(&w, CARI_SCAP_TX_POWER, lo, 4);
    CHECK_EQ(cari_caps_parse(buf, w.len, true, &c), -1);
    /* writer rejects mistyped ranges */
    cari_tlv_init(&w, buf, sizeof buf);
    CHECK_EQ(cari_tlv_range_f32(&w, CARI_SCAP_RX_FREQ, 1, 2), -1);
    CHECK(w.overflow);
    cari_tlv_init(&w, buf, 3);
    CHECK_EQ(cari_tlv_range_f32(&w, CARI_SCAP_LNA_GAIN, 1, 2), -1);

    /* device capability list */
    cari_tlv_init(&w, buf, sizeof buf);
    cari_tlv_flag(&w, CARI_DCAP_COMPRESSION);
    cari_tlv_flag(&w, CARI_DCAP_SUPERVISION);
    cari_tlv_flag(&w, 0x10);
    CHECK_EQ(cari_caps_parse(buf, w.len, false, &c), 0);
    CHECK(cari_caps_has_flag(&c, CARI_DCAP_COMPRESSION) && cari_caps_has_flag(&c, CARI_DCAP_SUPERVISION));
    CHECK_EQ(c.unknown_skipped, 1);
}

static void make_sub_caps(cari_caps_t *c, bool rx, bool tx)
{
    cari_caps_clear(c);
    cari_caps_set_flag(c, CARI_SCAP_IQ);
    if (rx) cari_caps_set_flag(c, CARI_SCAP_RX);
    if (tx) cari_caps_set_flag(c, CARI_SCAP_TX);
    cari_caps_set_flag(c, CARI_SCAP_FMT_CS16);
    cari_caps_set_flag(c, CARI_SCAP_FM_MOD);
    cari_caps_add_range(c, CARI_SCAP_RX_FREQ, cari_value_u64(400000000), cari_value_u64(510000000));
    cari_caps_add_range(c, CARI_SCAP_TX_FREQ, cari_value_u64(400000000), cari_value_u64(510000000));
    cari_caps_add_range(c, CARI_SCAP_LNA_GAIN, cari_value_f32(-48), cari_value_f32(-48));
    cari_caps_add_range(c, CARI_SCAP_LNA_GAIN, cari_value_f32(0), cari_value_f32(0));
    cari_caps_add_range(c, CARI_SCAP_SAMPLE_RATE, cari_value_f32(125000), cari_value_f32(125000));
    cari_caps_add_range(c, CARI_SCAP_FM_DEV, cari_value_f32(100), cari_value_f32(5000));
}

static void test_param_check(void)
{
    cari_caps_t s, dev;
    make_sub_caps(&s, true, false);
    cari_caps_clear(&dev);
    cari_value_t v;

    v = cari_value_u64(433000000);
    CHECK_EQ(cari_param_check(&s, &dev, CARI_P_RX_FREQ, &v, 0), CARI_OK);
    CHECK_EQ(cari_param_check(&s, &dev, CARI_P_TX_FREQ, &v, 0), CARI_S_UNSUPPORTED); /* RX-only */
    v = cari_value_u64(520000000);
    CHECK_EQ(cari_param_check(&s, &dev, CARI_P_RX_FREQ, &v, 0), CARI_S_OUT_OF_RANGE);
    v = cari_value_f32(0);
    CHECK_EQ(cari_param_check(&s, &dev, CARI_P_LNA_GAIN, &v, 0), CARI_OK);
    v = cari_value_f32(-6);
    CHECK_EQ(cari_param_check(&s, &dev, CARI_P_LNA_GAIN, &v, 0), CARI_S_OUT_OF_RANGE);
    v = cari_value_f32(NAN);
    CHECK_EQ(cari_param_check(&s, &dev, CARI_P_LNA_GAIN, &v, 0), CARI_S_OUT_OF_RANGE);
    v = cari_value_f32(125000);
    CHECK_EQ(cari_param_check(&s, &dev, CARI_P_SAMPLE_RATE, &v, 0), CARI_OK);
    v = cari_value_f32(125001);
    CHECK_EQ(cari_param_check(&s, &dev, CARI_P_SAMPLE_RATE, &v, 0), CARI_S_OUT_OF_RANGE);
    v = cari_value_u8(CARI_FMT_CS16);
    CHECK_EQ(cari_param_check(&s, &dev, CARI_P_FORMAT, &v, 0), CARI_OK);
    v = cari_value_u8(CARI_FMT_CF32);
    CHECK_EQ(cari_param_check(&s, &dev, CARI_P_FORMAT, &v, 0), CARI_S_OUT_OF_RANGE);
    v = cari_value_u8(0x30);
    CHECK_EQ(cari_param_check(&s, &dev, CARI_P_FORMAT, &v, 0), CARI_S_OUT_OF_RANGE);
    v = cari_value_u8(CARI_MODE_IQ);
    CHECK_EQ(cari_param_check(&s, &dev, CARI_P_MODE, &v, 0), CARI_OK);
    v = cari_value_u8(CARI_MODE_FM);
    CHECK_EQ(cari_param_check(&s, &dev, CARI_P_MODE, &v, 0), CARI_OK);
    v = cari_value_u8(CARI_MODE_AM);
    CHECK_EQ(cari_param_check(&s, &dev, CARI_P_MODE, &v, 0), CARI_S_OUT_OF_RANGE);
    v = cari_value_u8(0x06);
    CHECK_EQ(cari_param_check(&s, &dev, CARI_P_MODE, &v, 0), CARI_S_OUT_OF_RANGE);
    v = cari_value_bool(true);
    CHECK_EQ(cari_param_check(&s, &dev, CARI_P_AGC, &v, 0), CARI_S_UNSUPPORTED);
    CHECK_EQ(cari_param_check(&s, &dev, CARI_P_COMPRESSION, &v, 0), CARI_S_UNSUPPORTED);
    cari_caps_set_flag(&dev, CARI_DCAP_COMPRESSION);
    CHECK_EQ(cari_param_check(&s, &dev, CARI_P_COMPRESSION, &v, 0), CARI_OK);
    v.v.u8 = 2;                                              /* boolean 2 -> 5 */
    CHECK_EQ(cari_param_check(&s, &dev, CARI_P_COMPRESSION, &v, 0), CARI_S_OUT_OF_RANGE);
    v = cari_value_u8(0);
    CHECK_EQ(cari_param_check(&s, &dev, CARI_P_TX_STATE, &v, 0), CARI_S_READ_ONLY);
    /* deviation is bounded by the FM range only in FM mode */
    v = cari_value_f32(10000);
    CHECK_EQ(cari_param_check(&s, &dev, CARI_P_DEVIATION, &v, CARI_MODE_FM), CARI_S_OUT_OF_RANGE);
    /* CARI 2.1: deviation outside FM/PM is a state error (value checks first) */
    CHECK_EQ(cari_param_check(&s, &dev, CARI_P_DEVIATION, &v, CARI_MODE_IQ), CARI_S_INVALID_STATE);
    CHECK_EQ(cari_param_check(&s, &dev, CARI_P_DEVIATION, &v, CARI_MODE_AM), CARI_S_INVALID_STATE);
    v = cari_value_f32(NAN);
    CHECK_EQ(cari_param_check(&s, &dev, CARI_P_DEVIATION, &v, CARI_MODE_IQ), CARI_S_OUT_OF_RANGE);
    v = cari_value_f32(2500);
    CHECK_EQ(cari_param_check(&s, &dev, CARI_P_DEVIATION, &v, CARI_MODE_FM), CARI_OK);
    /* frequency correction has no capability: accepted here, device decides */
    v = cari_value_f32(1.5f);
    CHECK_EQ(cari_param_check(&s, &dev, CARI_P_FREQ_CORR, &v, 0), CARI_OK);
    /* type mismatch */
    v = cari_value_f32(1.0f);
    CHECK_EQ(cari_param_check(&s, &dev, CARI_P_RX_FREQ, &v, 0), CARI_S_MALFORMED);
}

static void test_combo(void)
{
    CHECK_EQ(cari_stream_combo_check(CARI_FMT_CS16, CARI_MODE_IQ, false, 250000), CARI_OK);
    CHECK_EQ(cari_stream_combo_check(CARI_FMT_S16, CARI_MODE_IQ, false, 250000), CARI_S_INVALID_STATE);
    CHECK_EQ(cari_stream_combo_check(CARI_FMT_CS16, CARI_MODE_FM, false, 48000), CARI_S_INVALID_STATE);
    CHECK_EQ(cari_stream_combo_check(CARI_FMT_S16, CARI_MODE_FM, false, 48000), CARI_OK);
    CHECK_EQ(cari_stream_combo_check(CARI_FMT_CS16, CARI_MODE_IQ, true, 500000), CARI_OK);
    CHECK_EQ(cari_stream_combo_check(CARI_FMT_CF32, CARI_MODE_IQ, true, 500000), CARI_S_INVALID_STATE);
    CHECK_EQ(cari_stream_combo_check(CARI_FMT_CS16, CARI_MODE_IQ, true, 700000), CARI_S_INVALID_STATE);
    CHECK_EQ(cari_stream_combo_check(CARI_FMT_CS16, CARI_MODE_IQ, true, 65537), CARI_S_INVALID_STATE);
    CHECK_EQ(cari_stream_combo_check(CARI_FMT_CS16, CARI_MODE_IQ, true, 48000.5), CARI_S_INVALID_STATE);
    CHECK_EQ(cari_stream_combo_check(CARI_FMT_CU8, CARI_MODE_IQ, true, 65535), CARI_OK);
}

static void test_spvn(void)
{
    int n = 0;
    n = cari_spvn_append(buf, sizeof buf, n, CARI_Q_TEMPERATURE, 0xFF, 41.5f);
    n = cari_spvn_append(buf, sizeof buf, n, CARI_Q_RETURN_LOSS, 2, 18.0f);
    n = cari_spvn_append(buf, sizeof buf, n, CARI_Q_VOLTAGE, 0xFF, 5.0f);
    CHECK_EQ(n, 5 + 6 + 5);
    CHECK(buf[5] == CARI_Q_RETURN_LOSS && buf[6] == 2);
    cari_spvn_entry_t e[4];
    CHECK_EQ(cari_spvn_parse(buf, n, e, 4), 3);
    CHECK(e[0].qid == 0 && e[0].sub == 0xFF && e[0].value == 41.5f);
    CHECK(e[1].qid == 3 && e[1].sub == 2 && e[1].value == 18.0f);
    CHECK_EQ(cari_spvn_parse(buf, n - 1, e, 4), -1);
    buf[0] = 0x20;
    CHECK_EQ(cari_spvn_parse(buf, n, e, 4), -1);
    CHECK_EQ(cari_spvn_append(buf, sizeof buf, 0, 0x06, 0, 1), -1);
}

static void test_tables_values(void)
{
    CHECK(cari_param_by_key("tx_power")->id == CARI_P_TX_POWER);
    CHECK(cari_param_info(CARI_P_SAMPLE_RATE)->flags & CARI_PF_STREAM_LOCKED);
    CHECK(cari_param_info(CARI_P_FORMAT)->flags & CARI_PF_STREAM_LOCKED);
    CHECK(cari_param_info(CARI_P_MODE)->flags & CARI_PF_STREAM_LOCKED);
    CHECK(cari_param_info(CARI_P_COMPRESSION)->flags & CARI_PF_STREAM_LOCKED);
    CHECK(!(cari_param_info(CARI_P_RX_FREQ)->flags & CARI_PF_STREAM_LOCKED));
    CHECK(cari_param_info(0x0F) == NULL);
    CHECK(cari_format_by_name("cs16")->bytes == 4);
    CHECK_EQ(cari_format_bytes(CARI_FMT_CF32), 8);
    CHECK_EQ(cari_format_bytes(CARI_FMT_U8), 1);
    CHECK(cari_quantity_info(CARI_Q_POWER_REFLECTED)->per_subdevice);
    CHECK(!cari_quantity_info(CARI_Q_CURRENT)->per_subdevice);
    CHECK(cari_subcap_info(CARI_SCAP_PM_DEV)->type == CARI_T_F32);
    CHECK(cari_subcap_info(CARI_SCAP_TX_FREQ)->type == CARI_T_U64);
    CHECK(cari_action_by_key("clear_trip")->id == CARI_ACT_CLEAR_TRIP);
    CHECK_EQ(CARI_VERSION_BYTE, 0x21);

    cari_value_t v;
    CHECK_EQ(cari_value_parse(CARI_T_U64, "433.475e6", &v), 0);
    CHECK_EQ(v.v.u64, 433475000ull);
    CHECK_EQ(cari_value_parse(CARI_T_U64, "433475001", &v), 0);
    CHECK_EQ(v.v.u64, 433475001ull);
    CHECK_EQ(cari_value_parse(CARI_T_U64, "-1", &v), -1);
    CHECK_EQ(cari_value_parse(CARI_T_U64, "1.5", &v), -1);
    CHECK_EQ(cari_value_parse(CARI_T_F32, "-12.5", &v), 0);
    CHECK(v.v.f32 == -12.5f);
    CHECK_EQ(cari_value_parse(CARI_T_BOOL, "on", &v), 0);
    CHECK_EQ(v.v.u8, 1);
    CHECK_EQ(cari_value_parse(CARI_T_U8, "0x41", &v), 0);
    CHECK_EQ(v.v.u8, 0x41);
    CHECK_EQ(cari_value_parse(CARI_T_U8, "256", &v), -1);
    char s[32];
    v = cari_value_u64(435000000);
    cari_value_format(&v, s, sizeof s);
    CHECK(!strcmp(s, "435000000"));
}

int main(void)
{
    RUN(test_le);
    RUN(test_encode_lengths);
    RUN(test_decode_roundtrip);
    RUN(test_decode_malformed);
    RUN(test_replies);
    RUN(test_tlv);
    RUN(test_param_check);
    RUN(test_combo);
    RUN(test_spvn);
    RUN(test_tables_values);
    T_DONE();
}
