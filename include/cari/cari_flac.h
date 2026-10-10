/*
 * libcari - FLAC baseband compression (CARI 2.1 "Compressed baseband")
 *
 * Thin libFLAC wrapper that produces and consumes CARI-style FLAC data:
 * bare frames (no "fLaC" marker, no metadata), sample rate and bit depth
 * written explicitly in every frame header, unsigned formats offset by
 * -2^(n-1). Only available when libcari is built with libFLAC
 * (CARI_HAVE_FLAC is defined in cari_config.h).
 */
#ifndef CARI_FLAC_H
#define CARI_FLAC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cari_flac_enc cari_flac_enc_t;
typedef struct cari_flac_dec cari_flac_dec_t;

/* fmt: an integer CARI signal format. rate must satisfy cari_flac_rate_ok().
 * blocksize: samples per FLAC frame (16..65535), level: 0..8. */
cari_flac_enc_t *cari_flac_enc_new(uint8_t fmt, uint32_t rate, uint32_t blocksize, int level);
void             cari_flac_enc_free(cari_flac_enc_t *e);
/* Push samples (in fmt). Complete frames accumulate in an output buffer.
 * Note: libFLAC emits a block once it holds blocksize+1 samples. */
int              cari_flac_enc_push(cari_flac_enc_t *e, const void *samples, size_t n);
/* Encode the remaining partial block as a final (shorter) frame. The encoder
 * can not be used afterwards except for take() and free(). */
int              cari_flac_enc_finish(cari_flac_enc_t *e);
/* Accumulated complete frames; resets the buffer. NULL if empty. The pointer
 * stays valid until the next push/finish/free. */
const uint8_t   *cari_flac_enc_take(cari_flac_enc_t *e, size_t *len);

/* expected_rate: 0 to accept any rate, otherwise frames with a different
 * header rate are rejected. */
cari_flac_dec_t *cari_flac_dec_new(uint8_t fmt, uint32_t expected_rate);
void             cari_flac_dec_free(cari_flac_dec_t *d);
/* Decode all complete frames in one message (data before the first sync
 * code is discarded). Returns the number of frames decoded, or -1 if a frame
 * failed to decode or did not match fmt/rate. */
int              cari_flac_dec_feed(cari_flac_dec_t *d, const uint8_t *msg, size_t n);
/* Decoded samples in fmt (nsamples = len / bytes per sample). Resets. */
const uint8_t   *cari_flac_dec_take(cari_flac_dec_t *d, size_t *len);

typedef struct {
    uint64_t frames;        /* frames decoded */
    uint64_t samples;       /* samples produced */
    uint64_t rejected;      /* frames rejected (bad params / decode error) */
    uint64_t skipped_bytes; /* bytes before sync / not verifying */
} cari_flac_dec_stats_t;
void cari_flac_dec_stats(const cari_flac_dec_t *d, cari_flac_dec_stats_t *st);

#ifdef __cplusplus
}
#endif
#endif /* CARI_FLAC_H */
