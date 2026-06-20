/* Copyright 2023 @ Keychron (https://www.keychron.com)
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

#include "color.h"
#include "keycodes.h"
#include "rgb_matrix.h"
#include "raw_hid.h"
#include QMK_KEYBOARD_H

// clang-format off
enum layers{
  MAC_BASE,
  MAC_FN,
  WIN_BASE,
  WIN_FN
};

// Tap Dance declarations. The order here defines the slot index used both in the
// keymap (TD(...)) and in the EEPROM config / raw HID protocol.
enum {
    TD_NO_CAPS,
    TD_HOME_END,
    TD_ESC_CW,
    TD_SCLN_CLN,
    TD_F_PSCR,
    TD_F_SCRL,
    TD_F_PAUS,
    TD_F_NUM,
    // TD_RSFT_NUM,
    TD_NAMED_COUNT  // number of named/preconfigured slots
};

// Total configurable tap dance slots. Slots beyond the named ones default to
// KC_NO/disabled and can be assigned keycodes at runtime. Capped at 32 because
// td_enabled/td_mode are 32-bit bitfields.
#define TD_SLOT_COUNT 32

#define DEFAULT_TAPPING_TERM 200

#define TD_MODE_DOUBLE   0  // secondary keycode fires on double-tap
#define TD_MODE_TAP_HOLD 1  // secondary keycode fires on hold

// Per-slot tap dance keycodes (stored in EEPROM).
typedef struct {
    uint16_t tap;        // primary keycode (single tap)
    uint16_t secondary;  // double-tap or hold keycode, depending on mode
} td_kc_t;

// Runtime configuration persisted to the EEPROM user data block.
// 32-bit fields are placed first to avoid alignment padding. Bumping
// EECONFIG_USER_DATA_VERSION (config.h) invalidates old layouts so defaults
// are reapplied after a format change.
typedef struct {
    uint32_t td_enabled;          // bit i: slot i secondary action enabled
    uint32_t td_mode;             // bit i: 0 = double-tap, 1 = tap-hold
    uint16_t tapping_term;        // 0 = uninitialized sentinel
    uint8_t  reserved[6];         // pad header to 16 bytes
    td_kc_t  td[TD_SLOT_COUNT];   // 32 * 4 = 128 bytes
} user_config_t;

_Static_assert(sizeof(user_config_t) == 144, "user_config_t must match EECONFIG_USER_DATA_SIZE");

static user_config_t user_config;

// Compile-time defaults. TD_SCLN_CLN ships as a disabled tap-hold so placing
// TD(TD_SCLN_CLN) in the layout behaves like a plain key until enabled at runtime.
static const user_config_t default_config = {
    .tapping_term = DEFAULT_TAPPING_TERM,
    // all named slots enabled except the ;/: dance; unnamed slots (8+) disabled
    .td_enabled   = ((1u << TD_NAMED_COUNT) - 1) & ~(1u << TD_SCLN_CLN),
    .td_mode      = (1u << TD_SCLN_CLN),  // ;/: is tap-hold, rest double
    .td = {
        [TD_NO_CAPS]  = {KC_NO,   KC_CAPS},
        [TD_HOME_END] = {KC_HOME, KC_END},
        [TD_ESC_CW]   = {KC_ESC,  KC_CAPS},
        [TD_SCLN_CLN] = {KC_SCLN, KC_COLN},
        [TD_F_PSCR]   = {KC_F9,   KC_PSCR},
        [TD_F_SCRL]   = {KC_F10,  KC_SCRL},
        [TD_F_PAUS]   = {KC_F11,  KC_PAUS},
        [TD_F_NUM]    = {KC_F12,  KC_NUM},
    },
};

// ----- Unified, runtime-configurable tap dance -----
// Every tap dance slot uses the same callbacks; behavior (keycodes, double vs
// tap-hold mode, enabled) is read from user_config at runtime. user_data carries
// the slot index. td_registered tracks what was pressed so reset can release it.
static uint16_t td_registered[TD_SLOT_COUNT];

static inline bool td_slot_enabled(uint8_t i) { return user_config.td_enabled & (1u << i); }
static inline bool td_slot_hold(uint8_t i)    { return user_config.td_mode    & (1u << i); }

void td_each(tap_dance_state_t *state, void *ud) {
    uint8_t i = (uint8_t)(uintptr_t)ud;
    if (td_slot_hold(i)) return;  // tap-hold resolves in finished/reset
    // double mode: fire the secondary keycode on the second tap when enabled
    if (state->count == 2 && td_slot_enabled(i)) {
        td_registered[i] = user_config.td[i].secondary;
        register_code16(td_registered[i]);
        state->finished = true;
    }
}

void td_finished(tap_dance_state_t *state, void *ud) {
    uint8_t i = (uint8_t)(uintptr_t)ud;
    if (td_slot_hold(i)) {
        // tap-hold: only act if the key is still held at resolution time. A quick
        // tap (released early) is emitted by process_record_user instead, so the
        // tap isn't delayed by the tapping term.
        if (state->pressed) {
            if (state->count == 1 && td_slot_enabled(i)
#ifndef PERMISSIVE_HOLD
                && !state->interrupted
#endif
            ) {
                td_registered[i] = user_config.td[i].secondary;  // hold
            } else {
                td_registered[i] = user_config.td[i].tap;        // disabled / repeated
            }
            register_code16(td_registered[i]);
        }
    } else {
        // double mode: single tap (or disabled) -> primary keycode.
        if (state->count == 1 || !td_slot_enabled(i)) {
            td_registered[i] = user_config.td[i].tap;
            register_code16(td_registered[i]);
        }
        // count==2 enabled already handled in td_each
    }
}

void td_reset(tap_dance_state_t *state, void *ud) {
    uint8_t i = (uint8_t)(uintptr_t)ud;
    if (!td_slot_hold(i)) wait_ms(TAP_CODE_DELAY);
    if (td_registered[i]) {
        unregister_code16(td_registered[i]);
        td_registered[i] = 0;
    }
}

#define ACTION_TD_SLOT(idx) \
    { .fn = {td_each, td_finished, td_reset, NULL}, .user_data = (void *)(uintptr_t)(idx) }

// Every slot index 0..TD_SLOT_COUNT-1 needs a valid entry so any slot can be
// placed in the keymap via TD(n). All use the same callbacks; behavior comes
// from user_config. Named slots are spelled out; the rest are filled by index.
#define S4(n)  ACTION_TD_SLOT(n),     ACTION_TD_SLOT((n)+1),  \
               ACTION_TD_SLOT((n)+2), ACTION_TD_SLOT((n)+3)
tap_dance_action_t tap_dance_actions[TD_SLOT_COUNT] = {
    [TD_NO_CAPS]  = ACTION_TD_SLOT(TD_NO_CAPS),
    [TD_HOME_END] = ACTION_TD_SLOT(TD_HOME_END),
    [TD_ESC_CW]   = ACTION_TD_SLOT(TD_ESC_CW),
    [TD_SCLN_CLN] = ACTION_TD_SLOT(TD_SCLN_CLN),
    [TD_F_PSCR]   = ACTION_TD_SLOT(TD_F_PSCR),
    [TD_F_SCRL]   = ACTION_TD_SLOT(TD_F_SCRL),
    [TD_F_PAUS]   = ACTION_TD_SLOT(TD_F_PAUS),
    [TD_F_NUM]    = ACTION_TD_SLOT(TD_F_NUM),
    // slots 8..31 (generic, runtime-configurable)
    [8] = S4(8), S4(12), S4(16), S4(20), S4(24), S4(28),
};
_Static_assert(TD_SLOT_COUNT == 32, "tap_dance_actions fill assumes 32 slots");
#undef S4

// ----- Raw HID runtime configuration -----
// Custom command byte 0xAC, routed via q1_pro.c's via_command_user() hook.
#define HID_CMD_BYTE     0xAC
#define HID_GET_GLOBAL   0x01  // -> tt_lo, tt_hi, slot_count, td_enabled[4], td_mode[4]
#define HID_SET_TT       0x02  // tt_lo, tt_hi
#define HID_SET_TD_EN    0x03  // idx, enable(0/1)
#define HID_SET_TD_MODE  0x04  // idx, mode(0=double,1=hold)
#define HID_RESET        0x05
#define HID_GET_TD       0x06  // idx -> idx, tap_lo, tap_hi, sec_lo, sec_hi, enabled, mode
#define HID_SET_TD_KC    0x07  // idx, tap_lo, tap_hi, sec_lo, sec_hi
#define HID_IDENTIFY     0x08  // ack, then report next keypress: row, col, kc_lo, kc_hi
#define HID_OK           0x00
#define HID_ERR          0xFF

// When set by an IDENTIFY command, the next keypress is captured and reported
// over raw HID (and consumed) instead of being typed.
static bool identify_pending = false;

// For tap-hold-mode slots, emit the tap immediately on a quick release so the
// primary key doesn't wait out the tapping term.
bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    if (identify_pending && record->event.pressed) {
        identify_pending = false;
        uint8_t buf[32] = {0};
        buf[0] = HID_CMD_BYTE;
        buf[1] = HID_IDENTIFY;
        buf[2] = record->event.key.row;
        buf[3] = record->event.key.col;
        buf[4] = keycode & 0xFF;
        buf[5] = (keycode >> 8) & 0xFF;
        raw_hid_send(buf, sizeof(buf));
        return false;  // consume the keypress so it isn't typed
    }
    if (IS_QK_TAP_DANCE(keycode)) {
        uint8_t i = TD_INDEX(keycode);
        if (i < TD_SLOT_COUNT && td_slot_hold(i)) {
            tap_dance_action_t *action = &tap_dance_actions[i];
            if (!record->event.pressed && action->state.count && !action->state.finished) {
                tap_code16(user_config.td[i].tap);
            }
        }
    }
    return true;
}

// get_tapping_term callback (TAPPING_TERM_PER_KEY): applies the runtime tapping
// term to all keys, including tap dance timing.
uint16_t get_tapping_term(uint16_t keycode, keyrecord_t *record) {
    return user_config.tapping_term;
}

static void hid_put_u32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

static void hid_put_global(uint8_t *data) {
    data[2] = user_config.tapping_term & 0xFF;
    data[3] = (user_config.tapping_term >> 8) & 0xFF;
    data[4] = TD_SLOT_COUNT;
    hid_put_u32(&data[5], user_config.td_enabled);  // data[5..8]
    hid_put_u32(&data[9], user_config.td_mode);     // data[9..12]
}

bool via_command_user(uint8_t *data, uint8_t length) {
    if (data[0] != HID_CMD_BYTE) return false;

    uint8_t idx = data[2];  // only valid for per-slot subcommands

    switch (data[1]) {
        case HID_GET_GLOBAL:
            data[1] = HID_OK;
            hid_put_global(data);
            break;
        case HID_SET_TT: {
            uint16_t tt = (uint16_t)data[2] | ((uint16_t)data[3] << 8);
            if (tt == 0) { data[1] = HID_ERR; break; }
            user_config.tapping_term = tt;
            eeconfig_update_user_datablock(&user_config);
            data[1] = HID_OK;
            hid_put_global(data);
            break;
        }
        case HID_SET_TD_EN:
            if (idx >= TD_SLOT_COUNT) { data[1] = HID_ERR; break; }
            if (data[3]) user_config.td_enabled |=  (1u << idx);
            else         user_config.td_enabled &= ~(1u << idx);
            eeconfig_update_user_datablock(&user_config);
            data[1] = HID_OK;
            hid_put_global(data);
            break;
        case HID_SET_TD_MODE:
            if (idx >= TD_SLOT_COUNT) { data[1] = HID_ERR; break; }
            if (data[3]) user_config.td_mode |=  (1u << idx);
            else         user_config.td_mode &= ~(1u << idx);
            eeconfig_update_user_datablock(&user_config);
            data[1] = HID_OK;
            hid_put_global(data);
            break;
        case HID_GET_TD:
            if (idx >= TD_SLOT_COUNT) { data[1] = HID_ERR; break; }
            data[1] = HID_OK;
            data[2] = idx;
            data[3] = user_config.td[idx].tap & 0xFF;
            data[4] = (user_config.td[idx].tap >> 8) & 0xFF;
            data[5] = user_config.td[idx].secondary & 0xFF;
            data[6] = (user_config.td[idx].secondary >> 8) & 0xFF;
            data[7] = td_slot_enabled(idx) ? 1 : 0;
            data[8] = td_slot_hold(idx) ? 1 : 0;
            break;
        case HID_SET_TD_KC:
            if (idx >= TD_SLOT_COUNT) { data[1] = HID_ERR; break; }
            user_config.td[idx].tap       = (uint16_t)data[3] | ((uint16_t)data[4] << 8);
            user_config.td[idx].secondary = (uint16_t)data[5] | ((uint16_t)data[6] << 8);
            eeconfig_update_user_datablock(&user_config);
            data[1] = HID_OK;
            data[2] = idx;
            break;
        case HID_RESET:
            user_config = default_config;
            eeconfig_update_user_datablock(&user_config);
            data[1] = HID_OK;
            hid_put_global(data);
            break;
        case HID_IDENTIFY:
            identify_pending = true;  // next keypress reported via raw_hid_send
            data[1] = HID_OK;
            break;
        default:
            data[1] = HID_ERR;
            break;
    }

    raw_hid_send(data, length);
    return true;
}


const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
 [MAC_BASE] = LAYOUT_ansi_82(
        KC_ESC,   KC_BRID,  KC_BRIU,  KC_MCTL,  KC_LPAD,  RGB_VAD,  RGB_VAI,  KC_MPRV,  KC_MPLY,  KC_MNXT,  KC_MUTE,  KC_VOLD,  KC_VOLU,  KC_DEL,             KC_MUTE,
        KC_GRV,   KC_1,     KC_2,     KC_3,     KC_4,     KC_5,     KC_6,     KC_7,     KC_8,     KC_9,     KC_0,     KC_MINS,  KC_EQL,   KC_BSPC,            KC_PGUP,
        KC_TAB,   KC_Q,     KC_W,     KC_E,     KC_R,     KC_T,     KC_Y,     KC_U,     KC_I,     KC_O,     KC_P,     KC_LBRC,  KC_RBRC,  KC_BSLS,            KC_PGDN,
        KC_CAPS,  KC_A,     KC_S,     KC_D,     KC_F,     KC_G,     KC_H,     KC_J,     KC_K,     KC_L,     KC_SCLN,  KC_QUOT,            KC_ENT,             KC_HOME,
        KC_LSFT,            KC_Z,     KC_X,     KC_C,     KC_V,     KC_B,     KC_N,     KC_M,     KC_COMM,  KC_DOT,   KC_SLSH,            KC_RSFT,  KC_UP,
        KC_LCTL,  KC_LOPTN, KC_LCMMD,                               KC_SPC,                                 KC_LCMMD,MO(MAC_FN),KC_RCTL,  KC_LEFT,  KC_DOWN,  KC_RGHT),

    [MAC_FN] = LAYOUT_ansi_82(
        KC_TRNS,  KC_F1,    KC_F2,    KC_F3,    KC_F4,    KC_F5,    KC_F6,    KC_F7,    KC_F8,    KC_F9,    KC_F10,   KC_F11,   KC_F12,   KC_TRNS,            RGB_TOG,
        KC_TRNS,  BT_HST1,  BT_HST2,  BT_HST3,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,            KC_TRNS,
        RGB_TOG,  RGB_MOD,  RGB_VAI,  RGB_HUI,  RGB_SAI,  RGB_SPI,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,            KC_TRNS,
        KC_TRNS,  RGB_RMOD, RGB_VAD,  RGB_HUD,  RGB_SAD,  RGB_SPD,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,            KC_TRNS,            KC_END,
        KC_TRNS,            KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,  BAT_LVL,  NK_TOGG,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,            KC_TRNS,  KC_TRNS,
        KC_TRNS,  KC_TRNS,  KC_TRNS,                                KC_TRNS,                                KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS),

    [WIN_BASE] = LAYOUT_ansi_82(
        KC_ESC,   KC_F1,    KC_F2,    KC_F3,    KC_F4,    KC_F5,    KC_F6,    KC_F7,    KC_F8,    TD(TD_F_PSCR),    TD(TD_F_SCRL),   TD(TD_F_PAUS),   TD(TD_F_NUM),   KC_DEL,             KC_MUTE,
        KC_GRV,   KC_1,     KC_2,     KC_3,     KC_4,     KC_5,     KC_6,     KC_7,     KC_8,     KC_9,     KC_0,     KC_MINS,  KC_EQL,   KC_BSPC,            KC_PGUP,
        KC_TAB,   KC_Q,     KC_W,     KC_E,     KC_R,     KC_T,     KC_Y,     KC_U,     KC_I,     KC_O,     KC_P,     KC_LBRC,  KC_RBRC,  KC_BSLS,            KC_PGDN,
        TD(TD_NO_CAPS) /*KC_CAPS*/,  KC_A,     KC_S,     KC_D,     KC_F,     KC_G,     KC_H,     KC_J,     KC_K,     KC_L,     TD(TD_SCLN_CLN),  KC_QUOT,            KC_ENT,             TD(TD_HOME_END) /*KC_HOME*/,
        KC_LSFT,            KC_Z,     KC_X,     KC_C,     KC_V,     KC_B,     KC_N,     KC_M,     KC_COMM,  KC_DOT,   KC_SLSH,            /*TD(TD_RSFT_NUM)*/ KC_RSFT,  KC_UP,
        KC_LCTL,  KC_LGUI,  KC_LALT,                                KC_SPC,                                 KC_RALT, MO(WIN_FN),KC_RCTL,  KC_LEFT,  KC_DOWN,  KC_RGHT),

    [WIN_FN] = LAYOUT_ansi_82(
        KC_TRNS,  KC_BRID,  KC_BRIU,  KC_TASK,  KC_FILE,  RGB_VAD,  RGB_VAI,  KC_MPRV,  KC_MPLY,  KC_MNXT,  KC_MUTE,  KC_VOLD,  KC_VOLU,  KC_TRNS,            RGB_TOG,
        KC_TRNS,  BT_HST1,  BT_HST2,  BT_HST3,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,            KC_TRNS,
        RGB_TOG,  RGB_MOD,  RGB_VAI,  RGB_HUI,  RGB_SAI,  RGB_SPI,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,            KC_TRNS,
        KC_TRNS,  RGB_RMOD, RGB_VAD,  RGB_HUD,  RGB_SAD,  RGB_SPD,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,            KC_TRNS,            KC_END,
        KC_TRNS,            KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,  BAT_LVL,  NK_TOGG,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,            KC_TRNS,  KC_TRNS,
        KC_TRNS,  KC_TRNS,  KC_TRNS,                                KC_TRNS,                                KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS)

};

