/* libcari sample scaling and FLAC framing tests */
#include "t.h"
#include <cari/cari_proto.h>

static void test_scaling_points(void)
{
    float f[4];
    /* CS16: x = v / 2^15 */
    int16_t cs16[4] = { -32768, 32767, 16384, 0 };
    cari_samples_to_float(CARI_FMT_CS16, cs16, 2, f);
    CHECK(f[0] == -1.0f);
    CHECK_NEAR(f[1], 32767.0 / 32768.0, 1e-9);
    CHECK(f[2] == 0.5f && f[3] == 0.0f);
    /* U8: mid-scale (128) is zero, 0 is -1 */
    uint8_t u8[3] = { 128, 0, 255 };
    cari_samples_to_float(CARI_FMT_U8, u8, 3, f);
    CHECK(f[0] == 0.0f && f[1] == -1.0f);
    CHECK_NEAR(f[2], 127.0 / 128.0, 1e-9);
    /* CU8 interleaved */
    uint8_t cu8[2] = { 192, 64 };
    cari_samples_to_float(CARI_FMT_CU8, cu8, 1, f);
    CHECK(f[0] == 0.5f && f[1] == -0.5f);
    /* S8 / CS8 */
    int8_t s8[2] = { -128, 64 };
    cari_samples_to_float(CARI_FMT_S8, s8, 2, f);
    CHECK(f[0] == -1.0f && f[1] == 0.5f);
    /* float formats are the value itself, little-endian */
    uint8_t cf[8];
    cari_put_f32(cf, 0.25f);
    cari_put_f32(cf + 4, -0.75f);
    cari_samples_to_float(CARI_FMT_CF32, cf, 1, f);
    CHECK(f[0] == 0.25f && f[1] == -0.75f);
}

static void test_scaling_encode(void)
{
    float in[6] = { 1.0f, -1.0f, 0.5f, 2.0f, -3.0f, NAN };
    int16_t s16[6];
    cari_samples_from_float(CARI_FMT_S16, in, 6, s16);
    CHECK(s16[0] == 32767 && s16[1] == -32768 && s16[2] == 16384);
    CHECK(s16[3] == 32767 && s16[4] == -32768 && s16[5] == 0);   /* clipped, NaN -> 0 */
    uint8_t u8[6];
    cari_samples_from_float(CARI_FMT_U8, in, 6, u8);
    CHECK(u8[0] == 255 && u8[1] == 0 && u8[2] == 192 && u8[5] == 128);

    /* round trip of every 8-bit code in every 8-bit format */
    for (int fmt = CARI_FMT_CS8; fmt <= CARI_FMT_U8; fmt++) {
        uint8_t raw[256], back[256];
        float tmp[256];
        for (int i = 0; i < 256; i++)
            raw[i] = (uint8_t)i;
        size_t n = 256 / cari_format_bytes((uint8_t)fmt);
        cari_samples_to_float((uint8_t)fmt, raw, n, tmp);
        cari_samples_from_float((uint8_t)fmt, tmp, n, back);
        CHECK(!memcmp(raw, back, 256));
    }
    /* round trip of CS16 */
    int16_t r16[512], b16[512];
    float t16[512];
    for (int i = 0; i < 512; i++)
        r16[i] = (int16_t)(i * 127 - 32768);
    cari_samples_to_float(CARI_FMT_CS16, r16, 256, t16);
    cari_samples_from_float(CARI_FMT_CS16, t16, 256, b16);
    CHECK(!memcmp(r16, b16, sizeof r16));
}

