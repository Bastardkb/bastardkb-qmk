// Copyright 2024 George Norton (@george-norton)
// SPDX-License-Identifier: GPL-2.0-or-later

#include QMK_KEYBOARD_H
#include "i2c_master.h"
#include "maxtouch.h"

#include "digitizer.h"
#include "digitizer_driver.h"

#ifdef MAXTOUCH_DEBUG
#    include "raw_hid.h"
#endif

#define SWAP_BYTES(a) (((a << 8) & 0xff00) | ((a >> 8) & 0xff))

// Mandatory configuration. These are hardware specific.
#ifndef MXT_SENSOR_WIDTH_MM
#    error "You must define the MXT_SENSOR_WIDTH_MM"
#endif

#ifndef MXT_SENSOR_HEIGHT_MM
#    error "You must define the MXT_SENSOR_HEIGHT_MM"
#endif

// By default we assume all available X and Y pins are in use, but a designer
// may decide to leave some pins unconnected, so the size can be overridden here.
#ifndef MXT_MATRIX_X_SIZE
#    define MXT_MATRIX_X_SIZE information.matrix_x_size
#endif

#ifndef MXT_MATRIX_Y_SIZE
#    define MXT_MATRIX_Y_SIZE information.matrix_y_size
#endif

#ifndef MXT_SCROLL_DIVISOR
#    define MXT_SCROLL_DIVISOR 4
#endif

// We detect a tap gesture if an UP event occurs within MXT_TAP_TIME
// milliseconds of the DOWN event.
#ifndef MXT_TAP_TIME
#    define MXT_TAP_TIME 100
#endif

// We detect a tap and hold gesture if a finger does not move
// further than MXT_TAP_AND_HOLD_DISTANCE within MXT_TAP_AND_HOLD_TIME
// milliseconds of being put down on the sensor.
#ifndef MXT_TAP_AND_HOLD_TIME
#    define MXT_TAP_AND_HOLD_TIME 200
#endif
#ifndef MXT_TAP_AND_HOLD_DISTANCE
#    define MXT_TAP_AND_HOLD_DISTANCE 5
#endif

#ifndef MXT_RECALIBRATE_AFTER
// Steps of 200ms, 25 = 5 seconds
#    define MXT_RECALIBRATE_AFTER 50
#endif

#ifndef MXT_TOUCH_THRESHOLD
#    define MXT_TOUCH_THRESHOLD 18
#endif

#ifndef MXT_GAIN
#    define MXT_GAIN 4
#endif

#ifndef MXT_TOUCH_HYST
#    define MXT_TOUCH_HYST 0
#endif

#ifndef MXT_INTERNAL_TOUCH_HYST
#    define MXT_INTERNAL_TOUCH_HYST 0
#endif

#ifndef MXT_INTERNAL_TOUCH_THRESHOLD
#    define MXT_INTERNAL_TOUCH_THRESHOLD 0
#endif

#ifndef MXT_DX_GAIN
#    define MXT_DX_GAIN 0
#endif

#ifndef MXT_X_PITCH
#    define MXT_X_PITCH (MXT_SENSOR_WIDTH_MM * 10 / MXT_MATRIX_X_SIZE)
#endif

#ifndef MXT_Y_PITCH
#    define MXT_Y_PITCH (MXT_SENSOR_HEIGHT_MM * 10 / MXT_MATRIX_Y_SIZE)
#endif

#ifndef MXT_MESALLOW
#    define MXT_MESALLOW 3
#endif

#ifndef MXT_IDLE_SYNCS_PER_X
#    define MXT_IDLE_SYNCS_PER_X 0
#endif

#ifndef MXT_ACTIVE_SYNCS_PER_X
#    define MXT_ACTIVE_SYNCS_PER_X 0
#endif

#ifndef MXT_IDLE_ACQUISITION_INTERVAL
#    define MXT_IDLE_ACQUISITION_INTERVAL 32
#endif

#ifndef MXT_ACTIVE_ACQUISITION_INTERVAL
#    define MXT_ACTIVE_ACQUISITION_INTERVAL 10
#endif

#ifndef MXT_RETRANSMISSION_COMPENSATION_ENABLE
#    define MXT_RETRANSMISSION_COMPENSATION_ENABLE 1
#endif

#ifndef MXT_MOVE_HYSTERESIS_INITIAL
#    define MXT_MOVE_HYSTERESIS_INITIAL 10
#endif

#ifndef MXT_MOVE_HYSTERESIS_NEXT
#    define MXT_MOVE_HYSTERESIS_NEXT 4
#endif

