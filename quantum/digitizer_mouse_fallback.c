// Copyright 2025 George Norton (@george-norton)
// SPDX-License-Identifier: GPL-2.0-or-later

#if defined(POINTING_DEVICE_DRIVER_digitizer)

// We can fallback to reporting as a mouse for hosts which do not implement trackpad support.

#    include <stdlib.h>
#    include "digitizer.h"
#    include "digitizer_mouse_fallback.h"
#    include "debug.h"
#    include "timer.h"
#    include "action.h"

#    ifndef DIGITIZER_MOUSE_TAP_DETECTION_TIMEOUT
#        define DIGITIZER_MOUSE_TAP_DETECTION_TIMEOUT 200
#    endif

#    ifndef DIGITIZER_MOUSE_TAP_DURATION
#        define DIGITIZER_MOUSE_TAP_DURATION 1
#    endif

#    ifndef DIGITIZER_MOUSE_TAP_DISTANCE
#        define DIGITIZER_MOUSE_TAP_DISTANCE 25
#    endif

#    ifndef DIGITIZER_SCROLL_DIVISOR
#        define DIGITIZER_SCROLL_DIVISOR 10
#    endif

#    ifndef DIGITIZER_MOUSE_SWIPE_TIMEOUT
#        define DIGITIZER_MOUSE_SWIPE_TIMEOUT 1000
#    endif

#    ifndef DIGITIZER_MOUSE_SWIPE_DISTANCE
#        define DIGITIZER_MOUSE_SWIPE_DISTANCE 500
#    endif

#    ifndef DIGITIZER_MOUSE_SWIPE_THRESHOLD
#        define DIGITIZER_MOUSE_SWIPE_THRESHOLD 300
#    endif

#    ifndef DIGITIZER_SWIPE_LEFT_KC
#        define DIGITIZER_SWIPE_LEFT_KC QK_MOUSE_BUTTON_3
#    endif

#    ifndef DIGITIZER_SWIPE_RIGHT_KC
#        define DIGITIZER_SWIPE_RIGHT_KC QK_MOUSE_BUTTON_4
#    endif

#    ifndef DIGITIZER_SWIPE_UP_KC
#        define DIGITIZER_SWIPE_UP_KC KC_LEFT_GUI
#    endif

#    ifndef DIGITIZER_SWIPE_DOWN_KC
#        define DIGITIZER_SWIPE_DOWN_KC KC_ESC
#    endif

#    ifndef DIGITIZER_MIN_CPI
#        define DIGITIZER_MIN_CPI 50
#    endif

#    ifndef DIGITIZER_MAX_CPI
#        define DIGITIZER_MAX_CPI 1200
#    endif

#    ifdef DIGITIZER_REPORT_TAPS_AS_CLICKS
bool digitizer_taps_as_clicks = true;
#    else
bool digitizer_taps_as_clicks = false;
#    endif

#define CLIP(X, A, B) (X<A ? A : X>B ? B : X)

// This variable indicates that we are sending mouse reports. It will be updated
// during USB enumeration if the host sends a feature report indicating it supports
// Microsofts Precision Trackpad protocol. This variable can also be modified by users
// to force reporting as a mouse or as a digitizer.
bool                  digitizer_send_mouse_reports = true;
static report_mouse_t mouse_report                 = {};

static report_mouse_t digitizer_get_mouse_report(report_mouse_t _mouse_report);
static uint16_t       digitizer_get_cpi(void);
static void           digitizer_set_cpi(uint16_t cpi);
static bool           digitizer_mouse_fallback_init(void);

const pointing_device_driver_t digitizer_pointing_device_driver = {.init = digitizer_mouse_fallback_init, .get_report = digitizer_get_mouse_report, .get_cpi = digitizer_get_cpi, .set_cpi = digitizer_set_cpi};

/**
 * @brief Initialize the pointing device driver. If this function does not return true, the pointing device
 * is not usable.
 *
 * @return report_mouse_t
 */
static bool digitizer_mouse_fallback_init(void)
{
    // TODO: Return false here, if we have a physical digitizer device and its initialization failed.
    return true;
}

/**
 * @brief Gets the current digitizer mouse report, the pointing device feature will send this is we
 * nave fallen back to mouse mode.
 *
 * @return report_mouse_t
 */
/* Spike detection to filter stale data after DPI/CPI changes
 *
 * MaxTouch digitizer coordinates are scale-dependent. When CPI changes, the same
 * physical position reports different absolute coordinates, causing large position
 * jumps in the first few frames after the change. We scan for these spikes and
 * discard the first report that exceeds the threshold.
 */