static void test_int32(void)
{
    uint8_t cu8[6] = { 0, 255, 128, 129, 10, 20 };
    int32_t i[3], q[3];
    int32_t *const ch[2] = { i, q };
    CHECK_EQ(cari_samples_to_int32(CARI_FMT_CU8, cu8, 3, ch), 0);
    CHECK(i[0] == -128 && q[0] == 127 && i[1] == 0 && q[1] == 1);
    uint8_t back[6];
    cari_samples_from_int32(CARI_FMT_CU8, (const int32_t *const *)ch, 3, back);
    CHECK(!memcmp(cu8, back, 6));
    CHECK_EQ(cari_samples_to_int32(CARI_FMT_CF32, cu8, 1, ch), -1);
    /* clipping on output */
    i[0] = 40000; q[0] = -40000;
    int16_t o[2];
    cari_samples_from_int32(CARI_FMT_CS16, (const int32_t *const *)ch, 1, o);
    CHECK(o[0] == 32767 && o[1] == -32768);
}

static void test_flac_rate(void)
{
    CHECK(cari_flac_rate_ok(125000));
    CHECK(cari_flac_rate_ok(250000));
    CHECK(cari_flac_rate_ok(500000));
    CHECK(cari_flac_rate_ok(48000));
    CHECK(cari_flac_rate_ok(65535));
    CHECK(cari_flac_rate_ok(655350));
    CHECK(!cari_flac_rate_ok(655360));
    CHECK(!cari_flac_rate_ok(65537));       /* > 65535 and not a multiple of 10 */
    CHECK(!cari_flac_rate_ok(1e6));
    CHECK(!cari_flac_rate_ok(44100.5));
    CHECK(!cari_flac_rate_ok(0));
    CHECK(!cari_flac_rate_ok(-8000));
}

static void test_crc(void)
{
    const uint8_t s[] = "123456789";
    CHECK_EQ(cari_flac_crc8(s, 9), 0xF4);       /* CRC-8 poly 0x07 */
    CHECK_EQ(cari_flac_crc16(s, 9), 0xFEE8);    /* CRC-16/UMTS (poly 0x8005, init 0) */
}

/* Build a synthetic but valid FLAC frame: header + one verbatim-free body
 * (contents are irrelevant to the framing helpers) + CRC-16. */
static size_t make_frame(uint8_t *p, uint32_t frame_no, uint8_t sr_code, const uint8_t *extra_rate,
                         size_t body_len, uint8_t fill)
{
    size_t n = 0;
    p[n++] = 0xFF;
    p[n++] = 0xF8;                      /* fixed blocksize */
    p[n++] = (uint8_t)((7 << 4) | sr_code);    /* 16-bit blocksize-1 follows */
    p[n++] = (uint8_t)((1 << 4) | (4 << 1));   /* 2 channels, 16 bit */
    p[n++] = (uint8_t)frame_no;         /* < 128 */
    p[n++] = 0x04; p[n++] = 0xE1;       /* blocksize 1250 */
    if (sr_code == 12) p[n++] = extra_rate[0];
    if (sr_code == 13 || sr_code == 14) { p[n++] = extra_rate[0]; p[n++] = extra_rate[1]; }
    p[n] = cari_flac_crc8(p, n);
    n++;
    memset(p + n, fill, body_len);
    n += body_len;
    uint16_t c = cari_flac_crc16(p, n);
    p[n++] = (uint8_t)(c >> 8);
    p[n++] = (uint8_t)c;
    return n;
}