#ifndef MXT_LOW_PASS_FILTER_COEFFICIENT
#    define MXT_LOW_PASS_FILTER_COEFFICIENT 0
#endif

#ifndef MXT_CHARGE_TIME
#    define MXT_CHARGE_TIME 1
#endif

// Any stylus event smaller than this, is treated as a hover rather than a press.
#ifndef MXT_STYLUS_HOVER_THRESHOLD
#    define MXT_STYLUS_HOVER_THRESHOLD 6
#endif

#ifndef MXT_CONFTHR
#    define MXT_CONFTHR 2
#endif

#ifndef MXT_ENABLE_STYLUS
#    ifdef DIGITIZER_HAS_STYLUS
#       define MXT_ENABLE_STYLUS 1
#    else
#       define MXT_ENABLE_STYLUS 0
#    endif
#endif

#ifndef MXT_T47_CONTMAX
#    define MXT_T47_CONTMAX 60
#endif

#ifndef MXT_T47_MAXTCHAREA
#    define MXT_T47_MAXTCHAREA 70
#endif

#ifndef MXT_T47_CONFTHR
#    define MXT_T47_CONFTHR 6
#endif

// The amplitude is optionally reported as pressure information,
// but the value may need to be scaled on larger devices.
#ifndef MXT_T100_AMPL_COEFF
#    define MXT_T100_AMPL_COEFF 0
#endif

#ifndef MAX_T100_AMPL_OFFSET
#    define MAX_T100_AMPL_OFFSET 0
#endif

// Data from the object table. Registers are not at fixed addresses, they may vary between firmware
// versions. Instead must read the addresses from the object table.
static uint16_t t2_encryption_status_address                 = 0;
static uint16_t t5_message_processor_address                 = 0;
static uint16_t t5_max_message_size                          = 0;
static uint16_t t6_command_processor_address                 = 0;
static uint16_t t6_command_processor_report_id               = 0;
static uint16_t t7_powerconfig_address                       = 0;
static uint16_t t8_acquisitionconfig_address                 = 0;
static uint16_t t25_self_test_address                        = 0;
static uint16_t t37_diagnostic_debug_address                 = 0;
static uint16_t t42_proci_touchsupression_address            = 0;
static uint16_t t44_message_count_address                    = 0;
static uint16_t t46_cte_config_address                       = 0;
static uint16_t t47_proci_stylus_address                     = 0;
static uint16_t t56_proci_shieldless_address                 = 0;
static uint16_t t65_proci_lensbending_address                = 0;
static uint16_t t80_proci_retransmissioncompensation_address = 0;
static uint16_t t100_multiple_touch_touchscreen_address      = 0;

// The object table also contains report_ids. These are used to identify which object generated a
// message. Again we must lookup these values rather than using hard coded values.
// Most messages are ignored, we basically just want the messages from the t100 object for now.
static uint16_t              t25_self_test_report_id                             = 0;
static uint16_t              t100_first_report_id                                = 0;
static uint16_t              t100_second_report_id                               = 0;
static uint16_t              t100_subsequent_report_ids[DIGITIZER_CONTACT_COUNT] = {};
static uint16_t              t100_num_reports                                    = 0;
static mxt_information_block information                                         = {0};

void maxtouch_print_info(void) {
    // Pavonis: Found MXT 164:75, fw 16.170 with 32 objects. Matrix size 41x26
    uprintf("Found MXT %d:%d, fw %d.%d with %d objects. Matrix size %dx%d\n", information.family_id, information.variant_id, information.version, information.build, information.num_objects, information.matrix_x_size, information.matrix_y_size);
}

