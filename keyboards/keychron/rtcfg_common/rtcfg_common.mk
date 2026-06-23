# Shared runtime-config (rtcfg) core for Keychron boards.
# Include from a keymap's rules.mk:  include keyboards/keychron/rtcfg_common/rtcfg_common.mk
# The board's rules.mk already adds $(TOP_DIR)/keyboards/keychron to VPATH.

RTCFG_COMMON_DIR = rtcfg_common
SRC += \
	$(RTCFG_COMMON_DIR)/rtcfg_common.c \
	$(RTCFG_COMMON_DIR)/debounce_rt.c

VPATH += $(TOP_DIR)/keyboards/keychron/$(RTCFG_COMMON_DIR)

# Runtime-selectable debounce dispatcher lives in debounce_rt.c.
DEBOUNCE_TYPE = custom

# Features the core depends on.
VIA_ENABLE = yes
TAP_DANCE_ENABLE = yes
CAPS_WORD_ENABLE = yes
COMBO_ENABLE = yes
KEY_OVERRIDE_ENABLE = yes
TAPPING_TERM_PER_KEY = yes
AUTO_SHIFT_ENABLE = yes
KEY_LOCK_ENABLE = yes
