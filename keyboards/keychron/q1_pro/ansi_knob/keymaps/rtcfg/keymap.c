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

// Runtime config (raw HID, command byte 0xAC) is driven by the companion host
// app in its own repo: C:\Users\mitch\source\repos\q1config (q1config.py CLI +
// q1config.html WebHID GUI). Wire protocol documented there in PROTOCOL.md.
#include "color.h"
#include "keycodes.h"
#include "rgb_matrix.h"
#include "raw_hid.h"
#include "action_util.h"
#include "process_combo.h"
#include "process_key_override.h"
#include "rtcfg.h"
#include QMK_KEYBOARD_H

// clang-format off
enum layers{
  MAC_BASE,
  MAC_FN,
  WIN_BASE,
  WIN_FN
};

// Tap Dance declarations. Slots are named TD0..TD63 so a slot's name is its
// index everywhere (keymap TD(...), EEPROM config, raw HID protocol, and the
// host config tool) — this makes a slot trivial to find. The first eight ship
// with sensible default keycodes (see default_config); the rest start blank.
enum {
    TD0,  // caps lock
    TD1,  // home / end
    TD2,  // esc / caps word
    TD3,  // ; / :
    TD4,  // F9 / print screen
    TD5,  // F10 / scroll lock
    TD6,  // F11 / pause
    TD7,  // F12 / num lock
    TD_NAMED_COUNT  // number of slots with default keycodes
};

// Total configurable tap dance slots. Slots beyond the named ones default to
// KC_NO/disabled and can be assigned keycodes at runtime. td_enabled/td_mode are
// 64-bit bitfields (only the low TD_SLOT_COUNT bits are used).
#define TD_SLOT_COUNT 32

#define DEFAULT_TAPPING_TERM 200
#define DEFAULT_DEBOUNCE     5   // ms; matches QMK's default DEBOUNCE

#define TD_MODE_DOUBLE   0  // secondary keycode fires on double-tap
#define TD_MODE_TAP_HOLD 1  // secondary keycode fires on hold

// Per-slot tap dance keycodes (stored in EEPROM).
typedef struct {
    uint16_t tap;        // primary keycode (single tap)
    uint16_t secondary;  // double-tap or hold keycode, depending on mode
} td_kc_t;

// Runtime combos. Up to COMBO_MAX_KEYS input keycodes (KC_NO-padded) fire the
// output keycode when pressed together. Stored in EEPROM; a live combo_t table
// is rebuilt from these (see rebuild_combos).
#define COMBO_SLOT_COUNT 16
#define COMBO_MAX_KEYS    4
typedef struct {
    uint16_t keys[COMBO_MAX_KEYS];  // KC_NO-padded input keycodes
    uint16_t output;                // keycode emitted when the combo fires
} combo_def_t;

// Runtime key overrides. Mirrors the fields of QMK's key_override_t that we
// expose; a live key_override_t table is rebuilt from these (see rebuild_kos).
#define KO_SLOT_COUNT 16
typedef struct {
    uint16_t trigger;            // non-modifier keycode that triggers the override
    uint16_t replacement;        // keycode sent instead
    uint8_t  trigger_mods;       // mods that must be held
    uint8_t  suppressed_mods;    // mods hidden from the host while active
    uint8_t  negative_mod_mask;  // mods that must NOT be held
    uint8_t  layers;             // bitmask of layers this applies to (bit i = layer i)
    uint8_t  options;            // ko_option_t bits
    uint8_t  _pad;               // keep struct size even/deterministic
} ko_def_t;

// feature_flags bit positions
#define FF_CAPS_WORD_EN          (1u << 0)
#define FF_PERMISSIVE_HOLD       (1u << 1)
#define FF_HOLD_ON_OTHER_KEY     (1u << 2)
#define FF_RETRO_TAPPING         (1u << 3)
#define FF_AUTOSHIFT_EN          (1u << 4)
#define FF_CW_DOUBLE_SHIFT       (1u << 5)  // double-tap LShift turns on Caps Word
#define FF_CW_BOTH_SHIFTS        (1u << 6)  // hold both shifts turns on Caps Word

// Whole-board RGB indicator per state: on/off + color.
typedef struct {
    uint8_t enabled;
    uint8_t r, g, b;
} indicator_t;
enum { IND_CAPS_LOCK, IND_CAPS_WORD, IND_WIN_FN, INDICATOR_COUNT };

