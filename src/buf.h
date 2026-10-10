/* libcari - internal growable byte buffer */
#ifndef CARI_BUF_H
#define CARI_BUF_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *p;
    size_t   len;
    size_t   cap;
} cari_buf_t;

static inline int cari_buf_reserve(cari_buf_t *b, size_t need)
{
    if (b->len + need <= b->cap)
        return 0;
    size_t nc = b->cap ? b->cap : 4096;
    while (nc < b->len + need)
        nc *= 2;
    uint8_t *np = realloc(b->p, nc);
    if (!np)
        return -1;
    b->p = np;
    b->cap = nc;
    return 0;
}

static inline int cari_buf_append(cari_buf_t *b, const void *d, size_t n)
{
    if (cari_buf_reserve(b, n) < 0)
        return -1;
    memcpy(b->p + b->len, d, n);
    b->len += n;
    return 0;
}

static inline void cari_buf_free(cari_buf_t *b)
{
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

#endif