void maxtouch_init(void) {
#ifdef MXT_I2CMODE_PIN
    gpio_set_pin_output(MXT_I2CMODE_PIN);
    gpio_write_pin_high(MXT_I2CMODE_PIN);
#endif
#ifdef MXT_RESET_PIN
    gpio_set_pin_output(MXT_RESET_PIN);
    gpio_write_pin_low(MXT_RESET_PIN);
    wait_ms(500);
    gpio_write_pin_high(MXT_RESET_PIN);
    wait_ms(300);
#endif
    i2c_init();
    i2c_status_t status = i2c_read_register16(MXT336UD_ADDRESS, MXT_REG_INFORMATION_BLOCK, (uint8_t *)&information, sizeof(mxt_information_block), MXT_I2C_TIMEOUT_MS);

    // First read the object table to lookup addresses and report_ids of the various objects
    if (status == I2C_STATUS_SUCCESS) {
        // I2C found device family: 166 with 34 objects
        dprintf("Found MXT %d:%d, fw %d.%d with %d objects. Matrix size %dx%d\n", information.family_id, information.variant_id, information.version, information.build, information.num_objects, information.matrix_x_size, information.matrix_y_size);
        int      report_id                    = 1;
        uint16_t object_table_element_address = sizeof(mxt_information_block);
        for (int i = 0; i < information.num_objects; i++) {
            mxt_object_table_element object = {};
            i2c_status_t             status = i2c_read_register16(MXT336UD_ADDRESS, SWAP_BYTES(object_table_element_address), (uint8_t *)&object, sizeof(mxt_object_table_element), MXT_I2C_TIMEOUT_MS);
            if (status == I2C_STATUS_SUCCESS) {
                // Store addresses in network byte order
                const uint16_t address = object.position_ms_byte | (object.position_ls_byte << 8);
                switch (object.type) {
                    case 2:
                        t2_encryption_status_address = address;
                        break;
                    case 5:
                        t5_message_processor_address = address;
                        t5_max_message_size          = object.size_minus_one - 1;
                        break;
                    case 6:
                        t6_command_processor_address   = address;
                        t6_command_processor_report_id = report_id;
                        break;
                    case 7:
                        t7_powerconfig_address = address;
                        break;
                    case 8:
                        t8_acquisitionconfig_address = address;
                        break;
                    case 25:
                        t25_self_test_address   = address;
                        t25_self_test_report_id = report_id;
                        break;
                    case 37:
                        t37_diagnostic_debug_address = address;
                        break;
                    case 42:
                        t42_proci_touchsupression_address = address;
                        break;
                    case 44:
                        t44_message_count_address = address;
                        break;
                    case 46:
                        t46_cte_config_address = address;
                        break;
                    case 47:
                        t47_proci_stylus_address = address;
                        break;
                    case 56:
                        t56_proci_shieldless_address = address;
                        break;
                    case 65:
                        t65_proci_lensbending_address = address;
                        break;
                    case 80:
                        t80_proci_retransmissioncompensation_address = address;
                        break;
                    case 100:
                        t100_multiple_touch_touchscreen_address = address;
                        t100_first_report_id                    = report_id;
                        t100_second_report_id                   = report_id + 1;
                        for (t100_num_reports = 0; t100_num_reports < DIGITIZER_CONTACT_COUNT && t100_num_reports < object.report_ids_per_instance; t100_num_reports++) {
                            t100_subsequent_report_ids[t100_num_reports] = report_id + 2 + t100_num_reports;
                        }
                        break;
                }
                object_table_element_address += sizeof(mxt_object_table_element);
                report_id += object.report_ids_per_instance * (object.instances_minus_one + 1);
            } else {
                dprintf("Failed to read object table element. Status: %d\n", status);
            }
        }
    } else {
        dprintf("Failed to read object table. Status: %d\n", status);
    }

    // TODO Remove? Maybe not interesting unless for whatever reason encryption is enabled and we need to turn it off
    if (t2_encryption_status_address) {
        mxt_gen_encryptionstatus_t2 t2     = {};
        i2c_status_t                status = i2c_read_register16(MXT336UD_ADDRESS, t2_encryption_status_address, (uint8_t *)&t2, sizeof(mxt_gen_encryptionstatus_t2), MXT_I2C_TIMEOUT_MS);
        if (status != I2C_STATUS_SUCCESS) {
            dprintf("Failed to read T2. Status: %02x %d\n", t2.status, t2.error);
        }
    }

    // Configure power saving features
    if (t7_powerconfig_address) {
        mxt_gen_powerconfig_t7 t7 = {};
        t7.idleacqint             = MXT_IDLE_ACQUISITION_INTERVAL;         // The acquisition interval while in idle mode. 255 is free-running (as fast as possible).
        t7.actacqint              = MXT_ACTIVE_ACQUISITION_INTERVAL;       // The acquisition interval while in active mode. 255 is free-running (as fast as possible).
        t7.actv2idelto            = 50;                                    // The timeout for transitioning from active to idle mode
        t7.cfg                    = T7_CFG_ACTVPIPEEN | T7_CFG_IDLEPIPEEN; // Enable pipelining in both active and idle mode

        i2c_write_register16(MXT336UD_ADDRESS, t7_powerconfig_address, (uint8_t *)&t7, sizeof(mxt_gen_powerconfig_t7), MXT_I2C_TIMEOUT_MS);
    }

    // Configure capacitive acquision, currently we use all the default values but it feels like some of this stuff might be important.
    if (t8_acquisitionconfig_address) {
        mxt_gen_acquisitionconfig_t8 t8 = {};
        t8.chrgtime                     = MXT_CHARGE_TIME;
        t8.tchautocal                   = MXT_RECALIBRATE_AFTER;
        t8.atchcalst                    = 0;

        // Antitouch detection - reject palms etc..
        t8.atchcalsthr     = 50;
        t8.atchfrccalthr   = 50;
        t8.atchfrccalratio = 25;
        t8.measallow       = MXT_MESALLOW;

        i2c_write_register16(MXT336UD_ADDRESS, t8_acquisitionconfig_address, (uint8_t *)&t8, sizeof(mxt_gen_acquisitionconfig_t8), MXT_I2C_TIMEOUT_MS);
    }

#ifdef DIGITIZER_HAS_STYLUS
    if (t42_proci_touchsupression_address) {
        mxt_proci_touchsupression_t42 t42 = {};

        t42.ctrl            = T42_CTRL_ENABLE | T42_CTRL_SHAPEEN;
        t42.maxapprarea     = 0; // Default (0): suppress any touch that approaches >40 channels.
        t42.maxtcharea      = 0; // Default (0): suppress any touch that covers >35 channels.
        t42.maxnumtchs      = MAX(5, DIGITIZER_CONTACT_COUNT); // Suppress all touches more contacts than this are detected.
        t42.supdist         = 0; // Default (0): Suppress all touches within 5 nodes of a suppressed large object detection.
        t42.disthyst        = 0;
        t42.supstrength     = 0; // Default (0): suppression strength of 128.
        t42.supextto        = 0; // Timeout to save power; set to 0 to disable.
        t42.shapestrength   = 0; // Default (0): shape suppression strength of 10, range [0, 31].
        t42.maxscrnarea     = 0;
        t42.edgesupstrength = 0;
        t42.cfg             = 1;
        i2c_write_register16(MXT336UD_ADDRESS, t42_proci_touchsupression_address, (uint8_t *)&t42, sizeof(mxt_proci_touchsupression_t42), MXT_I2C_TIMEOUT_MS);
    }
#endif

    // Mutural Capacitive Touch Engine (CTE) configuration, currently we use all the default values but it feels like some of this stuff might be important.
    if (t46_cte_config_address) {
        mxt_spt_cteconfig_t46 t46 = {};
        t46.idlesyncsperx         = MXT_IDLE_SYNCS_PER_X; // ADC samples per X.
        t46.activesyncsperx       = MXT_ACTIVE_SYNCS_PER_X; // ADC samples per X.
        t46.inrushcfg             = 0;  // Set Y-line inrush limit resistors.

        i2c_write_register16(MXT336UD_ADDRESS, t46_cte_config_address, (uint8_t *)&t46, sizeof(mxt_spt_cteconfig_t46), MXT_I2C_TIMEOUT_MS);
    }

#ifdef DIGITIZER_HAS_STYLUS
    if (t47_proci_stylus_address) {
        mxt_proci_stylus_t47 t47 = {};
        t47.ctrl                 = MXT_ENABLE_STYLUS;   // Enable stylus detection
        t47.cfg                  = T47_CFG_SUPSTY;      // Supress stylus detections when normal touches are present.
        t47.contmax              = MXT_T47_CONTMAX;     // The maximum contact diameter of the stylus in 0.1mm increments
        t47.maxtcharea           = MXT_T47_MAXTCHAREA;  // Maximum touch area a contact can have an still be considered a stylus
        t47.stability            = 0;                   // Higher values prevent the stylus from dropping out when it gets small
        t47.confthr              = MXT_T47_CONFTHR;     // Higher values increase the chances of correctly detecting as stylus, but introduce a delay
        t47.amplthr              = 0;                   // Any touches smaller than this are classified as stylus touches
        t47.supstyto             = 0;                   // Continue to suppress stylus touches until supstyto x 200ms after the last touch is removed.
        t47.hoversup             = 0;                   // 255 Disables hover supression
        t47.maxnumsty            = 1;                   // Only report a single stylus
        i2c_write_register16(MXT336UD_ADDRESS, t47_proci_stylus_address, (uint8_t *)&t47, sizeof(mxt_proci_stylus_t47), MXT_I2C_TIMEOUT_MS);
    }
#endif

    if (t80_proci_retransmissioncompensation_address) {
        mxt_proci_retransmissioncompensation_t80 t80 = {};
        t80.ctrl                                     = MXT_RETRANSMISSION_COMPENSATION_ENABLE;
        t80.compgain                                 = 5;
        t80.targetdelta                              = 125;
        t80.compthr                                  = 60;
        i2c_write_register16(MXT336UD_ADDRESS, t80_proci_retransmissioncompensation_address, (uint8_t *)&t80, sizeof(mxt_proci_retransmissioncompensation_t80), MXT_I2C_TIMEOUT_MS);
    }

    // Multiple touch touchscreen confguration - defines an area of the sensor to use as a trackpad/touchscreen. This object generates all our interesting report messages.
    if (t100_multiple_touch_touchscreen_address) {
        mxt_touch_multiscreen_t100 cfg = {};

        cfg.ctrl         = T100_CTRL_RPTEN | T100_CTRL_ENABLE | T100_CTRL_SCANEN; // Enable the t100 object, and enable message reporting for the t100 object.1. Also enable close scanning mode.
        // TODO: Generic handling of rotation/inversion for absolute mode?
        uint8_t rotation = 0;
#ifdef MXT_INVERT_X
        rotation         |= T100_CFG_INVERTX;
#endif
#ifdef MXT_INVERT_Y
        rotation         |= T100_CFG_INVERTY;
#endif
#ifdef MXT_SWITCH_XY
        rotation         |= T100_CFG_SWITCHXY;
#endif
        cfg.cfg1         = rotation;
        cfg.scraux       = 0x7;                                             // AUX data: Report the number of touch events, touch area, anti touch area
        cfg.tchaux       = 0x2;                                             // report amplitude
#ifdef DIGITIZER_REPORT_FINGER_SIZE
        cfg.tchaux       |= 0x8;
#endif
        cfg.tcheventcfg  = 24;                                              // Disable reporting suppressed events
        cfg.numtch       = DIGITIZER_CONTACT_COUNT;                         // The number of touch reports we want to receive (upto 10)
        cfg.xsize        = MXT_MATRIX_X_SIZE;                               // Make configurable as this depends on the sensor design.
        cfg.ysize        = MXT_MATRIX_Y_SIZE;                               // Make configurable as this depends on the sensor design.
        cfg.xpitch       = MXT_X_PITCH;                                     // Pitch between X-Lines in 0.1mm increments.
        cfg.ypitch       = MXT_Y_PITCH;                                     // Pitch between Y-Lines in 0.1mm increments.
        cfg.xedgecfg     = 9;
        cfg.xedgedist    = 10;
        cfg.yedgecfg     = 9;
        cfg.yedgedist    = 10;
        cfg.gain         = MXT_GAIN;            // Single transmit gain for mutual capacitance measurements
        cfg.dxgain       = MXT_DX_GAIN;         // Dual transmit gain for mutual capacitance measurements (255 = auto calibrate)
        cfg.tchthr       = MXT_TOUCH_THRESHOLD; // Touch threshold
        cfg.tchhyst      = MXT_TOUCH_HYST;
        cfg.intthr       = MXT_INTERNAL_TOUCH_THRESHOLD;
        cfg.intthryst    = MXT_INTERNAL_TOUCH_HYST;
        cfg.mrgthr       = 5;  // Merge threshold
        cfg.mrghyst      = 10; // Merge threshold hysteresis
        cfg.mrgthradjstr = 20;
        cfg.movsmooth    = 0; // The amount of smoothing applied to movements, this tails off at higher speeds
        cfg.movfilter    = 0; // The lower 4 bits are the speed response value, higher values reduce lag, but also smoothing
        // These two fields implement a simple filter for reducing jitter, but large values cause the pointer to stick in place before moving.
        cfg.movhysti     = MXT_MOVE_HYSTERESIS_INITIAL; // Initial movement hysteresis
        cfg.movhystn     = MXT_MOVE_HYSTERESIS_NEXT;  // Next movement hysteresis

        cfg.tchdiup      = 4; // MXT_UP touch detection integration - the number of cycles before the sensor decides an MXT_UP event has occurred
        cfg.tchdidown    = 2; // MXT_DOWN touch detection integration - the number of cycles before the sensor decides an MXT_DOWN event has occurred
        cfg.nexttchdi    = 2;
        cfg.calcfg       = 0;
#ifdef MXT_SWITCH_XY
        cfg.xrange       = DIGITIZER_RESOLUTION_Y - 1; // The logical and physical resolution is reported in our USB descriptor
        cfg.yrange       = DIGITIZER_RESOLUTION_X - 1; // the host uses this to set the speed of the pointer.
#else
        cfg.xrange       = DIGITIZER_RESOLUTION_X - 1; // The logical and physical resolution is reported in our USB descriptor
        cfg.yrange       = DIGITIZER_RESOLUTION_Y - 1; // the host uses this to set the speed of the pointer.
#endif
        cfg.cfg2         = MXT_CONFTHR; // Touch debounce

        // Scale amplitude reporting
        cfg.amplcoeff    = MXT_T100_AMPL_COEFF;
        cfg.amploffset   = MAX_T100_AMPL_OFFSET;

        i2c_status_t status = i2c_write_register16(MXT336UD_ADDRESS, t100_multiple_touch_touchscreen_address, (uint8_t *)&cfg, sizeof(mxt_touch_multiscreen_t100), MXT_I2C_TIMEOUT_MS);
        if (status != I2C_STATUS_SUCCESS) {
            dprintf("T100 Configuration failed: %d\n", status);
        }
    }

    // Configure shieldless and lensbending objects to provide some additional resistance
    // against bad behaviour.
#ifdef MXT_T56_SHIELDLESS_ENABLE
    if (t56_proci_shieldless_address) {
        mxt_proci_shieldless_t56 t56 = {};
        t56.ctrl                     = T56_CTRL_ENABLE;
        t56.optint                   = 1;
        t56.inttime                  = 10;
        i2c_write_register16(MXT336UD_ADDRESS, t56_proci_shieldless_address, (uint8_t *)&t56, sizeof(mxt_proci_shieldless_t56), MXT_I2C_TIMEOUT_MS);
    }
#endif
#ifdef MXT_T65_LENS_BENDING_ENABLE
    if (t65_proci_lensbending_address) {
        mxt_proci_lensbending_t65 t65 = {};
        t65.ctrl                      = T65_CTRL_ENABLE;
        t65.lpfiltcoef                = MXT_LOW_PASS_FILTER_COEFFICIENT; // default (0): 5, range 1 to 15.
        i2c_write_register16(MXT336UD_ADDRESS, t65_proci_lensbending_address, (uint8_t *)&t65, sizeof(mxt_proci_lensbending_t65), MXT_I2C_TIMEOUT_MS);
    }
#endif
}

