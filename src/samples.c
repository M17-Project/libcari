/*
 * libcari - baseband sample scaling (Table 18, "Sample scaling")
 */
#include "cari/cari_proto.h"

#include <math.h>

size_t cari_format_bytes(uint8_t fmt)
{
    const cari_format_info_t *fi = cari_format_info(fmt);
    return fi ? fi->bytes : 0;
}

/* One component, raw integer (sign-extended / offset-removed) */
static int32_t get_comp_int(const cari_format_info_t *fi, const uint8_t *p)
{
    if (fi->bits == 16)
        return (int16_t)cari_get_le16(p);
    if (fi->is_signed)
        return (int8_t)p[0];
    return (int32_t)p[0] - 128;     /* unsigned: mid-scale is zero */
}

static void put_comp_int(const cari_format_info_t *fi, uint8_t *p, int32_t v)
{
    int32_t max = (1 << (fi->bits - 1)) - 1, min = -(1 << (fi->bits - 1));
    if (v > max) v = max;
    if (v < min) v = min;
    if (fi->bits == 16)
        cari_put_le16(p, (uint16_t)(int16_t)v);
    else if (fi->is_signed)
        p[0] = (uint8_t)(int8_t)v;
    else
        p[0] = (uint8_t)(v + 128);
}

int cari_samples_to_float(uint8_t fmt, const void *src, size_t n, float *dst)
{
    const cari_format_info_t *fi = cari_format_info(fmt);
    if (!fi)
        return -1;
    const uint8_t *p = src;
    size_t ncomp = n * fi->channels, csz = fi->bits / 8;
    if (fi->is_float) {
        for (size_t i = 0; i < ncomp; i++)
            dst[i] = cari_get_f32(p + 4 * i);
        return 0;
    }
    float scale = 1.0f / (float)(1 << (fi->bits - 1));
    for (size_t i = 0; i < ncomp; i++)
        dst[i] = (float)get_comp_int(fi, p + csz * i) * scale;
    return 0;
}

int cari_samples_from_float(uint8_t fmt, const float *src, size_t n, void *dst)
{
    const cari_format_info_t *fi = cari_format_info(fmt);
    if (!fi)
        return -1;
    uint8_t *p = dst;
    size_t ncomp = n * fi->channels, csz = fi->bits / 8;
    if (fi->is_float) {
        for (size_t i = 0; i < ncomp; i++)
            cari_put_f32(p + 4 * i, src[i]);
        return 0;
    }
    float full = (float)(1 << (fi->bits - 1));
    for (size_t i = 0; i < ncomp; i++) {
        float x = src[i];
        if (!(x == x))
            x = 0.0f;               /* NaN -> 0 */
        long v = lrintf(x * full);  /* put_comp_int clips to [-2^(n-1), 2^(n-1)-1] */
        if (v > 1L << 30) v = 1L << 30;
        if (v < -(1L << 30)) v = -(1L << 30);
        put_comp_int(fi, p + csz * i, (int32_t)v);
    }
    return 0;
}

int cari_samples_to_int32(uint8_t fmt, const void *src, size_t n, int32_t *const ch[])
{
    const cari_format_info_t *fi = cari_format_info(fmt);
    if (!fi || fi->is_float)
        return -1;
    const uint8_t *p = src;
    size_t csz = fi->bits / 8;
    for (size_t i = 0; i < n; i++)
        for (unsigned c = 0; c < fi->channels; c++)
            ch[c][i] = get_comp_int(fi, p + csz * (i * fi->channels + c));
    return 0;
}

int cari_samples_from_int32(uint8_t fmt, const int32_t *const ch[], size_t n, void *dst)
{
    const cari_format_info_t *fi = cari_format_info(fmt);
    if (!fi || fi->is_float)
        return -1;
    uint8_t *p = dst;
    size_t csz = fi->bits / 8;
    for (size_t i = 0; i < n; i++)
        for (unsigned c = 0; c < fi->channels; c++)
            put_comp_int(fi, p + csz * (i * fi->channels + c), ch[c][i]);
    return 0;
}
