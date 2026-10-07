/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 */
#include "sampan/ring.h"

#include <string.h>

bool sampan_ring_init(struct sampan_ring *r, uint8_t *buf, size_t size)
{
    if (size == 0 || (size & (size - 1)) != 0)
        return false;
    r->buf = buf;
    r->mask = size - 1;
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
    return true;
}

size_t sampan_ring_used(const struct sampan_ring *r)
{
    size_t t = atomic_load_explicit(&((struct sampan_ring *)r)->tail, memory_order_acquire);
    size_t h = atomic_load_explicit(&((struct sampan_ring *)r)->head, memory_order_acquire);
    return h - t;
}

size_t sampan_ring_free(const struct sampan_ring *r)
{
    return r->mask + 1 - sampan_ring_used(r);
}

/* Copy n bytes between the ring at index pos and a flat buffer, wrapping. */
static void copy_in(struct sampan_ring *r, size_t pos, const uint8_t *src, size_t n)
{
    size_t off = pos & r->mask;
    size_t first = r->mask + 1 - off;
    if (first > n)
        first = n;
    memcpy(r->buf + off, src, first);
    memcpy(r->buf, src + first, n - first);
}

static void copy_out(const struct sampan_ring *r, size_t pos, uint8_t *dst, size_t n)
{
    size_t off = pos & r->mask;
    size_t first = r->mask + 1 - off;
    if (first > n)
        first = n;
    memcpy(dst, r->buf + off, first);
    memcpy(dst + first, r->buf, n - first);
}

size_t sampan_ring_write(struct sampan_ring *r, const uint8_t *data, size_t n)
{
    size_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
    size_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
    size_t room = r->mask + 1 - (h - t);
    if (n > room)
        n = room;
    copy_in(r, h, data, n);
    atomic_store_explicit(&r->head, h + n, memory_order_release);
    return n;
}

size_t sampan_ring_read(struct sampan_ring *r, uint8_t *data, size_t n)
{
    size_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    size_t h = atomic_load_explicit(&r->head, memory_order_acquire);
    if (n > h - t)
        n = h - t;
    copy_out(r, t, data, n);
    atomic_store_explicit(&r->tail, t + n, memory_order_release);
    return n;
}