#if defined(ENCODER_MAP_ENABLE)
const uint16_t PROGMEM encoder_map[][NUM_ENCODERS][2] = {
    [MAC_BASE] = {ENCODER_CCW_CW(KC_VOLD, KC_VOLU)},
    [MAC_FN] = {ENCODER_CCW_CW(RGB_VAD, RGB_VAI)},
    [WIN_BASE] = {ENCODER_CCW_CW(KC_VOLD, KC_VOLU)},
    [WIN_FN] = {ENCODER_CCW_CW(RGB_VAD, RGB_VAI)}
};
#endif // ENCODER_MAP_ENABLE

// Called by QMK when the user data block is missing/invalid (first flash or
// after a version bump).
void eeconfig_init_user_datablock(void) {
    user_config = default_config;
    eeconfig_update_user_datablock(&user_config);
}

void keyboard_post_init_user(void) {
    // Load runtime config; read returns zeros if the block is invalid, so a
    // tapping_term of 0 means "uninitialized" -> apply defaults.
    eeconfig_read_user_datablock(&user_config);
    if (user_config.tapping_term == 0) {
        user_config = default_config;
        eeconfig_update_user_datablock(&user_config);
    }

    // rgblight_enable_noeeprom();
    rgblight_enable_noeeprom(); // enables Rgb, without saving settings
    rgblight_sethsv_noeeprom(180, 255, 255); // sets the color to teal/cyan without saving
    // rgblight_mode_noeeprom(RGBLIGHT_MODE_BREATHING + 3); // sets mode to Fast breathing without saving
    rgb_matrix_sethsv_noeeprom(170, 180, 128);
}