// Runtime configuration persisted to the EEPROM user data block.
// 64-bit fields are placed first to avoid alignment padding. Bumping
// EECONFIG_USER_DATA_VERSION (config.h) invalidates old layouts so defaults
// are reapplied after a format change.
typedef struct {
    uint64_t td_enabled;          // bit i: slot i secondary action enabled
    uint64_t td_mode;             // bit i: 0 = double-tap, 1 = tap-hold
    uint16_t tapping_term;        // 0 = uninitialized sentinel
    uint16_t quick_tap_term;      // get_quick_tap_term()
    uint16_t autoshift_timeout;   // set_autoshift_timeout()
    uint16_t caps_word_timeout;   // our runtime idle timeout (ms; 0 = never)
    uint16_t feature_flags;       // see FF_* bits
    indicator_t indicators[INDICATOR_COUNT]; // 3 * 4 = 12 bytes
    uint8_t  debounce_time;       // ms; custom debounce reads this (0 = none)
    uint8_t  debounce_method;     // 0 none, 1 sym_defer_g, 2 sym_eager_pk, 3 asym_eager_defer_pk
    td_kc_t  td[TD_SLOT_COUNT];   // 32 * 4 = 128 bytes
    uint16_t oneshot_timeout;     // ms; runtime one-shot idle timeout (0 = never)
    uint16_t combo_enabled;       // bit i: combo slot i active
    uint16_t ko_enabled;          // bit i: key-override slot i active
    combo_def_t combos[COMBO_SLOT_COUNT]; // 16 * 10 = 160 bytes
    ko_def_t    kos[KO_SLOT_COUNT];       // 16 * 10 = 160 bytes
} user_config_t;

_Static_assert(sizeof(user_config_t) == 496, "user_config_t must match EECONFIG_USER_DATA_SIZE");

static user_config_t user_config;

// Neutral/stock compile-time defaults: every tap-dance slot DISABLED and in
// double mode, with the primary keycode = the natural key for its position, so a
// fresh flash behaves like a normal keyboard. All extra features off. Personal
// preferences are applied at runtime (q1config.py `mine`) and live in EEPROM
// only — sharing this firmware does not impose the author's setup.
static const user_config_t default_config = {
    .tapping_term      = DEFAULT_TAPPING_TERM,
    .quick_tap_term    = 120,
    .autoshift_timeout = 175,
    .caps_word_timeout = 5000,
    .debounce_time     = DEFAULT_DEBOUNCE,
    .debounce_method   = 1,  // sym_defer_g, matches QMK's stock default
    .feature_flags     = 0,
    .oneshot_timeout   = 1000,  // ms; one-shot mod/layer expires after 1s idle
    .combo_enabled     = 0,     // all combos disabled until configured
    .ko_enabled        = 0,     // all key overrides disabled until configured
    // combos[] and kos[] zero-initialized (unlisted fields default to 0)
    // Indicators ship disabled, with the author's colors pre-stored so enabling
    // alone reproduces the original look (red caps lock / green caps word / olive FN).
    .indicators = {
        [IND_CAPS_LOCK] = {0, 0xFF, 0x00, 0x00},
        [IND_CAPS_WORD] = {0, 0x00, 0xA0, 0x00},
        [IND_WIN_FN]    = {0, 0x80, 0x80, 0x00},
    },
    .td_enabled        = 0,
    .td_mode           = 0,
    // All tap-dance slots ship blank/disabled (stock): td[] is left zero-initialized.
    // Personal slot keycodes and key->TD assignments are applied at runtime by
    // loading a preset (`mine`).
};

// ----- Unified, runtime-configurable tap dance -----
// Every tap dance slot uses the same callbacks; behavior (keycodes, double vs
// tap-hold mode, enabled) is read from user_config at runtime. user_data carries
// the slot index. td_registered tracks what was pressed so reset can release it.
static uint16_t td_registered[TD_SLOT_COUNT];

