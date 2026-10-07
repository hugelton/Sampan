/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * Single-producer single-consumer byte ring. One side may be an ISR or
 * another core; the size must be a power of two.
 */
#ifndef SAMPAN_RING_H
#define SAMPAN_RING_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sampan_ring {
    uint8_t *buf;
    size_t mask;
    atomic_size_t head; /* written by the producer */
    atomic_size_t tail; /* written by the consumer */
};

bool sampan_ring_init(struct sampan_ring *r, uint8_t *buf, size_t size);
size_t sampan_ring_used(const struct sampan_ring *r);
size_t sampan_ring_free(const struct sampan_ring *r);
/* Copy as much as fits / is available; return the byte count. */
size_t sampan_ring_write(struct sampan_ring *r, const uint8_t *data, size_t n);
size_t sampan_ring_read(struct sampan_ring *r, uint8_t *data, size_t n);

#ifdef __cplusplus
}
#endif

#endif
