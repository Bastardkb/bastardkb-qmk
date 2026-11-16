// Copyright 2024 George Norton (@george-norton)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#define I2C_DRIVER I2CD0
#define I2C1_SDA_PIN GP8
#define I2C1_SCL_PIN GP9
#define I2C1_CLOCK_SPEED 1000000

#define DIGITIZER_MOTION_PIN GP11
#define DIGITIZER_MOTION_PIN_ACTIVE_LOW yes
#define DIGITIZER_REPORT_FINGER_PRESSURE

// If enabled, the stylus will automatically be detected if the touch is small and round.
// #define DIGITIZER_HAS_STYLUS yes

#define MXT_SENSOR_WIDTH_MM 156
#define MXT_SENSOR_HEIGHT_MM 99

// TODO: Tweak these if the sensor tracks poorly
#define MXT_TOUCH_THRESHOLD 24
#define MXT_GAIN 11
#define MXT_TOUCH_HYST 6
#define MXT_INTERNAL_TOUCH_HYST 3
#define MXT_INTERNAL_TOUCH_THRESHOLD 8

#define MXT_IDLE_SYNCS_PER_X 40
#define MXT_ACTIVE_SYNCS_PER_X 40
#define MXT_IDLE_ACQUISITION_INTERVAL 255
#define MXT_ACTIVE_ACQUISITION_INTERVAL 255
#define MXT_T56_SHIELDLESS_ENABLE
#define MXT_T65_LENS_BENDING_ENABLE
#define MXT_LOW_PASS_FILTER_COEFFICIENT 10

#define MXT_INVERT_X
#define MXT_INVERT_Y
#define MXT_RESET_PIN GP12
#define MXT_I2CMODE_PIN GP6

#define UNUSABLE_PINS \
    { GP3, GP7, GP10, GP13, GP14, GP16, GP17, GP18, GP19, GP20, GP21, GP22, GP23, GP25, GP26, GP27, GP28, GP29 }

#define HAPTICS_PIN GP15
#define DEBUG_LED_PIN GP24

#define PAVONIS