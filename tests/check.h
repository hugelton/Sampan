/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 */
#ifndef SAMPAN_TESTS_CHECK_H
#define SAMPAN_TESTS_CHECK_H

#include <stdio.h>

static int failures, checks;

#define CHECK(cond)                                                            \
    do {                                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,   \
                    #cond);                                                    \
        }                                                                      \
    } while (0)

#define CHECK_DONE()                                                           \
    (printf("%s: %d checks, %d failures\n", __FILE__, checks, failures),       \
     failures ? 1 : 0)

#endif