#define SPIKE_THRESHOLD 100           /* Magnitude above this is considered a stale delta */
#define SPIKE_SCAN_FRAMES 150         /* Max frames to scan for spike after CPI change */
static uint16_t g_frames_since_cpi_change;  /* Frame counter since last CPI change */
static bool     g_spike_found;               /* Whether we've found and discarded the spike */
static uint8_t  g_last_cpi;                  /* Track CPI changes to detect transitions */

static report_mouse_t digitizer_get_mouse_report(report_mouse_t _mouse_report) {
    if (digitizer_send_mouse_reports) {
        report_mouse_t report = mouse_report;
        /* Retain the button state, but drop any motion. */
        memset(&mouse_report, 0, sizeof(report_mouse_t));

        /* Spike detection: discard stale deltas after CPI changes */
        uint16_t current_cpi = digitizer_get_cpi();
        if (current_cpi != g_last_cpi) {
            /* CPI changed - reset spike detection state */
            g_frames_since_cpi_change = 0;
            g_spike_found = false;
            g_last_cpi = current_cpi;
        }

        if (!g_spike_found && g_frames_since_cpi_change < SPIKE_SCAN_FRAMES) {
            g_frames_since_cpi_change++;
            int16_t magnitude = (report.x < 0 ? -report.x : report.x) +
                              (report.y < 0 ? -report.y : report.y);
            if (magnitude > SPIKE_THRESHOLD) {
                /* Found the spike - discard it */
                g_spike_found = true;
                report.x = 0;
                report.y = 0;
            }
        }

        mouse_report.buttons = report.buttons;
        return report;
    }
    return _mouse_report;
}

static uint16_t mouse_cpi = 400;

/**
 * @brief Gets the CPI used by the digitizer mouse fallback feature.
 *
 * @return the current CPI value
 */
static uint16_t digitizer_get_cpi(void) {
    return mouse_cpi;
}

/**
 * @brief Sets the CPI used by the digitizer mouse fallback feature.
 *
 *  @param[in] the new CPI value
 */
static void digitizer_set_cpi(uint16_t cpi) {
    mouse_cpi = CLIP(cpi, DIGITIZER_MIN_CPI, DIGITIZER_MAX_CPI);
    digitizer_set_scale((mouse_cpi * 100) / DIGITIZER_MAX_CPI);
}

// The gesture detection state machine will transition between these states.
typedef enum { None, Down, MoveScroll, Tapped, DoubleTapped, Drag, Swipe, Finished } State;

static State state     = None;
static int   tap_count = 0;

/**
 * \brief Signals that a gesture is in progress so digitizer_update_mouse_report should be called,
 * even if no new digitizer data is available.
 * @return true if update_mouse_report should run.
 */
bool digitizer_update_gesture_state(void) {
    return tap_count || state != None;
}

/**
 * \brief Generate a mouse report from the digitizer report. This function implements
 * a state machine to detect gestures and handle them.
 * @param[in] report a new digitizer report
 */
