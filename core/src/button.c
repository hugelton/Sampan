/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 */
#include "sampan/button.h"

#include <string.h>

void sampan_btn_init(struct sampan_btn *b, const struct sampan_btn_cfg *cfg)
{
    memset(b, 0, sizeof(*b));
    if (cfg) {
        b->cfg = *cfg;
    } else {
        b->cfg.debounce_ms = 10;
        b->cfg.double_ms = 250;
        b->cfg.long_ms = 600;
    }
}

int sampan_btn_update(struct sampan_btn *b, bool pressed, uint32_t now, uint8_t *ev)
{
    int n = 0;

    if (pressed != b->raw) {
        b->raw = pressed;
        b->raw_since = now;
    }
    bool edge = b->raw != b->stable && (uint32_t)(now - b->raw_since) >= b->cfg.debounce_ms;

    if (edge && b->raw) {
        b->stable = true;
        ev[n++] = SAMPAN_BTN_PRESS;
        b->press_at = now;
        b->long_fired = false;
        b->second = b->click_wait;
        b->click_wait = false;
    } else if (edge) {
        b->stable = false;
        ev[n++] = SAMPAN_BTN_RELEASE;
        if (!b->long_fired) {
            if (b->second) {
                ev[n++] = SAMPAN_BTN_DOUBLE;
            } else if (b->cfg.double_ms == 0) {
                ev[n++] = SAMPAN_BTN_CLICK;
            } else {
                b->click_wait = true;
                b->release_at = now;
            }
        }
        b->second = false;
    }

    if (b->stable && !b->long_fired && (uint32_t)(now - b->press_at) >= b->cfg.long_ms) {
        b->long_fired = true;
        if (b->second)
            ev[n++] = SAMPAN_BTN_CLICK;
        b->second = false;
        ev[n++] = SAMPAN_BTN_LONG;
    }

    /* A press still being debounced counts as inside the window. */
    if (b->click_wait && !b->raw && (uint32_t)(now - b->release_at) >= b->cfg.double_ms) {
        b->click_wait = false;
        ev[n++] = SAMPAN_BTN_CLICK;
    }
    return n;
}
