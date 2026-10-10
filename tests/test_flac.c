/* libcari FLAC compression tests (libFLAC wrapper + framing helpers) */
#include "t.h"
#include <cari/cari_flac.h>
#include <cari/cari_proto.h>

#include <stdint.h>

static void gen(uint8_t fmt, uint8_t *dst, size_t n, double phase0)
{
    const cari_format_info_t *fi = cari_format_info(fmt);
    float *f = malloc(n * fi->channels * sizeof(float));
    for (size_t i = 0; i < n; i++) {
        double ph = phase0 + 2 * M_PI * 0.013 * (double)i;
        f[i * fi->channels] = (float)(0.7 * cos(ph));
        if (fi->channels == 2)
            f[i * 2 + 1] = (float)(0.7 * sin(ph));
    }
    cari_samples_from_float(fmt, f, n, dst);
    free(f);
}

static void roundtrip(uint8_t fmt, uint32_t rate, uint32_t bs, size_t chunk, size_t total)
{
    const cari_format_info_t *fi = cari_format_info(fmt);
    size_t bps = fi->bytes;
    uint8_t *src = malloc(total * bps);
    gen(fmt, src, total, 0.3);

    cari_flac_enc_t *e = cari_flac_enc_new(fmt, rate, bs, 5);
    cari_flac_dec_t *d = cari_flac_dec_new(fmt, rate);
    CHECK(e && d);
    if (!e || !d)
        return;
    uint8_t *got = malloc(total * bps + 1);
    size_t got_n = 0, msgs = 0, comp = 0;
    for (size_t off = 0; off < total; off += chunk) {
        size_t n = total - off < chunk ? total - off : chunk;
        CHECK_EQ(cari_flac_enc_push(e, src + off * bps, n), 0);
        if (off + n >= total)
            CHECK_EQ(cari_flac_enc_finish(e), 0);
        size_t len;
        const uint8_t *m = cari_flac_enc_take(e, &len);
        if (!m)
            continue;
        msgs++;
        comp += len;
        /* every message is whole frames with explicit rate/bps */
        cari_flac_span_t sp[64];
        size_t skipped;
        size_t cnt = cari_flac_split(m, len, sp, 64, &skipped);
        CHECK(cnt >= 1);
        CHECK_EQ(skipped, 0);
        CHECK_EQ(sp[cnt - 1].off + sp[cnt - 1].len, len);
        CHECK(m[0] == 0xFF);                        /* no "fLaC" marker */
        for (size_t i = 0; i < cnt; i++) {
            CHECK(cari_flac_hdr_explicit(&sp[i].hdr));
            CHECK_EQ(sp[i].hdr.sample_rate, rate);
            CHECK_EQ(sp[i].hdr.bps, fi->bits);
            CHECK_EQ(sp[i].hdr.channels, fi->channels);
        }
        int fr = cari_flac_dec_feed(d, m, len);
        CHECK_EQ(fr, (int)cnt);
        size_t dl;
        const uint8_t *o = cari_flac_dec_take(d, &dl);
        if (o) {
            CHECK(got_n + dl <= total * bps);
            memcpy(got + got_n, o, dl);
            got_n += dl;
        }
    }
    CHECK_EQ(got_n, total * bps);
    CHECK(!memcmp(src, got, total * bps));
    fprintf(stderr, "   %s %u Hz bs=%u: %zu msgs, %zu -> %zu bytes (%.0f%%)\n", fi->name, rate, bs,
            msgs, total * bps, comp, 100.0 * comp / (total * bps));
    cari_flac_dec_stats_t st;
    cari_flac_dec_stats(d, &st);
    CHECK_EQ(st.samples, total);
    CHECK_EQ(st.rejected, 0);
    cari_flac_enc_free(e);
    cari_flac_dec_free(d);
    free(src);
    free(got);
}

