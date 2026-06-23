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

#pragma once

#include "quantum.h"

// The core does not assume any particular layer enum — each board keymap keeps
// its own. The only layer the core cares about is the one whose FN-layer RGB
// indicator it paints; that layer's index defaults to 3 (the WIN_FN slot on the
// standard Mac/Win Keychron keymaps) and can be overridden per board in config.h
// via RTCFG_FN_INDICATOR_LAYER.

// --- Board-independent rtcfg core (rtcfg_common.c) ---------------------------
// The board keymap is thin: it supplies keymaps[] / encoder_map[] and calls
// rtcfg_process_record() from its own process_record_user(). Everything else
// (EEPROM config, tap-dance slots, combos, key overrides, one-shot, Caps Word,
// Auto Shift, RGB indicators, and the 0xAC raw HID handler) lives in the core.

// Number of runtime combo slots. The board keymap must define the table QMK's
// combo introspection serves, sized to this, at file scope:
//
//     combo_t key_combos[RTCFG_COMBO_COUNT];
//
// It has to live in the keymap translation unit (keymap_introspection.c #includes
// the keymap and takes sizeof(key_combos)); the core fills it at runtime.
#define RTCFG_COMBO_COUNT 16

// Call from the board keymap's process_record_user(); returns false to consume
// the event (identify capture / quick-tap emit), true to let QMK keep processing.
bool rtcfg_process_record(uint16_t keycode, keyrecord_t *record);

// Live debounce settings, read by the runtime debounce dispatcher (debounce_rt.c)
// without coupling it to the user_config_t layout.
uint8_t rtcfg_debounce_time(void);    // ms; 0 = no debounce
uint8_t rtcfg_debounce_method(void);  // 0 none, 1 sym_defer_g, 2 sym_eager_pk, 3 asym_eager_defer_pk
