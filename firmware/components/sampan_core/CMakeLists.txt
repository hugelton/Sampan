# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
#
# The portable core (../../../core), built as an ESP-IDF component.
set(core ${CMAKE_CURRENT_LIST_DIR}/../../../core)
idf_component_register(
    SRCS ${core}/src/proto.c ${core}/src/adpcm.c ${core}/src/ring.c ${core}/src/tx.c
         ${core}/src/button.c ${core}/src/midi.c ${core}/src/scope.c ${core}/src/palette.c
         ${core}/src/dsp.c ${core}/src/sample.c
    INCLUDE_DIRS ${core}/include)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
