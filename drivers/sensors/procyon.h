// Copyright 2024 George Norton (@george-norton)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// These parameters are common to all Procyon boards
// OR in new definitions as boards become available
#if defined(PROCYON_50_42) || defined(PROCYON_42_50) || defined(PROCYON_57_80)
#    ifndef MXT_GAIN
#        define MXT_GAIN 10
#    endif
#    ifndef MXT_TOUCH_THRESHOLD
#        define MXT_TOUCH_THRESHOLD 20
#    endif
#    ifndef MXT_TOUCH_HYST
#        define MXT_TOUCH_HYST 10
#    endif
#    ifndef MXT_INTERNAL_TOUCH_THRESHOLD
#        define MXT_INTERNAL_TOUCH_THRESHOLD 10
#    endif
#    ifndef MXT_INTERNAL_TOUCH_HYST
#        define MXT_INTERNAL_TOUCH_HYST 5
#    endif
#    define MXT_T56_SHIELDLESS_ENABLE
#    define MXT_RETRANSMISSION_COMPENSATION_ENABLE 0
#    define MXT_X_PITCH 38 // Procyon boards have a standard 3.8mm pitch
#    define MXT_Y_PITCH 38
#    define MXT_ACTIVE_ACQUISITION_INTERVAL 255 // Free run while active - this enables us to reach a polling rate of around 300Hz
#    define MXT_CONFTHR 5 // Increase debounce time
#    define MXT_MOVE_HYSTERESIS_NEXT 16
#    ifndef MXT_T100_AMPL_COEFF
#        define MXT_T100_AMPL_COEFF 16 // Scale pressure reports
#    endif
#endif

// Board specific configuration

// The procyon 50x42 is an early prototype - it has been replaced by the 42x50 and will be deleted in the future
#ifdef PROCYON_50_42
#    define MXT_SENSOR_WIDTH_MM 50
#    define MXT_SENSOR_HEIGHT_MM 42
#    define MXT_MATRIX_X_SIZE 12
#    define MXT_MATRIX_Y_SIZE 10
#    define MXT_MESALLOW 1 // This board has matrix size which prohibits self capacitance measurements
#endif

#ifdef PROCYON_42_50
#    define MXT_SENSOR_WIDTH_MM 42
#    define MXT_SENSOR_HEIGHT_MM 50
#    define MXT_MATRIX_X_SIZE 10
#    define MXT_MATRIX_Y_SIZE 12
#    define MXT_MESALLOW 3
#endif

#ifdef PROCYON_57_80
#    define MXT_SENSOR_WIDTH_MM 57
#    define MXT_SENSOR_HEIGHT_MM 80
#    define MXT_MATRIX_X_SIZE 14
#    define MXT_MATRIX_Y_SIZE 20
#    define MXT_MESALLOW 3
#endif