static void test_roundtrips(void)
{
    roundtrip(CARI_FMT_CS16, 500000, 5000, 5000, 50000);
    roundtrip(CARI_FMT_CS16, 125000, 1250, 1250, 12500 + 17);   /* short last block */
    roundtrip(CARI_FMT_CS16, 250000, 4096, 700, 30000);         /* several frames/msg */
    roundtrip(CARI_FMT_S16, 48000, 1024, 960, 9600);
    roundtrip(CARI_FMT_CS8, 65535, 512, 512, 5000);
    roundtrip(CARI_FMT_CU8, 24000, 256, 300, 3000);             /* unsigned offset */
    roundtrip(CARI_FMT_U8, 8000, 192, 192, 1920);
    roundtrip(CARI_FMT_S8, 655350, 2048, 2048, 8192);
}

static void test_invalid(void)
{
    CHECK(cari_flac_enc_new(CARI_FMT_CF32, 48000, 1024, 5) == NULL);   /* float */
    CHECK(cari_flac_enc_new(CARI_FMT_CS16, 1000000, 1024, 5) == NULL); /* rate */
    CHECK(cari_flac_enc_new(CARI_FMT_CS16, 48000, 8, 5) == NULL);      /* block */
    CHECK(cari_flac_dec_new(CARI_FMT_F32, 0) == NULL);
}

static void test_join_and_mismatch(void)
{
    /* Receiver joins an ongoing stream: first message starts mid-frame. */
    cari_flac_enc_t *e = cari_flac_enc_new(CARI_FMT_CS16, 250000, 1000, 5);
    uint8_t src[4000 * 4];
    gen(CARI_FMT_CS16, src, 4000, 0);
    cari_flac_enc_push(e, src, 4000);
    cari_flac_enc_finish(e);
    size_t len;
    const uint8_t *m = cari_flac_enc_take(e, &len);
    CHECK(m && len > 100);

    cari_flac_dec_t *d = cari_flac_dec_new(CARI_FMT_CS16, 250000);
    int fr = cari_flac_dec_feed(d, m + 37, len - 37);
    CHECK_EQ(fr, 3);
    cari_flac_dec_stats_t st;
    cari_flac_dec_stats(d, &st);
    CHECK(st.skipped_bytes > 0);
    size_t dl;
    const uint8_t *o = cari_flac_dec_take(d, &dl);
    CHECK_EQ(dl, 3000 * 4);
    CHECK(o && !memcmp(o, src + 1000 * 4, 3000 * 4));
    cari_flac_dec_free(d);

    /* wrong rate or format is rejected */
    d = cari_flac_dec_new(CARI_FMT_CS16, 125000);
    CHECK_EQ(cari_flac_dec_feed(d, m, len), -1);
    cari_flac_dec_take(d, &dl);
    CHECK_EQ(dl, 0);
    cari_flac_dec_free(d);
    d = cari_flac_dec_new(CARI_FMT_S16, 0);
    CHECK_EQ(cari_flac_dec_feed(d, m, len), -1);
    cari_flac_dec_free(d);

    /* a corrupted frame is dropped, the others decode */
    uint8_t *cp = malloc(len);
    memcpy(cp, m, len);
    cari_flac_span_t sp[8];
    size_t sk;
    size_t cnt = cari_flac_split(cp, len, sp, 8, &sk);
    CHECK_EQ(cnt, 4);
    cp[sp[1].off + sp[1].len / 2] ^= 0x5A;
    d = cari_flac_dec_new(CARI_FMT_CS16, 0);
    fr = cari_flac_dec_feed(d, cp, len);
    cari_flac_dec_stats(d, &st);
    CHECK_EQ(st.frames, 3);
    cari_flac_dec_take(d, &dl);
    CHECK_EQ(dl, 3000 * 4);
    /* and the decoder keeps working afterwards */
    CHECK_EQ(cari_flac_dec_feed(d, m, len), 4);
    cari_flac_dec_free(d);
    free(cp);
    cari_flac_enc_free(e);
}

int main(void)
{
    RUN(test_roundtrips);
    RUN(test_invalid);
    RUN(test_join_and_mismatch);
    T_DONE();
}