void digitizer_update_mouse_report(report_digitizer_t *report) {
    static int      contact_start_time = 0;
    static int      contact_start_x    = 0;
    static int      contact_start_y    = 0;
    static int      tap_contacts       = 0;
    static int      last_contacts      = 0;
    static uint16_t last_x             = 0;
    static uint16_t last_y             = 0;
    const uint16_t  x                  = report->fingers[0].x;
    const uint16_t  y                  = report->fingers[0].y;
    const uint32_t  duration           = timer_elapsed32(contact_start_time);
    int             contacts           = 0;

    memset(&mouse_report, 0, sizeof(report_mouse_t));

    for (int i = 0; i < DIGITIZER_FINGER_COUNT; i++) {
        if (report->fingers[i].tip) {
            contacts++;
        }
    }
    switch (state) {
        case None: {
            if (contacts != 0) {
                state              = Down;
                contact_start_time = timer_read32();
                contact_start_x    = x;
                contact_start_y    = y;
                tap_contacts       = contacts;
            }
            break;
        }
        case Down: {
            const uint16_t distance_x = abs(contact_start_x - x);
            const uint16_t distance_y = abs(contact_start_y - y);
            tap_contacts              = MAX(contacts, tap_contacts);

            if (contacts == 0) {
                state              = Tapped;
                contact_start_time = timer_read32();
            } else if (contacts >= 3) {
                state = Swipe;
            } else if (duration > DIGITIZER_MOUSE_TAP_DETECTION_TIMEOUT || distance_x > DIGITIZER_MOUSE_TAP_DISTANCE || distance_y > DIGITIZER_MOUSE_TAP_DISTANCE) {
                state = MoveScroll;
            }
            break;
        }
        case Drag:
        case MoveScroll: {
            if (contacts == 0) {
                state = None;
            } else if (contacts == 1) {
                if (x && y && last_x && last_y) {
                    mouse_report.x = x - last_x;
                    mouse_report.y = y - last_y;
                }
                last_x = x;
                last_y = y;
            } else if (contacts == 3 && duration < DIGITIZER_MOUSE_SWIPE_TIMEOUT) {
                state = Swipe;
            } else {
                static int carry_h = 0;
                static int carry_v = 0;
                const int  h       = x - last_x + carry_h;
                const int  v       = y - last_y + carry_v;

                carry_h = h % DIGITIZER_SCROLL_DIVISOR;
                carry_v = v % DIGITIZER_SCROLL_DIVISOR;

                mouse_report.h = h / DIGITIZER_SCROLL_DIVISOR;
                mouse_report.v = v / DIGITIZER_SCROLL_DIVISOR;
            }
            break;
        }
        case DoubleTapped:
        case Tapped: {
            tap_contacts = MAX(contacts, tap_contacts);
            if (contacts == 0 && last_contacts != contacts) {
                tap_count++;
                state              = DoubleTapped;
                contact_start_time = timer_read32();
            } else if (duration > DIGITIZER_MOUSE_TAP_DETECTION_TIMEOUT) {
                if (contacts > 0 && state == Tapped) {
                    state = Drag;
                } else {
                    tap_count++;
                    state = Finished;
                }
            }
            break;
        }
        case Swipe: {
            const int32_t distance_x = x - contact_start_x;
            const int32_t distance_y = y - contact_start_y;
            if (contacts == 0) {
                state = None;
            } else if (duration > DIGITIZER_MOUSE_SWIPE_TIMEOUT) {
                state = MoveScroll;
            } else if (digitizer_send_mouse_reports) {
                if (distance_x > DIGITIZER_MOUSE_SWIPE_DISTANCE && abs(distance_y) < DIGITIZER_MOUSE_SWIPE_THRESHOLD) {
                    // Swipe right
                    tap_code(DIGITIZER_SWIPE_RIGHT_KC);
                    state = Finished;
                } else if (distance_x < -DIGITIZER_MOUSE_SWIPE_DISTANCE && abs(distance_y) < DIGITIZER_MOUSE_SWIPE_THRESHOLD) {
                    // Swipe left
                    tap_code(DIGITIZER_SWIPE_LEFT_KC);
                    state = Finished;
                } else if (distance_y > DIGITIZER_MOUSE_SWIPE_DISTANCE && abs(distance_x) < DIGITIZER_MOUSE_SWIPE_THRESHOLD) {
                    // Swipe down
                    tap_code(DIGITIZER_SWIPE_DOWN_KC);
                    state = Finished;
                } else if (distance_y < -DIGITIZER_MOUSE_SWIPE_DISTANCE && abs(distance_x) < DIGITIZER_MOUSE_SWIPE_THRESHOLD) {
                    // Swipe up
                    tap_code(DIGITIZER_SWIPE_UP_KC);
                    state = Finished;
                }
            }
            break;
        }
        case Finished: {
            if (contacts == 0) {
                state = None;
            }
            break;
        }
    }
    static bool     tap      = false;
    static uint32_t tap_time = 0;
    if (tap_count) {
        if (timer_elapsed32(tap_time) > DIGITIZER_MOUSE_TAP_DURATION) {
            tap = !tap;
            if (!tap) {
                tap_count--;
            }
            tap_time = timer_read32();
        }
    }
    const bool button_pressed = tap || (state == Drag);
    if (report->button1 || (tap_contacts == 1 && button_pressed)) {
        mouse_report.buttons |= 0x1;
        if (digitizer_taps_as_clicks) report->button1 = 1;
    }
    if (report->button2 || (tap_contacts == 2 && button_pressed)) {
        mouse_report.buttons |= 0x2;
        if (digitizer_taps_as_clicks) report->button2 = 1;
    }
    if (report->button3 || (tap_contacts == 3 && button_pressed)) {
        mouse_report.buttons |= 0x4;
        if (digitizer_taps_as_clicks) report->button3 = 1;
    }
    last_contacts = contacts;
}
#endif