static inline bool td_slot_enabled(uint8_t i) { return user_config.td_enabled & (1ULL << i); }
static inline bool td_slot_hold(uint8_t i)    { return user_config.td_mode    & (1ULL << i); }

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
            // Honor the runtime permissive-hold flag: when set, a hold resolves
            // even if another key interrupted it.
            if (state->count == 1 && td_slot_enabled(i)
                && ((user_config.feature_flags & FF_PERMISSIVE_HOLD) || !state->interrupted)
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
    [TD0] = ACTION_TD_SLOT(TD0),
    [TD1] = ACTION_TD_SLOT(TD1),
    [TD2] = ACTION_TD_SLOT(TD2),
    [TD3] = ACTION_TD_SLOT(TD3),
    [TD4] = ACTION_TD_SLOT(TD4),
    [TD5] = ACTION_TD_SLOT(TD5),
    [TD6] = ACTION_TD_SLOT(TD6),
    [TD7] = ACTION_TD_SLOT(TD7),
    // slots 8..31 (runtime-configurable, blank until assigned)
    [8] = S4(8), S4(12), S4(16), S4(20), S4(24), S4(28),
};
_Static_assert(TD_SLOT_COUNT == 32, "tap_dance_actions fill assumes 32 slots");
#undef S4

// ----- Raw HID runtime configuration -----
// Custom command byte 0xAC, routed via q1_pro.c's via_command_user() hook.
#define HID_CMD_BYTE     0xAC
#define HID_GET_GLOBAL   0x01  // -> tt_lo, tt_hi, slot_count, td_enabled[8], td_mode[8], combo_count, ko_count
#define HID_SET_TT       0x02  // tt_lo, tt_hi
#define HID_SET_TD_EN    0x03  // idx, enable(0/1)
#define HID_SET_TD_MODE  0x04  // idx, mode(0=double,1=hold)
#define HID_RESET        0x05
#define HID_GET_TD       0x06  // idx -> idx, tap_lo, tap_hi, sec_lo, sec_hi, enabled, mode
#define HID_SET_TD_KC    0x07  // idx, tap_lo, tap_hi, sec_lo, sec_hi
#define HID_IDENTIFY     0x08  // ack, then report next keypress: row, col, kc_lo, kc_hi
#define HID_GET_FEATURES 0x09  // -> flags[2], quick_tap[2], as_timeout[2], cw_timeout[2], debounce[1], debounce_method[1], oneshot_timeout[2]
#define HID_SET_FLAG     0x0A  // bit_idx, value(0/1)
#define HID_SET_PARAM    0x0B  // param_id(0=quicktap,1=astimeout,2=cwtimeout,3=debounce,4=debounce_method,5=oneshot_timeout), lo, hi
#define HID_GET_INDICATOR 0x0C // idx -> idx, enabled, r, g, b
#define HID_SET_INDICATOR 0x0D // idx, enabled, r, g, b
#define HID_GET_COMBO    0x0E  // idx -> idx, key0..3[2 each], output[2], enabled
#define HID_SET_COMBO    0x0F  // idx, key0..3[2 each], output[2], enabled
#define HID_GET_KO       0x10  // idx -> idx, trig[2], repl[2], trig_mods, supp_mods, neg_mods, layers, options, enabled
#define HID_SET_KO       0x11  // idx, trig[2], repl[2], trig_mods, supp_mods, neg_mods, layers, options, enabled
#define HID_OK           0x00
#define HID_ERR          0xFF

// When set by an IDENTIFY command, the next keypress is captured and reported
// over raw HID (and consumed) instead of being typed.
static bool identify_pending = false;