extern bool is_caps_word_on(void);
bool rgb_matrix_indicators_advanced_user(uint8_t led_min, uint8_t led_max) {
    for (uint8_t i = led_min; i < led_max; i++) {
        if (host_keyboard_led_state().caps_lock) {
            if (g_led_config.flags[i] & LED_FLAG_KEYLIGHT) {
                rgb_matrix_set_color(i, RGB_RED);
            }
        }
        else if (is_caps_word_on()) {
            if (g_led_config.flags[i] & LED_FLAG_KEYLIGHT) {
                rgb_matrix_set_color(i, 0x00, 0xA0, 0x00);
            }
        }
        else {
            switch(get_highest_layer(layer_state|default_layer_state)) {
                // case 0:
                //     rgb_matrix_set_color(i, 0x40, 0x26, 0x16);
                //     break;
                // case 1:
                //     rgb_matrix_set_color(i, RGB_ORANGE);
                //     break;
                // case 2:
                //     rgb_matrix_set_color(i, 0x16, 0x26, 0x40);
                //     break;
                case 3:
                    rgb_matrix_set_color(i, 0x80, 0x80, 0x00);
                    break;
                default:
                    break;
            }
        }
    }
    // if (host_keyboard_led_state().caps_lock) {
    //     for (uint8_t i = led_min; i < led_max; i++) {
    //         if (g_led_config.flags[i] & LED_FLAG_KEYLIGHT) {
    //             rgb_matrix_set_color(i, RGB_RED);
    //         }
    //     }
    // }
    // if (is_caps_word_on()) {
    //     for (uint8_t i = led_min; i < led_max; i++) {
    //         if (g_led_config.flags[i] & LED_FLAG_KEYLIGHT) {
    //             rgb_matrix_set_color(i, 180, 72, 0);
    //         }
    //     }
    // }
    return false;
}
