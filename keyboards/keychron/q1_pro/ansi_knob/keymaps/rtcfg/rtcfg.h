#pragma once

#include <stdint.h>

// Accessors into the runtime user_config (defined in keymap.c), exposed so the
// runtime debounce dispatcher (debounce_rt.c) can read the current settings
// without depending on the user_config_t layout.
uint8_t rtcfg_debounce_time(void);    // ms; 0 = no debounce
uint8_t rtcfg_debounce_method(void);  // 0 none, 1 sym_defer_g, 2 sym_eager_pk, 3 asym_eager_defer_pk
