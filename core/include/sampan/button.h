/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * Button debounce and gestures. Feed the raw level with a millisecond clock
 * (wrap-safe); call often, also while nothing changes, so timeouts fire.
 *
 *   PRESS / RELEASE  on every debounced edge, with no delay
 *   CLICK            short press, sent once the double-click window has passed
 *                    (at once on release when double_ms is 0)
 *   DOUBLE           second short press inside the window, sent on its release
 *   LONG             held for long_ms, sent while still held
 *
 * If the second press of a double is held long, CLICK (for the first press)
 * and LONG are sent.
 */
#ifndef SAMPAN_BUTTON_H
#define SAMPAN_BUTTON_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum sampan_btn_event {
    SAMPAN_BTN_PRESS = 1,
    SAMPAN_BTN_RELEASE,
    SAMPAN_BTN_CLICK,
    SAMPAN_BTN_DOUBLE,
    SAMPAN_BTN_LONG,
};

struct sampan_btn_cfg {
    uint16_t debounce_ms; /* default 10 */
    uint16_t double_ms;   /* default 250; 0 disables DOUBLE */
    uint16_t long_ms;     /* default 600 */
};

struct sampan_btn {
    struct sampan_btn_cfg cfg;
    bool raw, stable;
    uint32_t raw_since;
    uint32_t press_at, release_at;
    bool long_fired;
    bool second;       /* this press is the second of a possible double */
    bool click_wait;   /* one click done, waiting for a second press */
};

#define SAMPAN_BTN_MAX_EVENTS 3

void sampan_btn_init(struct sampan_btn *b, const struct sampan_btn_cfg *cfg /* NULL = defaults */);

/* Returns the number of events written to ev (at most SAMPAN_BTN_MAX_EVENTS). */
int sampan_btn_update(struct sampan_btn *b, bool pressed, uint32_t now_ms, uint8_t *ev);

#ifdef __cplusplus
}
#endif

#endif
