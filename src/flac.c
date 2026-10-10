/*
 * libcari - libFLAC wrapper for CARI compressed baseband
 */
#include "cari/cari_flac.h"
#include "cari/cari_proto.h"
#include "buf.h"

#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>

/* ---------------------------------------------------------------- encoder */

struct cari_flac_enc {
    FLAC__StreamEncoder      *enc;
    const cari_format_info_t *fi;
    uint8_t                   fmt;
    cari_buf_t                out;
    size_t                    taken;    /* bytes handed out by the last take() */
    int32_t                  *ch[2];
    size_t                    chcap;
    bool                      finished;
};

static FLAC__StreamEncoderWriteStatus enc_write_cb(const FLAC__StreamEncoder *enc,
        const FLAC__byte buffer[], size_t bytes, uint32_t samples, uint32_t frame, void *ud)
{
    (void)enc; (void)frame;
    cari_flac_enc_t *e = ud;
    if (samples == 0)               /* "fLaC" marker and metadata: not transmitted */
        return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
    if (cari_buf_append(&e->out, buffer, bytes) < 0)
        return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

cari_flac_enc_t *cari_flac_enc_new(uint8_t fmt, uint32_t rate, uint32_t blocksize, int level)
{
    const cari_format_info_t *fi = cari_format_info(fmt);
    if (!fi || fi->is_float || !cari_flac_rate_ok(rate) || blocksize < 16 || blocksize > 65535)
        return NULL;
    cari_flac_enc_t *e = calloc(1, sizeof *e);
    if (!e)
        return NULL;
    e->fi = fi;
    e->fmt = fmt;
    e->enc = FLAC__stream_encoder_new();
    if (!e->enc)
        goto fail;
    FLAC__stream_encoder_set_verify(e->enc, false);
    FLAC__stream_encoder_set_channels(e->enc, fi->channels);
    FLAC__stream_encoder_set_bits_per_sample(e->enc, fi->bits);
    FLAC__stream_encoder_set_sample_rate(e->enc, rate);
    FLAC__stream_encoder_set_compression_level(e->enc, (uint32_t)(level < 0 ? 0 : level > 8 ? 8 : level));
    FLAC__stream_encoder_set_blocksize(e->enc, blocksize);
    /* Subset streams are decodable by any FLAC decoder. The subset allows
     * our rates (<= 655350 Hz) and block sizes up to 16384 above 48 kHz. */
    FLAC__stream_encoder_set_streamable_subset(e->enc, blocksize <= 16384 && (rate > 48000 || blocksize <= 4608));
    if (FLAC__stream_encoder_init_stream(e->enc, enc_write_cb, NULL, NULL, NULL, e)
        != FLAC__STREAM_ENCODER_INIT_STATUS_OK)
        goto fail;
    e->out.len = 0;                 /* nothing from init is kept anyway */
    return e;
fail:
    cari_flac_enc_free(e);
    return NULL;
}

void cari_flac_enc_free(cari_flac_enc_t *e)
{
    if (!e)
        return;
    if (e->enc) {
        if (!e->finished && FLAC__stream_encoder_get_state(e->enc) == FLAC__STREAM_ENCODER_OK)
            FLAC__stream_encoder_finish(e->enc);
        FLAC__stream_encoder_delete(e->enc);
    }
    cari_buf_free(&e->out);
    free(e->ch[0]);
    free(e->ch[1]);
    free(e);
}

static void drop_taken(cari_flac_enc_t *e)
{
    if (e->taken) {
        memmove(e->out.p, e->out.p + e->taken, e->out.len - e->taken);
        e->out.len -= e->taken;
        e->taken = 0;
    }
}

int cari_flac_enc_push(cari_flac_enc_t *e, const void *samples, size_t n)
{
    if (e->finished)
        return -1;
    drop_taken(e);
    if (n > e->chcap) {
        for (int c = 0; c < 2; c++) {
            int32_t *p = realloc(e->ch[c], n * sizeof(int32_t));
            if (!p)
                return -1;
            e->ch[c] = p;
        }
        e->chcap = n;
    }
    if (cari_samples_to_int32(e->fmt, samples, n, e->ch) < 0)
        return -1;
    const FLAC__int32 *const bufs[2] = { e->ch[0], e->ch[1] };
    return FLAC__stream_encoder_process(e->enc, bufs, (uint32_t)n) ? 0 : -1;
}

int cari_flac_enc_finish(cari_flac_enc_t *e)
{
    if (e->finished)
        return 0;
    drop_taken(e);
    e->finished = true;
    return FLAC__stream_encoder_finish(e->enc) ? 0 : -1;
}

const uint8_t *cari_flac_enc_take(cari_flac_enc_t *e, size_t *len)
{
    drop_taken(e);
    *len = e->out.len;
    if (!e->out.len)
        return NULL;
    e->taken = e->out.len;
    return e->out.p;
}

/* ---------------------------------------------------------------- decoder */

#define MAX_FRAMES_PER_MSG 512

struct cari_flac_dec {
    FLAC__StreamDecoder      *dec;
    const cari_format_info_t *fi;
    uint8_t                   fmt;
    uint32_t                  rate;
    const uint8_t            *in;
    size_t                    in_len, in_pos;
    cari_buf_t                out;
    size_t                    taken;
    size_t                    last_off;     /* start of the last write callback's samples */
    bool                      got_frame, frame_bad;
    cari_flac_dec_stats_t     st;
    cari_flac_span_t          spans[MAX_FRAMES_PER_MSG];
};

static FLAC__StreamDecoderReadStatus dec_read_cb(const FLAC__StreamDecoder *dec,
        FLAC__byte buffer[], size_t *bytes, void *ud)
{
    (void)dec;
    cari_flac_dec_t *d = ud;
    size_t left = d->in_len - d->in_pos;
    if (!left) {
        /* Returning CONTINUE with 0 bytes would make libFLAC spin. */
        *bytes = 0;
        return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }
    if (*bytes > left)
        *bytes = left;
    memcpy(buffer, d->in + d->in_pos, *bytes);
    d->in_pos += *bytes;
    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamDecoderWriteStatus dec_write_cb(const FLAC__StreamDecoder *dec,
        const FLAC__Frame *frame, const FLAC__int32 *const buffer[], void *ud)
{
    (void)dec;
    cari_flac_dec_t *d = ud;
    const FLAC__FrameHeader *h = &frame->header;
    d->got_frame = true;
    if (h->channels != d->fi->channels || h->bits_per_sample != d->fi->bits ||
        (d->rate && h->sample_rate != d->rate)) {
        d->frame_bad = true;
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }
    size_t nbytes = (size_t)h->blocksize * d->fi->bytes;
    d->last_off = d->out.len;
    if (cari_buf_reserve(&d->out, nbytes) < 0)
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    cari_samples_from_int32(d->fmt, (const int32_t *const *)buffer, h->blocksize, d->out.p + d->out.len);
    d->out.len += nbytes;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void dec_error_cb(const FLAC__StreamDecoder *dec, FLAC__StreamDecoderErrorStatus status, void *ud)
{
    (void)dec;
    cari_flac_dec_t *d = ud;
    /* libFLAC >= 1.4 reports a gap in frame numbers as MISSING_FRAME (6) and
     * writes a block of silence for it. CARI does not conceal lost frames
     * (the slave keeps time), so that is not an error for the frame at hand;
     * decode_one() keeps only the last block written. */
    if ((int)status == 6)
        return;
    d->frame_bad = true;
}

cari_flac_dec_t *cari_flac_dec_new(uint8_t fmt, uint32_t expected_rate)
{
    const cari_format_info_t *fi = cari_format_info(fmt);
    if (!fi || fi->is_float)
        return NULL;
    cari_flac_dec_t *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->fi = fi;
    d->fmt = fmt;
    d->rate = expected_rate;
    d->dec = FLAC__stream_decoder_new();
    if (!d->dec)
        goto fail;
    FLAC__stream_decoder_set_md5_checking(d->dec, false);
    if (FLAC__stream_decoder_init_stream(d->dec, dec_read_cb, NULL, NULL, NULL, NULL,
                                         dec_write_cb, NULL, dec_error_cb, d)
        != FLAC__STREAM_DECODER_INIT_STATUS_OK)
        goto fail;
    return d;
fail:
    cari_flac_dec_free(d);
    return NULL;
}

void cari_flac_dec_free(cari_flac_dec_t *d)
{
    if (!d)
        return;
    if (d->dec) {
        FLAC__stream_decoder_finish(d->dec);
        FLAC__stream_decoder_delete(d->dec);
    }
    cari_buf_free(&d->out);
    free(d);
}

/* Decode exactly one verified frame. */
static bool decode_one(cari_flac_dec_t *d, const uint8_t *p, size_t n)
{
    size_t before = d->out.len;
    d->in = p;
    d->in_len = n;
    d->in_pos = 0;
    d->got_frame = false;
    d->frame_bad = false;
    for (int guard = 0; guard < 8 && !d->got_frame; guard++) {
        if (!FLAC__stream_decoder_process_single(d->dec)) {
            FLAC__stream_decoder_reset(d->dec);
            break;
        }
        FLAC__StreamDecoderState s = FLAC__stream_decoder_get_state(d->dec);
        if (s == FLAC__STREAM_DECODER_END_OF_STREAM || s == FLAC__STREAM_DECODER_ABORTED)
            break;
    }
    FLAC__StreamDecoderState s = FLAC__stream_decoder_get_state(d->dec);
    if (d->got_frame && !d->frame_bad && d->last_off > before) {
        size_t nb = d->out.len - d->last_off;   /* drop concealment blocks */
        memmove(d->out.p + before, d->out.p + d->last_off, nb);
        d->out.len = before + nb;
    }
    if (s != FLAC__STREAM_DECODER_SEARCH_FOR_FRAME_SYNC && s != FLAC__STREAM_DECODER_SEARCH_FOR_METADATA)
        FLAC__stream_decoder_flush(d->dec);     /* back to frame sync search */
    d->in = NULL;
    return d->got_frame && !d->frame_bad;
}

int cari_flac_dec_feed(cari_flac_dec_t *d, const uint8_t *msg, size_t n)
{
    size_t skipped = 0, off = 0;
    int ok = 0;
    bool err = false;

    if (d->taken) {
        memmove(d->out.p, d->out.p + d->taken, d->out.len - d->taken);
        d->out.len -= d->taken;
        d->taken = 0;
    }
    while (off < n) {
        size_t sk = 0;
        size_t cnt = cari_flac_split(msg + off, n - off, d->spans, MAX_FRAMES_PER_MSG, &sk);
        skipped += sk;
        if (!cnt)
            break;
        for (size_t i = 0; i < cnt; i++) {
            const cari_flac_span_t *sp = &d->spans[i];
            if (!cari_flac_hdr_explicit(&sp->hdr) || sp->hdr.channels != d->fi->channels ||
                sp->hdr.bps != d->fi->bits || (d->rate && sp->hdr.sample_rate != d->rate)) {
                d->st.rejected++;
                err = true;
                continue;
            }
            size_t before = d->out.len;
            if (decode_one(d, msg + off + sp->off, sp->len)) {
                d->st.frames++;
                d->st.samples += (d->out.len - before) / d->fi->bytes;
                ok++;
            } else {
                d->out.len = before;
                d->st.rejected++;
                err = true;
            }
        }
        const cari_flac_span_t *last = &d->spans[cnt - 1];
        off += last->off + last->len;
    }
    d->st.skipped_bytes += skipped;
    return err ? -1 : ok;
}

const uint8_t *cari_flac_dec_take(cari_flac_dec_t *d, size_t *len)
{
    if (d->taken) {
        memmove(d->out.p, d->out.p + d->taken, d->out.len - d->taken);
        d->out.len -= d->taken;
        d->taken = 0;
    }
    *len = d->out.len;
    if (!d->out.len)
        return NULL;
    d->taken = d->out.len;
    return d->out.p;
}

void cari_flac_dec_stats(const cari_flac_dec_t *d, cari_flac_dec_stats_t *st)
{
    *st = d->st;
}
