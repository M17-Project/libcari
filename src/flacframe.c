/*
 * libcari - FLAC framing helpers ("Compressed baseband")
 *
 * These helpers work on the raw byte stream and do not need libFLAC: they
 * find and verify frame boundaries (frame header CRC-8, whole-frame CRC-16)
 * so that a receiver can resynchronise, check the CARI header requirements
 * and hand exactly one frame at a time to a decoder.
 *
 * Reference: https://xiph.org/flac/format.html, RFC 9639.
 */
#include "cari/cari_proto.h"

#include <math.h>

bool cari_flac_rate_ok(double rate)
{
    if (!(rate > 0) || rate != floor(rate))
        return false;
    if (rate <= 65535.0)
        return true;
    return rate <= 655350.0 && fmod(rate, 10.0) == 0.0;
}

uint8_t cari_flac_crc8(const uint8_t *p, size_t n)
{
    uint8_t crc = 0;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++)
            crc = (uint8_t)((crc & 0x80) ? (crc << 1) ^ 0x07 : (crc << 1));
    }
    return crc;
}

uint16_t cari_flac_crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0;
    while (n--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++)
            crc = (uint16_t)((crc & 0x8000) ? (crc << 1) ^ 0x8005 : (crc << 1));
    }
    return crc;
}

static const uint32_t rate_codes[12] = {
    0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000
};
static const uint8_t bps_codes[8] = { 0, 8, 12, 0xFF, 16, 20, 24, 32 };

int cari_flac_parse_header(const uint8_t *p, size_t n, cari_flac_hdr_t *h)
{
    if (n < 2)
        return -2;
    if (p[0] != 0xFF || (p[1] & 0xFE) != 0xF8)     /* 14-bit sync + reserved 0 */
        return -1;
    if (n < 5)
        return -2;

    h->variable_blocksize = p[1] & 1;
    uint8_t bs_code = p[2] >> 4, sr_code = p[2] & 0x0F;
    uint8_t ch_code = p[3] >> 4, ss_code = (p[3] >> 1) & 7;
    if (bs_code == 0 || sr_code == 15 || ch_code > 10 || ss_code == 3 || (p[3] & 1))
        return -1;

    /* UTF-8-like coded frame/sample number */
    size_t off = 4;
    uint8_t b0 = p[off++];
    int extra;
    uint64_t num;
    if (!(b0 & 0x80))              { extra = 0; num = b0; }
    else if ((b0 & 0xE0) == 0xC0)  { extra = 1; num = b0 & 0x1F; }
    else if ((b0 & 0xF0) == 0xE0)  { extra = 2; num = b0 & 0x0F; }
    else if ((b0 & 0xF8) == 0xF0)  { extra = 3; num = b0 & 0x07; }
    else if ((b0 & 0xFC) == 0xF8)  { extra = 4; num = b0 & 0x03; }
    else if ((b0 & 0xFE) == 0xFC)  { extra = 5; num = b0 & 0x01; }
    else if (b0 == 0xFE && h->variable_blocksize) { extra = 6; num = 0; }
    else return -1;
    if (off + (size_t)extra > n)
        return -2;
    for (int i = 0; i < extra; i++) {
        uint8_t c = p[off++];
        if ((c & 0xC0) != 0x80)
            return -1;
        num = (num << 6) | (c & 0x3F);
    }
    h->number = num;

    /* block size */
    if (bs_code == 1)
        h->block_size = 192;
    else if (bs_code <= 5)
        h->block_size = 576u << (bs_code - 2);
    else if (bs_code == 6) {
        if (off + 1 > n) return -2;
        h->block_size = (uint32_t)p[off] + 1;
        off += 1;
    } else if (bs_code == 7) {
        if (off + 2 > n) return -2;
        h->block_size = (((uint32_t)p[off] << 8) | p[off + 1]) + 1;
        off += 2;
    } else
        h->block_size = 256u << (bs_code - 8);

    /* sample rate */
    if (sr_code < 12)
        h->sample_rate = rate_codes[sr_code];
    else if (sr_code == 12) {
        if (off + 1 > n) return -2;
        h->sample_rate = (uint32_t)p[off] * 1000u;
        off += 1;
    } else {
        if (off + 2 > n) return -2;
        uint32_t v = ((uint32_t)p[off] << 8) | p[off + 1];
        h->sample_rate = sr_code == 13 ? v : v * 10u;
        off += 2;
    }

    h->chan_assign = ch_code;
    h->channels = ch_code < 8 ? (uint8_t)(ch_code + 1) : 2;
    h->bps = bps_codes[ss_code];

    if (off + 1 > n)
        return -2;
    if (cari_flac_crc8(p, off) != p[off])
        return -1;
    h->header_len = off + 1;
    return 0;
}

bool cari_flac_hdr_explicit(const cari_flac_hdr_t *h)
{
    return h->sample_rate != 0 && h->bps != 0;
}

/* Next position >= from where a valid frame header starts, or n. */
static size_t next_header(const uint8_t *m, size_t n, size_t from, cari_flac_hdr_t *h)
{
    for (size_t i = from; i + 1 < n; i++) {
        if (m[i] != 0xFF || (m[i + 1] & 0xFE) != 0xF8)
            continue;
        if (cari_flac_parse_header(m + i, n - i, h) == 0)
            return i;
    }
    return n;
}

size_t cari_flac_split(const uint8_t *m, size_t n, cari_flac_span_t *out, size_t max,
                       size_t *skipped)
{
    size_t off = 0, cnt = 0, skip = 0;
    cari_flac_hdr_t h, h2;

    while (off < n && cnt < max) {
        size_t pos = next_header(m, n, off, &h);
        if (pos >= n) {
            skip += n - off;
            off = n;
            break;
        }
        skip += pos - off;

        /* Frame end: the next verified header or the end of the message,
         * whichever first yields a matching CRC-16. A sync code inside the
         * frame data that also passes the header CRC-8 is rejected by the
         * CRC-16 check. */
        size_t end = 0;
        size_t q = pos + h.header_len;
        for (;;) {
            size_t cand = next_header(m, n, q, &h2);
            if (cand - pos >= h.header_len + 2) {
                uint16_t want = (uint16_t)((m[cand - 2] << 8) | m[cand - 1]);
                if (cari_flac_crc16(m + pos, cand - pos - 2) == want) {
                    end = cand;
                    break;
                }
            }
            if (cand >= n)
                break;
            q = cand + 1;
        }
        if (!end) {
            skip += 1;              /* false sync or damaged frame */
            off = pos + 1;
            continue;
        }
        out[cnt].off = pos;
        out[cnt].len = end - pos;
        out[cnt].hdr = h;
        cnt++;
        off = end;
    }
    /* If out[] filled up, the remaining bytes are left unconsumed. */
    if (skipped)
        *skipped = skip;
    return cnt;
}