// Store state different from report so we can report MXT_DOWNUP as MXT_DOWN, but remember we are MXT_UP
digitizer_t maxtouch_get_report(digitizer_t digitizer_report) {
    if (t44_message_count_address) {
        mxt_message_count message_count = {};

        i2c_status_t status = i2c_read_register16(MXT336UD_ADDRESS, t44_message_count_address, (uint8_t *)&message_count, sizeof(mxt_message_count), MXT_I2C_TIMEOUT_MS);
        if (status == I2C_STATUS_SUCCESS) {
            for (int i = 0; i < message_count.count; i++) {
                mxt_message message = {};
                status              = i2c_read_register16(MXT336UD_ADDRESS, t5_message_processor_address, (uint8_t *)&message, sizeof(mxt_message), MXT_I2C_TIMEOUT_MS);

                if (message.report_id == t100_first_report_id) {
                    const uint8_t  fingers  = message.data[1];
#ifdef MAXTOUCH_BOOTLOADER_GESTURE
                    // Debug feature - reboot to bootloader if 5 fingers are MXT_DOWN
                    // TODO: A better gesture.
                    if (fingers == DIGITIZER_CONTACT_COUNT) reset_keyboard();
#endif
                    if (fingers == 0) {
                        // Belt and braces, make sure we dont have any stuck contacts
                        for (int j = 0; j < DIGITIZER_CONTACT_COUNT; j++) {
                            digitizer_report.contacts[j].type = UNKNOWN;
                            digitizer_report.contacts[j].tip = false;
                            digitizer_report.contacts[j].in_range = false;
                            digitizer_report.contacts[j].confidence = false;
                        }
                    }
                } else if (message.report_id == t25_self_test_report_id) {
                    const uint8_t result = message.data[0];
                    switch (result) {
                        case T25_TEST_PASSED:
                            uprintf("Self Tests passed\n");
                            break;
                        case T25_TEST_INVALID:
                            uprintf("Invalid self test command\n");
                            break;
                        case T25_TEST_POWER:
                            uprintf("Power fault detected\n");
                            break;
                        case T25_TEST_PIN_FAULT:
                            uprintf("Pin fault detected. Seq %d, pin %dx%d\n", message.data[2], message.data[3], message.data[4]);
                            break;
                        case T25_TEST_SIGNAL_LIMIT:
                            uprintf("Signal limit fault detected\n");
                            break;
                    }
                } else if ((message.report_id >= t100_subsequent_report_ids[0]) && (message.report_id <= t100_subsequent_report_ids[t100_num_reports - 1])) {
                    const uint8_t  contact_id = message.report_id - t100_subsequent_report_ids[0];
                    const int      event      = (message.data[0] & 0xf);
                    const int      type       = (message.data[0] >> 4) & 0x7;
                    const uint16_t x          = message.data[1] | (message.data[2] << 8);
                    const uint16_t y          = message.data[3] | (message.data[4] << 8);
                    const uint8_t  ampl       = message.data[5];
#ifdef DIGITIZER_REPORT_FINGER_SIZE
                    digitizer_report.contacts[contact_id].width = message.data[6];
                    digitizer_report.contacts[contact_id].height = message.data[7];
#endif
                    // uprintf("EVT[%u] %d %d %ux%u %u\n", contact_id, event, type, x, y, ampl);

                    switch (type) {
                        case MXT_FINGER:
                            digitizer_report.contacts[contact_id].type = FINGER;
                            break;
                        case MXT_PASSIVE_STYLUS:
                            digitizer_report.contacts[contact_id].type = STYLUS;
                            break;
                        default:
                            digitizer_report.contacts[contact_id].type = UNKNOWN;
                            break;
                    }

                    digitizer_report.contacts[contact_id].in_range = true;
#ifdef DIGITIZER_REPORT_FINGER_PRESSURE
                    digitizer_report.contacts[contact_id].pressure = ampl;
#endif
                    if (type == MXT_FINGER) {
                        if (event == MXT_DOWN || event == MXT_MOVE) {
                            digitizer_report.contacts[contact_id].tip = true;
                        }
                    }
                    else if (type == MXT_PASSIVE_STYLUS) {
                        digitizer_report.contacts[contact_id].tip = ampl > MXT_STYLUS_HOVER_THRESHOLD;
                    }

                    if (event == MXT_UP || event == MXT_UNSUPSUP) {
                        digitizer_report.contacts[contact_id].tip = false;
                    }

                    if (event == MXT_MOVE || event == MXT_DOWN || event == MXT_DOWNSUP || event == MXT_UP || event == MXT_UNSUPUP || event == MXT_UNSUP) {
                        digitizer_report.contacts[contact_id].x = x;
                        digitizer_report.contacts[contact_id].y = y;
                    }
                    if (event == MXT_SUP || event == MXT_UNSUPSUP || event == MXT_DOWNSUP) {
                        digitizer_report.contacts[contact_id].confidence = 0;
                    } else {
                        digitizer_report.contacts[contact_id].confidence = 1;
                    }
                } else if (message.report_id == t6_command_processor_report_id) {
                    const uint8_t status = message.data[0];
                    uprintf("T6 status: RESET: %d, OFL: %d. SIGERR: %d, CAL: %d, CFGERR: %d. COMSERR: %d\n", status & (1 << 7) ? 1 : 0, status & (1 << 6) ? 1 : 0, status & (1 << 5) ? 1 : 0, status & (1 << 4) ? 1 : 0, status & (1 << 3) ? 1 : 0, status & (1 << 2) ? 1 : 0);
                    // Run all self tests after a reset
                    if (t25_self_test_address && status & (1 << 7)) {
                        mxt_spt_selftest_t25 t25 = {};
                        t25.ctrl                 = 0x3;
                        t25.cmd                  = T25_TEST_ALL;

                        // Min/Max values from the 1066u datasheet
                        t25.losiglim_msb = 0x44; // 17500
                        t25.losiglim_lsb = 0x5c;
                        t25.upsiglim_msb = 0x79; // 31000
                        t25.upsiglim_lsb = 0x18;

                        // Observed reference signal is approx 6000, and we get
                        // a 700 delta when touching. So create a range of 7000.
                        t25.sigrangelim_lsb = 0x58;
                        t25.sigrangelim_msb = 0x1B;

                        t25.sesiglimits[1] = MXT_GAIN;
                        t25.sesiglimits[2] = MXT_DX_GAIN;

                        i2c_write_register16(MXT336UD_ADDRESS, t25_self_test_address, (uint8_t *)&t25, sizeof(mxt_spt_selftest_t25), MXT_I2C_TIMEOUT_MS);
                    }
                } else {
                    uprintf("Unhandled event %d (%02x %02x %02x %02x %02x %02x) %d\n", message.report_id, message.data[0], message.data[1], message.data[2], message.data[3], message.data[4], message.data[5], t25_self_test_report_id);
                }
            }
        }
    }
    return digitizer_report;
}