// Last-activity timestamp for our runtime Caps Word idle timeout.
static uint16_t caps_word_timer = 0;

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
    if (record->event.pressed && is_caps_word_on()) {
        caps_word_timer = timer_read();  // reset our idle timeout on activity
    }

    // Runtime Caps Word shift activation (replaces the compile-time
    // DOUBLE_TAP_SHIFT/BOTH_SHIFTS defines). Gated on the master Caps Word flag
    // plus each method's own flag; only acts while Caps Word is off.
    //
    // This runs in process_record_user, *before* process_caps_word and before
    // the current key's modifier is registered. On the activating shift press we
    // must consume the key (return false): otherwise that same press flows into
    // process_caps_word's on-state handling, where a plain Shift hits
    // caps_word_press_user()'s default (returns false) and immediately turns
    // Caps Word back off. Consuming also avoids a stray Shift and lets us read
    // the second shift by folding its bit into the current mods.
    if ((user_config.feature_flags & FF_CAPS_WORD_EN) && !is_caps_word_on()
        && record->event.pressed) {
        if (user_config.feature_flags & FF_CW_BOTH_SHIFTS) {
            uint8_t mods = get_mods() | get_oneshot_mods();
            if (keycode == KC_LSFT) mods |= MOD_BIT(KC_LSFT);
            if (keycode == KC_RSFT) mods |= MOD_BIT(KC_RSFT);
            if (mods == MOD_MASK_SHIFT) { caps_word_on(); return false; }
        }
        if (user_config.feature_flags & FF_CW_DOUBLE_SHIFT) {
            static bool     lsft_tapped = false;
            static uint16_t lsft_timer  = 0;
            if (keycode == KC_LSFT || keycode == OSM(MOD_LSFT)) {
                if (lsft_tapped && !timer_expired(record->event.time, lsft_timer)) {
                    lsft_tapped = false;
                    caps_word_on();
                    return false;  // consume so process_caps_word can't cancel it
                }
                lsft_tapped = true;
                lsft_timer  = record->event.time + user_config.tapping_term;
            } else {
                lsft_tapped = false;  // any other key resets the double-tap window
            }
        }
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

// ----- Tap-hold per-key callbacks (runtime-toggled via feature_flags) -----
uint16_t get_tapping_term(uint16_t keycode, keyrecord_t *record) {
    return user_config.tapping_term;
}
uint16_t get_quick_tap_term(uint16_t keycode, keyrecord_t *record) {
    return user_config.quick_tap_term;
}
bool get_permissive_hold(uint16_t keycode, keyrecord_t *record) {
    return user_config.feature_flags & FF_PERMISSIVE_HOLD;
}
bool get_hold_on_other_key_press(uint16_t keycode, keyrecord_t *record) {
    return user_config.feature_flags & FF_HOLD_ON_OTHER_KEY;
}
bool get_retro_tapping(uint16_t keycode, keyrecord_t *record) {
    return user_config.feature_flags & FF_RETRO_TAPPING;
}

// ----- Caps Word: runtime enable gate + runtime idle timeout -----
void caps_word_set_user(bool active) {
    if (active) {
        if (!(user_config.feature_flags & FF_CAPS_WORD_EN)) {
            caps_word_off();  // feature disabled at runtime -> cancel activation
            return;
        }
        caps_word_timer = timer_read();  // start idle timer
    }
}

// Runtime one-shot timeout. QMK's built-in ONESHOT_TIMEOUT is disabled (config.h),
// so we expire a pending one-shot mod/layer here, mirroring the caps-word timeout.
// A toggled one-shot layer (held via tap-toggle) never expires.
static uint16_t oneshot_timer    = 0;
static bool     oneshot_pending  = false;

void housekeeping_task_user(void) {
    if (is_caps_word_on() && user_config.caps_word_timeout > 0
        && timer_elapsed(caps_word_timer) > user_config.caps_word_timeout) {
        caps_word_off();
    }

    uint8_t osl_state = get_oneshot_layer_state();
    bool pending = get_oneshot_mods() != 0
                   || (osl_state != 0 && !(osl_state & ONESHOT_TOGGLED));
    if (pending && !oneshot_pending) {
        oneshot_timer = timer_read();  // start the window when a one-shot arms
    }
    oneshot_pending = pending;
    if (pending && user_config.oneshot_timeout > 0
        && timer_elapsed(oneshot_timer) > user_config.oneshot_timeout) {
        clear_oneshot_mods();
        clear_oneshot_layer_state(ONESHOT_OTHER_KEY_PRESSED);
    }
}

// ----- Runtime-configurable debounce -----
// QMK's debounce method and time are both compile-time, so we use a custom
// dispatcher (debounce_rt.c, enabled via rules.mk: DEBOUNCE_TYPE = custom) that
// switches algorithms at runtime. It reads the live settings through these
// accessors (declared in rtcfg.h) to avoid coupling to user_config_t's layout.
uint8_t rtcfg_debounce_time(void)   { return user_config.debounce_time; }
uint8_t rtcfg_debounce_method(void) { return user_config.debounce_method; }

// ----- Runtime combos -----
// QMK's combo table is normally compile-time. keymap_introspection.c #includes
// this file, so we can't override the weak combo_count()/combo_get() here (same
// translation unit). Instead we expose the full-size key_combos[] that the stock
// combo_count_raw()/combo_get_raw() serve, and rebuild it from user_config at
// runtime. Disabled/invalid slots get an empty key list (keys[0] == COMBO_END)
// so they never match. RAM key pointers are fine: this is an ARM MCU, where
// pgm_read_word is a plain memory read.
combo_t key_combos[COMBO_SLOT_COUNT];
static uint16_t combo_keys[COMBO_SLOT_COUNT][COMBO_MAX_KEYS + 1]; // COMBO_END-terminated

static void rebuild_combos(void) {
    for (uint8_t i = 0; i < COMBO_SLOT_COUNT; i++) {
        uint8_t k = 0;
        if ((user_config.combo_enabled & (1u << i)) && user_config.combos[i].output != KC_NO) {
            for (uint8_t j = 0; j < COMBO_MAX_KEYS; j++) {
                uint16_t kc = user_config.combos[i].keys[j];
                if (kc != KC_NO) combo_keys[i][k++] = kc;
            }
            if (k < 2) k = 0;  // a combo needs at least two input keys
        }
        combo_keys[i][k] = COMBO_END;  // k==0 => empty list => never fires
        key_combos[i] = (combo_t){ .keys = combo_keys[i],
                                   .keycode = user_config.combos[i].output };
    }
}

// ----- Runtime key overrides -----
// QMK reads the weak `key_overrides` (a NULL-terminated array of pointers). We
// point it at a RAM table rebuilt from user_config.kos. Only enabled slots are
// included; the array is always NULL-terminated.
static key_override_t        ko_rt[KO_SLOT_COUNT];
static const key_override_t *ko_ptrs[KO_SLOT_COUNT + 1];

static void rebuild_kos(void) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < KO_SLOT_COUNT; i++) {
        if (!(user_config.ko_enabled & (1u << i))) continue;
        const ko_def_t *d = &user_config.kos[i];
        if (d->trigger == KC_NO) continue;
        ko_rt[n] = (key_override_t){
            .trigger           = d->trigger,
            .replacement       = d->replacement,
            .trigger_mods      = d->trigger_mods,
            .suppressed_mods   = d->suppressed_mods,
            .negative_mod_mask = d->negative_mod_mask,
            .layers            = (layer_state_t)d->layers,
            .options           = (ko_option_t)d->options,
            .custom_action     = NULL,
            .context           = NULL,
            .enabled           = NULL,  // included only when enabled, so always on
        };
        ko_ptrs[n] = &ko_rt[n];
        n++;
    }
    ko_ptrs[n] = NULL;
    key_overrides = ko_ptrs;
}