static void test_flac_header(void)
{
    uint8_t f[64];
    cari_flac_hdr_t h;
    uint8_t r[2] = { 125 };             /* code 12: kHz */
    make_frame(f, 5, 12, r, 10, 0);
    CHECK_EQ(cari_flac_parse_header(f, sizeof f, &h), 0);
    CHECK_EQ(h.block_size, 1250);
    CHECK_EQ(h.sample_rate, 125000);
    CHECK_EQ(h.channels, 2);
    CHECK_EQ(h.bps, 16);
    CHECK_EQ(h.number, 5);
    CHECK_EQ(h.header_len, 9);
    CHECK(cari_flac_hdr_explicit(&h));
    /* tens of Hz: 500 kHz = 50000 * 10 */
    uint8_t r2[2] = { 50000 >> 8, 50000 & 0xFF };
    make_frame(f, 1, 14, r2, 10, 0);
    CHECK_EQ(cari_flac_parse_header(f, sizeof f, &h), 0);
    CHECK_EQ(h.sample_rate, 500000);
    /* "from STREAMINFO" is legal FLAC but not CARI */
    make_frame(f, 1, 0, NULL, 10, 0);
    CHECK_EQ(cari_flac_parse_header(f, sizeof f, &h), 0);
    CHECK(!cari_flac_hdr_explicit(&h));
    /* corrupt CRC-8 */
    make_frame(f, 1, 12, r, 10, 0);
    f[4] ^= 1;
    CHECK_EQ(cari_flac_parse_header(f, sizeof f, &h), -1);
    /* truncated */
    make_frame(f, 1, 12, r, 10, 0);
    CHECK_EQ(cari_flac_parse_header(f, 6, &h), -2);
    /* not a sync */
    f[1] = 0xFA;
    CHECK_EQ(cari_flac_parse_header(f, sizeof f, &h), -1);
}

static void test_flac_split(void)
{
    uint8_t msg[512];
    uint8_t r[1] = { 250 };
    size_t n = 0, skipped;
    cari_flac_span_t sp[8];

    /* garbage, then 3 frames; the 2nd frame's body contains a fake sync */
    memcpy(msg, "\x12\x34\xFF\x00\x56", 5);
    n = 5;
    size_t f1 = make_frame(msg + n, 0, 12, r, 30, 0x55);
    n += f1;
    size_t f2off = n;
    size_t f2 = make_frame(msg + n, 1, 12, r, 40, 0xAA);
    /* plant a header-looking sequence inside frame 2's body and refresh CRC */
    uint8_t fake[16];
    size_t fl = make_frame(fake, 9, 12, r, 0, 0);
    memcpy(msg + f2off + 12, fake, fl - 2);
    uint16_t c = cari_flac_crc16(msg + f2off, f2 - 2);
    msg[f2off + f2 - 2] = (uint8_t)(c >> 8);
    msg[f2off + f2 - 1] = (uint8_t)c;
    n += f2;
    size_t f3 = make_frame(msg + n, 2, 12, r, 7, 0x01);
    n += f3;

    size_t cnt = cari_flac_split(msg, n, sp, 8, &skipped);
    CHECK_EQ(cnt, 3);
    CHECK_EQ(skipped, 5);
    CHECK_EQ(sp[0].off, 5);
    CHECK_EQ(sp[0].len, f1);
    CHECK_EQ(sp[1].len, f2);
    CHECK_EQ(sp[1].hdr.number, 1);
    CHECK_EQ(sp[2].len, f3);

    /* joining mid-frame: start in the middle of frame 1 */
    cnt = cari_flac_split(msg + 10, n - 10, sp, 8, &skipped);
    CHECK_EQ(cnt, 2);
    CHECK_EQ(sp[0].hdr.number, 1);

    /* a damaged frame is skipped, the next one survives */
    msg[5 + 20] ^= 0xFF;
    cnt = cari_flac_split(msg, n, sp, 8, &skipped);
    CHECK_EQ(cnt, 2);
    CHECK_EQ(sp[0].hdr.number, 1);
    CHECK(skipped >= 5 + f1);

    /* capacity limit */
    msg[5 + 20] ^= 0xFF;
    cnt = cari_flac_split(msg, n, sp, 1, &skipped);
    CHECK_EQ(cnt, 1);
    cnt = cari_flac_split(msg, 0, sp, 8, &skipped);
    CHECK_EQ(cnt, 0);
}

int main(void)
{
    RUN(test_scaling_points);
    RUN(test_scaling_encode);
    RUN(test_int32);
    RUN(test_flac_rate);
    RUN(test_crc);
    RUN(test_flac_header);
    RUN(test_flac_split);
    T_DONE();
}