#ifdef MAXTOUCH_DEBUG
#    define MAXTOUCH_DEBUG_MAGIC 0x9A4D
#    define MAXTOUCH_DEBUG_VERSION 0x0001

typedef enum {
    MAXTOUCH_DEBUG_CHECK_VERSION,
    MAXTOUCH_DEBUG_COMMAND,
    MAXTOUCH_DEBUG_READ,
    MAXTOUCH_DEBUG_WRITE,
} maxtouch_debug_command;

typedef enum { MAXTOUCH_DEBUG_REBOOT_BOOTLOADER, MAXTOUCH_DEBUG_SET_MOUSE_MODE, MAXTOUCH_DEBUG_GET_MOUSE_MODE } maxtouch_debug_command_type;

typedef enum { MAXTOUCH_DEBUG_OK, MAXTOUCH_DEBUG_INVALID_VERSION, MAXTOUCH_DEBUG_INVALID_CMD, MAXTOUCH_DEBUG_INVALID_LENGTH, MAXTOUCH_DEBUG_I2C_ERR } maxtouch_debug_status;

void raw_hid_receive(uint8_t *data, uint8_t length) {
    maxtouch_debug_status  status = MAXTOUCH_DEBUG_OK;
    maxtouch_debug_command cmd    = (maxtouch_debug_command)data[0];

    switch (cmd) {
        case MAXTOUCH_DEBUG_CHECK_VERSION: {
            const uint16_t magic   = (data[1] << 8) | data[2];
            const uint16_t version = (data[3] << 8) | data[4];
            if (magic != MAXTOUCH_DEBUG_MAGIC || version != MAXTOUCH_DEBUG_VERSION) {
                status = MAXTOUCH_DEBUG_INVALID_VERSION;
            }
            break;
        }
        case MAXTOUCH_DEBUG_COMMAND: {
            const maxtouch_debug_command_type cmd_type = data[1];
            switch (cmd_type) {
                case MAXTOUCH_DEBUG_REBOOT_BOOTLOADER:
                    reset_keyboard();
                    break;
#if defined(POINTING_DEVICE_DRIVER_digitizer)
                case MAXTOUCH_DEBUG_SET_MOUSE_MODE: {
                    extern bool digitizer_send_mouse_reports;
                    digitizer_send_mouse_reports = (bool)data[2];
                    break;
                }
                case MAXTOUCH_DEBUG_GET_MOUSE_MODE: {
                    extern bool digitizer_send_mouse_reports;
                    data[1] = digitizer_send_mouse_reports;
                    break;
                }
#endif
                default:
                    status = MAXTOUCH_DEBUG_INVALID_CMD;
                    break;
            }
            break;
        }
        case MAXTOUCH_DEBUG_READ: {
            const uint16_t read_address = (data[1] << 8) | data[2];
            const uint16_t read_length  = data[3];
            if (read_length > 0x1c) {
                status = MAXTOUCH_DEBUG_INVALID_LENGTH;
            } else {
                if (i2c_read_register16(MXT336UD_ADDRESS, read_address, (uint8_t *)&data[4], read_length, MXT_I2C_TIMEOUT_MS) != I2C_STATUS_SUCCESS) {
                    status = MAXTOUCH_DEBUG_I2C_ERR;
                }
            }
            break;
        }
        case MAXTOUCH_DEBUG_WRITE: {
            const uint16_t write_address = (data[1] << 8) | data[2];
            const uint16_t write_length  = data[3];
            if (write_length > 0x1c) {
                status = MAXTOUCH_DEBUG_INVALID_LENGTH;
            } else {
                if (i2c_write_register16(MXT336UD_ADDRESS, write_address, (uint8_t *)&data[4], write_length, MXT_I2C_TIMEOUT_MS) != I2C_STATUS_SUCCESS) {
                    status = MAXTOUCH_DEBUG_I2C_ERR;
                }
            }
            break;
        }
        default: {
            status = MAXTOUCH_DEBUG_INVALID_CMD;
        }
    }

    data[0] = (uint8_t)status;
    raw_hid_send(data, length);
}
#endif
