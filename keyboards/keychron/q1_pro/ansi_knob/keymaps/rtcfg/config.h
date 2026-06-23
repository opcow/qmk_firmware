#pragma once

// Reserve an EEPROM user data block for runtime tap dance / tapping term config.
// Must match sizeof(user_config_t) in keymap.c. Bump the version whenever the
// struct layout changes so old data is discarded and defaults reapplied.
//
// NOTE: growing this block shifts QMK's dynamic-keymap EEPROM base (it starts at
// EECONFIG_SIZE = core + this block), so the stored VIA keymap moves and must be
// reset once after flashing this version ("Reset keymap" in the host app).
#define EECONFIG_USER_DATA_SIZE 576
#define EECONFIG_USER_DATA_VERSION 0x00514411

// One-shot keys: disable QMK's built-in (compile-time) expiry; the keymap
// implements a runtime-configurable timeout instead (like CAPS_WORD_IDLE_TIMEOUT).
// Tap-toggle count stays compile-time (QMK has no runtime hook for it).
#define ONESHOT_TIMEOUT 0
#define ONESHOT_TAP_TOGGLE 2

// Per-key tap-hold callbacks so these can be toggled at runtime from user_config.
#define PERMISSIVE_HOLD_PER_KEY
#define HOLD_ON_OTHER_KEY_PRESS_PER_KEY
#define RETRO_TAPPING_PER_KEY
#define QUICK_TAP_TERM_PER_KEY

// Disable Caps Word's built-in (compile-time) idle timeout; we implement a
// runtime-configurable timeout in the keymap instead.
#define CAPS_WORD_IDLE_TIMEOUT 0

// Auto Shift compiled in but starts disabled; runtime opt-in via q1config.py.
#define AUTO_SHIFT_DISABLED_AT_STARTUP
