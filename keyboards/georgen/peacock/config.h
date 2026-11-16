// Copyright 2024 George Norton (@george-norton)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#define DIGITIZER_MOTION_PIN GP28
#define DIGITIZER_MOTION_PIN_ACTIVE_LOW yes
#define MXT_SWITCH_XY
#define MXT_SENSOR_WIDTH_MM 91
#define MXT_SENSOR_HEIGHT_MM 156

// TODO: Tweak these if the sensor tracks poorly
#define MXT_GAIN 5
#define MXT_TOUCH_THRESHOLD 18
#define MXT_TOUCH_HYST 8
#define MXT_INTERNAL_TOUCH_HYST 4
#define MXT_INTERNAL_TOUCH_THRESHOLD 10
#define MXT_CHARGE_TIME 10
#define MXT_IDLE_SYNCS_PER_X 20
#define MXT_ACTIVE_SYNCS_PER_X 20
#define MXT_ACTIVE_ACQUISITION_INTERVAL 255 // Free run while active - this enables us to reach a polling rate of around 300Hz
#define MXT_T100_AMPL_COEFF 25 // Scale pressure reports
#define I2C1_CLOCK_SPEED 1000000
#define DIGITIZER_REPORT_FINGER_PRESSURE
