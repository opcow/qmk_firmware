#pragma once

// Reserve an EEPROM user data block for runtime tap dance / tapping term config.
// Must match sizeof(user_config_t) in keymap.c. Bump the version whenever the
// struct layout changes so old data is discarded and defaults reapplied.
#define EECONFIG_USER_DATA_SIZE 144
#define EECONFIG_USER_DATA_VERSION 0x00514402