// Apply settings whose live state lives outside user_config (Auto Shift keeps
// enabled/timeout in RAM, not EEPROM, so we push our stored values into it).
static void apply_runtime_config(void) {
    set_autoshift_timeout(user_config.autoshift_timeout);
    if (user_config.feature_flags & FF_AUTOSHIFT_EN) {
        autoshift_enable();
    } else {
        autoshift_disable();
    }
}

static void hid_put_u64(uint8_t *p, uint64_t v) {
    for (uint8_t i = 0; i < 8; i++) p[i] = (v >> (8 * i)) & 0xFF;
}

static void hid_put_u16(uint8_t *p, uint16_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
}

static void hid_put_global(uint8_t *data) {
    data[2] = user_config.tapping_term & 0xFF;
    data[3] = (user_config.tapping_term >> 8) & 0xFF;
    data[4] = TD_SLOT_COUNT;
    hid_put_u64(&data[5], user_config.td_enabled);   // data[5..12]
    hid_put_u64(&data[13], user_config.td_mode);     // data[13..20]
    data[21] = COMBO_SLOT_COUNT;                      // data[21]
    data[22] = KO_SLOT_COUNT;                         // data[22]
}

static void hid_put_features(uint8_t *data) {
    hid_put_u16(&data[2], user_config.feature_flags);     // data[2..3]
    hid_put_u16(&data[4], user_config.quick_tap_term);    // data[4..5]
    hid_put_u16(&data[6], user_config.autoshift_timeout); // data[6..7]
    hid_put_u16(&data[8], user_config.caps_word_timeout); // data[8..9]
    data[10] = user_config.debounce_time;                 // data[10]
    data[11] = user_config.debounce_method;               // data[11]
    hid_put_u16(&data[12], user_config.oneshot_timeout);  // data[12..13]
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
            if (data[3]) user_config.td_enabled |=  (1ULL << idx);
            else         user_config.td_enabled &= ~(1ULL << idx);
            eeconfig_update_user_datablock(&user_config);
            data[1] = HID_OK;
            hid_put_global(data);
            break;
        case HID_SET_TD_MODE:
            if (idx >= TD_SLOT_COUNT) { data[1] = HID_ERR; break; }
            if (data[3]) user_config.td_mode |=  (1ULL << idx);
            else         user_config.td_mode &= ~(1ULL << idx);
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
            apply_runtime_config();
            rebuild_combos();
            rebuild_kos();
            data[1] = HID_OK;
            hid_put_global(data);
            break;
        case HID_IDENTIFY:
            identify_pending = true;  // next keypress reported via raw_hid_send
            data[1] = HID_OK;
            break;
        case HID_GET_FEATURES:
            data[1] = HID_OK;
            hid_put_features(data);
            break;
        case HID_SET_FLAG: {
            uint8_t bit = data[2];
            if (bit > 15) { data[1] = HID_ERR; break; }
            if (data[3]) user_config.feature_flags |=  (1u << bit);
            else         user_config.feature_flags &= ~(1u << bit);
            apply_runtime_config();  // push Auto Shift enable state live
            eeconfig_update_user_datablock(&user_config);
            data[1] = HID_OK;
            hid_put_features(data);
            break;
        }
        case HID_SET_PARAM: {
            uint16_t val = (uint16_t)data[3] | ((uint16_t)data[4] << 8);
            switch (data[2]) {
                case 0: user_config.quick_tap_term    = val; break;
                case 1: user_config.autoshift_timeout = val; set_autoshift_timeout(val); break;
                case 2: user_config.caps_word_timeout = val; break;
                case 3: user_config.debounce_time     = (uint8_t)val; break;
                case 4: user_config.debounce_method   = (uint8_t)val; break;
                case 5: user_config.oneshot_timeout   = val; break;
                default: data[1] = HID_ERR; goto param_done;
            }
            eeconfig_update_user_datablock(&user_config);
            data[1] = HID_OK;
            hid_put_features(data);
        param_done:
            break;
        }
        case HID_GET_INDICATOR:
            if (idx >= INDICATOR_COUNT) { data[1] = HID_ERR; break; }
            data[1] = HID_OK;
            data[2] = idx;
            data[3] = user_config.indicators[idx].enabled;
            data[4] = user_config.indicators[idx].r;
            data[5] = user_config.indicators[idx].g;
            data[6] = user_config.indicators[idx].b;
            break;
        case HID_SET_INDICATOR:
            if (idx >= INDICATOR_COUNT) { data[1] = HID_ERR; break; }
            user_config.indicators[idx].enabled = data[3] ? 1 : 0;
            user_config.indicators[idx].r       = data[4];
            user_config.indicators[idx].g       = data[5];
            user_config.indicators[idx].b       = data[6];
            eeconfig_update_user_datablock(&user_config);
            data[1] = HID_OK;
            data[2] = idx;
            break;
        case HID_GET_COMBO: {
            if (idx >= COMBO_SLOT_COUNT) { data[1] = HID_ERR; break; }
            data[1] = HID_OK;
            data[2] = idx;
            const combo_def_t *c = &user_config.combos[idx];
            for (uint8_t j = 0; j < COMBO_MAX_KEYS; j++)
                hid_put_u16(&data[3 + j * 2], c->keys[j]);  // data[3..10]
            hid_put_u16(&data[11], c->output);              // data[11..12]
            data[13] = (user_config.combo_enabled & (1u << idx)) ? 1 : 0;
            break;
        }
        case HID_SET_COMBO: {
            if (idx >= COMBO_SLOT_COUNT) { data[1] = HID_ERR; break; }
            combo_def_t *c = &user_config.combos[idx];
            for (uint8_t j = 0; j < COMBO_MAX_KEYS; j++)
                c->keys[j] = (uint16_t)data[3 + j * 2] | ((uint16_t)data[4 + j * 2] << 8);
            c->output = (uint16_t)data[11] | ((uint16_t)data[12] << 8);
            if (data[13]) user_config.combo_enabled |=  (1u << idx);
            else          user_config.combo_enabled &= ~(1u << idx);
            eeconfig_update_user_datablock(&user_config);
            rebuild_combos();
            data[1] = HID_OK;
            data[2] = idx;
            break;
        }
        case HID_GET_KO: {
            if (idx >= KO_SLOT_COUNT) { data[1] = HID_ERR; break; }
            data[1] = HID_OK;
            data[2] = idx;
            const ko_def_t *k = &user_config.kos[idx];
            hid_put_u16(&data[3], k->trigger);      // data[3..4]
            hid_put_u16(&data[5], k->replacement);  // data[5..6]
            data[7]  = k->trigger_mods;
            data[8]  = k->suppressed_mods;
            data[9]  = k->negative_mod_mask;
            data[10] = k->layers;
            data[11] = k->options;
            data[12] = (user_config.ko_enabled & (1u << idx)) ? 1 : 0;
            break;
        }
        case HID_SET_KO: {
            if (idx >= KO_SLOT_COUNT) { data[1] = HID_ERR; break; }
            ko_def_t *k = &user_config.kos[idx];
            k->trigger           = (uint16_t)data[3] | ((uint16_t)data[4] << 8);
            k->replacement       = (uint16_t)data[5] | ((uint16_t)data[6] << 8);
            k->trigger_mods      = data[7];
            k->suppressed_mods   = data[8];
            k->negative_mod_mask = data[9];
            k->layers            = data[10];
            k->options           = data[11];
            if (data[12]) user_config.ko_enabled |=  (1u << idx);
            else          user_config.ko_enabled &= ~(1u << idx);
            eeconfig_update_user_datablock(&user_config);
            rebuild_kos();
            data[1] = HID_OK;
            data[2] = idx;
            break;
        }
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
        KC_ESC,   KC_F1,    KC_F2,    KC_F3,    KC_F4,    KC_F5,    KC_F6,    KC_F7,    KC_F8,    KC_F9,    KC_F10,   KC_F11,   KC_F12,   KC_DEL,             KC_MUTE,
        KC_GRV,   KC_1,     KC_2,     KC_3,     KC_4,     KC_5,     KC_6,     KC_7,     KC_8,     KC_9,     KC_0,     KC_MINS,  KC_EQL,   KC_BSPC,            KC_PGUP,
        KC_TAB,   KC_Q,     KC_W,     KC_E,     KC_R,     KC_T,     KC_Y,     KC_U,     KC_I,     KC_O,     KC_P,     KC_LBRC,  KC_RBRC,  KC_BSLS,            KC_PGDN,
        KC_CAPS,  KC_A,     KC_S,     KC_D,     KC_F,     KC_G,     KC_H,     KC_J,     KC_K,     KC_L,     KC_SCLN,  KC_QUOT,            KC_ENT,             KC_HOME,
        KC_LSFT,            KC_Z,     KC_X,     KC_C,     KC_V,     KC_B,     KC_N,     KC_M,     KC_COMM,  KC_DOT,   KC_SLSH,            KC_RSFT,  KC_UP,
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
    apply_runtime_config();  // push Auto Shift enable/timeout into its RAM state
    rebuild_combos();        // build live combo table from EEPROM
    rebuild_kos();           // build live key-override table from EEPROM

    // rgblight_enable_noeeprom();
    rgblight_enable_noeeprom(); // enables Rgb, without saving settings
    rgblight_sethsv_noeeprom(180, 255, 255); // sets the color to teal/cyan without saving
    // rgblight_mode_noeeprom(RGBLIGHT_MODE_BREATHING + 3); // sets mode to Fast breathing without saving
    rgb_matrix_sethsv_noeeprom(170, 180, 128);
}

// Whole-board state indicators, runtime-configurable via user_config.indicators.
// Priority: Caps Lock > Caps Word > WIN_FN layer; each gated on its enabled flag.
bool rgb_matrix_indicators_advanced_user(uint8_t led_min, uint8_t led_max) {
    const indicator_t *ind = NULL;
    if (host_keyboard_led_state().caps_lock && user_config.indicators[IND_CAPS_LOCK].enabled) {
        ind = &user_config.indicators[IND_CAPS_LOCK];
    } else if (is_caps_word_on() && user_config.indicators[IND_CAPS_WORD].enabled) {
        ind = &user_config.indicators[IND_CAPS_WORD];
    } else if (get_highest_layer(layer_state | default_layer_state) == WIN_FN
               && user_config.indicators[IND_WIN_FN].enabled) {
        ind = &user_config.indicators[IND_WIN_FN];
    }

    if (ind) {
        for (uint8_t i = led_min; i < led_max; i++) {
            if (g_led_config.flags[i] & LED_FLAG_KEYLIGHT) {
                rgb_matrix_set_color(i, ind->r, ind->g, ind->b);
            }
        }
    }
    return false;
}
