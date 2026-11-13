/**
 * Copyright 2021 Charly Delay <charly@codesink.dev> (@0xcharly)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include QMK_KEYBOARD_H

enum charybdis_keymap_layers {
    LAYER_BASE = 0,
    LAYER_NUMBERS,
    LAYER_SYMBOLS,
    LAYER_ARROWS,
    LAYER_SETTINGS,
};

// Layer activation shortcuts
#define NUMBERS  MO(LAYER_NUMBERS)
#define SYMBOLS  MO(LAYER_SYMBOLS)
#define ARROWS   MO(LAYER_ARROWS)
#define SETTINGS MO(LAYER_SETTINGS)

#define CTL_BSP CTL_T(KC_BSPC)
#define SFT_SPC SFT_T(KC_SPC)
#define GUI_ENT GUI_T(KC_ENT)

// Short aliases for Home Row Mods
#define HM_A LSFT_T(KC_A)
#define HM_S LCTL_T(KC_S)
#define HM_D LALT_T(KC_D)
#define HM_F LGUI_T(KC_F)
#define HM_G LT(LAYER_NUMBERS, KC_G)

#define HM_J RGUI_T(KC_J)
#define HM_K LALT_T(KC_K)
#define HM_L RCTL_T(KC_L)
#define HM_SCLN RSFT_T(KC_SCLN)


#define HM_Q LT(LAYER_SETTINGS, KC_Q)
#define HM_ESC LT(LAYER_SYMBOLS, KC_ESC)
#define HM_Z LT(LAYER_ARROWS, KC_Z)

// Define the key pairs that trigger the combos
const uint16_t PROGMEM as_combo[]     = {HM_A, HM_S, COMBO_END};
const uint16_t PROGMEM jk_combo[]     = {HM_J, HM_K, COMBO_END};
const uint16_t PROGMEM l_scln_combo[] = {HM_L, HM_SCLN, COMBO_END};
const uint16_t PROGMEM lk_combo[]     = {HM_L, HM_K, COMBO_END};

// Map each combo to its output keycode
combo_t key_combos[] = {
    COMBO(as_combo, KC_TAB),
    COMBO(jk_combo, KC_GRV),      // Backtick (`)
    COMBO(l_scln_combo, KC_PIPE), // Pipe (|)
    COMBO(lk_combo, KC_BSPC),     // Backspace
};

// clang-format off
const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
  [LAYER_BASE] = LAYOUT(
  // ╭─────────────────────────────────────────────╮ ╭─────────────────────────────────────────────╮
          HM_Q,    KC_W,    KC_E,    KC_R,    KC_T,       KC_Y,    KC_U,    KC_I,    KC_O,    KC_P,
  // ├─────────────────────────────────────────────┤ ├─────────────────────────────────────────────┤
          HM_A,    HM_S,    HM_D,    HM_F,    HM_G,       KC_H,    HM_J,    HM_K,    HM_L, HM_SCLN,
  // ├─────────────────────────────────────────────┤ ├─────────────────────────────────────────────┤
          HM_Z,    KC_X,    KC_C,    KC_V,    KC_B,       KC_N,    KC_M, KC_COMM,  KC_DOT, KC_SLSH,
  // ╰─────────────────────────────────────────────┤ ├─────────────────────────────────────────────╯
                                     ARROWS,  KC_SPC,     KC_ENT,    HM_ESC, KC_BSPC
  //                   ╰───────────────────────────╯ ╰──────────────────╯
  ),

  /* 1: NUMBERS LAYER (Custom numpad: U=7, I=8, O=9 | J=4, K=5, L=6 | N=0, M=1, ,=2, .=3) */
    [LAYER_NUMBERS] = LAYOUT(
        KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS,     KC_TRNS, KC_7,    KC_8,    KC_9,    KC_PLUS,
        KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS,     KC_TRNS, KC_4,    KC_5,    KC_6,    KC_MINS,
        KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS,     KC_0,    KC_1,    KC_2,    KC_3,    KC_DOT,
                          KC_TRNS, KC_TRNS,     KC_TRNS, KC_TRNS, KC_TRNS
    ),

    /* 2: SYMBOLS LAYER */
    [LAYER_SYMBOLS] = LAYOUT(
        KC_EXLM, KC_AT,   KC_HASH, KC_DLR,  KC_PERC,     KC_TRNS, KC_CIRC, KC_AMPR, KC_ASTR, KC_BSLS,
        KC_CAPS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS,     KC_TRNS, KC_EQL,  KC_LPRN, KC_RPRN, KC_QUOT,
        KC_NO,   KC_NO,   KC_NO,   KC_NO,   KC_NO,       KC_TRNS, KC_MINS, KC_LBRC, KC_RBRC, KC_GRV,
                          KC_TRNS, KC_TRNS,     KC_TRNS, KC_TRNS, KC_TRNS
    ),

    /* 3: ARROWS LAYER (UIOP = Home/PgUp/PgDn/End, HJKL = Left/Down/Up/Right) */
    [LAYER_ARROWS] = LAYOUT(
        KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS,     KC_HOME, KC_PGUP, KC_PGDN, KC_END,  KC_TRNS,
        KC_CAPS, KC_TRNS, MS_BTN2, MS_BTN1, KC_TRNS,     KC_LEFT, KC_DOWN, KC_UP,   KC_RGHT, KC_TRNS,
        KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS,     KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS,
                          KC_TRNS, KC_TRNS,     KC_TRNS, KC_TRNS, KC_TRNS
    ),

    /* 4: SETTINGS LAYER (Empty placeholders) */
    [LAYER_SETTINGS] = LAYOUT(
        KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS,     KC_F12, KC_F7, KC_F8, KC_F9, KC_TRNS,
        KC_CAPS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS,     KC_F11, KC_F4, KC_F5, KC_F6, KC_TRNS,
        KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS,     KC_F10, KC_F1, KC_F2, KC_F3, KC_TRNS,
                          KC_TRNS, KC_TRNS,     KC_TRNS, KC_TRNS, KC_TRNS
    ),
};
// clang-format on
