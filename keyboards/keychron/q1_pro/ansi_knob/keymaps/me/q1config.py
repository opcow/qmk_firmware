#!/usr/bin/env python3
"""Configure Q1 Pro tap dance / tapping term over raw HID.

Requires: pip install hidapi

Commands:
    get                          show global config (tapping term, enabled, mode)
    tt <ms>                      set global tapping term
    list                         show all tap dance slots
    show <idx>                   show one tap dance slot
    en <idx> <0|1>              enable/disable a slot's secondary action
    mode <idx> <double|hold>     set a slot's secondary trigger
    kc <idx> <tap> <secondary>   set a slot's keycodes (names, e.g. SCLN COLN)
    reset                        restore defaults

  Assign physical keys to slots (runtime remap, persists in EEPROM):
    assign <layer> <row> <col> <slot>   make a key trigger tap-dance <slot>
    unassign <layer> <row> <col> <kc>   restore a key to a normal keycode
    keyget <layer> <row> <col>          show the keycode at a position
    keyset <layer> <row> <col> <kc>     set any keycode at a position
    id                                  press a key; prints its row/col
    dump <layer>                        print a layer's keycode grid
    kmreset                             reset WHOLE keymap to compiled defaults

  Feature config:
    features                            show feature flags + timeouts
    capsword <0|1>                      enable/disable Caps Word
    cwtimeout <ms>                      Caps Word idle timeout (0 = never)
    permissive <0|1>                    permissive hold
    hokp <0|1>                          hold on other key press
    retro <0|1>                         retro tapping
    quicktap <ms>                       quick tap term
    autoshift <0|1>                     Auto Shift on/off
    astimeout <ms>                      Auto Shift timeout

  Presets (JSON files in ./presets/, shared with the WebHID GUI):
    presets                             list available presets
    save <name>                         snapshot current config -> presets/<name>.json
    load <name>                         apply a preset (writes only what differs)
    mine                                alias for `load mine`

  RGB state indicators (capslock / capsword / winfn):
    indicators                          show indicator on/off + colors
    indicator <name> <on|off> [color]   set; color = "R G B" or "#rrggbb"

    mine                                apply the author's personal setup

Keycodes may be names (A, F12, SCLN, COLN, HOME, ENT, CW_TOGG, LOCK, AS_TOGG...),
shifted form S(SCLN), a tap-dance slot TD3 / TD(3), or raw numbers (0x0233 / 563).
Layers: 0 MAC_BASE  1 MAC_FN  2 WIN_BASE  3 WIN_FN. Matrix is 6 rows x 16 cols.
Tap dance slot indices:
    0 NO_CAPS  1 HOME_END  2 ESC_CW  3 SCLN_CLN
    4 F_PSCR   5 F_SCRL    6 F_PAUS  7 F_NUM   (8..31 generic)
"""
import sys
import json
from pathlib import Path
import hid

TD_SLOT_COUNT = 32
FLAG_KEYS = ["caps_word", "permissive_hold", "hold_on_other_key", "retro_tapping", "auto_shift"]
PRESET_DIR = Path(__file__).resolve().parent / "presets"

VID, PID = 0x3434, 0x0610
USAGE_PAGE, USAGE = 0xFF60, 0x61
CMD = 0xAC
GET_GLOBAL, SET_TT, SET_TD_EN, SET_TD_MODE, RESET, GET_TD, SET_TD_KC, IDENTIFY = \
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08
GET_FEATURES, SET_FLAG, SET_PARAM = 0x09, 0x0A, 0x0B
GET_INDICATOR, SET_INDICATOR = 0x0C, 0x0D
# feature_flags bit positions (must match keymap.c FF_* )
FF_CAPS_WORD, FF_PERMISSIVE, FF_HOKP, FF_RETRO, FF_AUTOSHIFT = 0, 1, 2, 3, 4
# SET_PARAM ids
PARAM_QUICKTAP, PARAM_ASTIMEOUT, PARAM_CWTIMEOUT = 0, 1, 2
# indicator indices (must match keymap.c IND_* )
IND_NAMES = ["capslock", "capsword", "winfn"]
# Standard VIA dynamic-keymap commands (handled by via.c, big-endian keycode)
VIA_GET_KEYCODE, VIA_SET_KEYCODE, VIA_KEYMAP_RESET = 0x04, 0x05, 0x06
QK_TAP_DANCE = 0x5700
ROWS, COLS, LAYERS = 6, 16, 4

TD_NAMES = ["NO_CAPS", "HOME_END", "ESC_CW", "SCLN_CLN",
            "F_PSCR", "F_SCRL", "F_PAUS", "F_NUM"]
TD_NAMES += [f"TD{i}" for i in range(len(TD_NAMES), 32)]  # generic slots 8..31

# --- keycode name -> value table (QMK basic keycodes / HID usage IDs) ---
NAMES = {"NO": 0x00, "TRNS": 0x01}
for i, c in enumerate("ABCDEFGHIJKLMNOPQRSTUVWXYZ"):
    NAMES[c] = 0x04 + i
for i, c in enumerate("123456789"):
    NAMES[c] = 0x1E + i
NAMES["0"] = 0x27
NAMES.update({
    "ENT": 0x28, "ESC": 0x29, "BSPC": 0x2A, "TAB": 0x2B, "SPC": 0x2C,
    "MINS": 0x2D, "EQL": 0x2E, "LBRC": 0x2F, "RBRC": 0x30, "BSLS": 0x31,
    "SCLN": 0x33, "QUOT": 0x34, "GRV": 0x35, "COMM": 0x36, "DOT": 0x37,
    "SLSH": 0x38, "CAPS": 0x39,
    "PSCR": 0x46, "SCRL": 0x47, "PAUS": 0x48, "INS": 0x49, "HOME": 0x4A,
    "PGUP": 0x4B, "DEL": 0x4C, "END": 0x4D, "PGDN": 0x4E,
    "RGHT": 0x4F, "LEFT": 0x50, "DOWN": 0x51, "UP": 0x52, "NUM": 0x53,
})
for i in range(1, 13):       # F1..F12
    NAMES[f"F{i}"] = 0x3A + (i - 1)
for i in range(13, 25):      # F13..F24
    NAMES[f"F{i}"] = 0x68 + (i - 13)
# common shifted symbols (S(x) = LSFT | x, modifier bit 0x0200)
NAMES.update({
    "COLN": 0x0233, "EXLM": 0x021E, "AT": 0x021F, "HASH": 0x0220,
    "DLR": 0x0221, "PERC": 0x0222, "CIRC": 0x0223, "AMPR": 0x0224,
    "ASTR": 0x0225, "LPRN": 0x0226, "RPRN": 0x0227, "UNDS": 0x022D,
    "PLUS": 0x022E, "LCBR": 0x022F, "RCBR": 0x0230, "PIPE": 0x0231,
    "DQUO": 0x0234, "TILD": 0x0235, "LT": 0x0236, "GT": 0x0237, "QUES": 0x0238,
})
# QMK feature keycodes (assignable to keys via assign/keyset)
NAMES.update({
    "CW_TOGG": 0x7C73,                       # Caps Word toggle
    "QK_LOCK": 0x7C59, "LOCK": 0x7C59,       # Key Lock
    "AS_DOWN": 0x7C10, "AS_UP": 0x7C11, "AS_RPT": 0x7C12,
    "AS_ON": 0x7C13, "AS_OFF": 0x7C14, "AS_TOGG": 0x7C15,
})
CODE_TO_NAME = {v: k for k, v in NAMES.items()}


def kc(token):
    t = token.upper()
    if t.startswith("KC_"):
        t = t[3:]
    if t.startswith("S(") and t.endswith(")"):
        return 0x0200 | kc(t[2:-1])
    if t.startswith("TD(") and t.endswith(")"):
        return QK_TAP_DANCE | int(t[3:-1])
    if t.startswith("TD") and t[2:].isdigit():
        return QK_TAP_DANCE | int(t[2:])
    if t.startswith("0X"):
        return int(t, 16)
    if t in NAMES:
        return NAMES[t]
    if token.isdigit():
        return int(token)
    sys.exit(f"Unknown keycode: {token}")


def name_of(code):
    if QK_TAP_DANCE <= code <= QK_TAP_DANCE | 0xFF:
        return f"TD{code & 0xFF}"
    return CODE_TO_NAME.get(code, f"0x{code:04X}")


def open_dev():
    for d in hid.enumerate(VID, PID):
        if d["usage_page"] == USAGE_PAGE and d["usage"] == USAGE:
            h = hid.device()
            h.open_path(d["path"])
            return h
    sys.exit("Keyboard raw HID interface not found (plugged in via USB?)")


def send_raw(h, payload):
    buf = bytes(payload) + bytes(32 - len(payload))
    h.write(b"\x00" + buf)  # leading 0 = report ID
    r = h.read(32, timeout_ms=1000)
    if not r:
        sys.exit("No response from keyboard")
    return r


def send(h, payload):
    # 0xAC commands: byte[1] of the reply is a status code (0 = OK).
    r = send_raw(h, payload)
    if r[1] != 0x00:
        sys.exit("Keyboard returned ERROR (bad index or value?)")
    return r


def via_get_keycode(h, layer, row, col):
    r = send_raw(h, [VIA_GET_KEYCODE, layer, row, col])
    return (r[4] << 8) | r[5]


def via_set_keycode(h, layer, row, col, code):
    if not (0 <= layer < LAYERS and 0 <= row < ROWS and 0 <= col < COLS):
        sys.exit(f"position out of range (layer<{LAYERS} row<{ROWS} col<{COLS})")
    send_raw(h, [VIA_SET_KEYCODE, layer, row, col, (code >> 8) & 0xFF, code & 0xFF])


def show_slot(h, i):
    r = send(h, [CMD, GET_TD, i])
    tap = r[3] | (r[4] << 8)
    sec = r[5] | (r[6] << 8)
    en = "on " if r[7] else "off"
    mode = "hold  " if r[8] else "double"
    print(f"  [{i}] {TD_NAMES[i]:<9} tap={name_of(tap):<6} "
          f"secondary={name_of(sec):<6} mode={mode} enabled={en}")


def set_flag(h, bit, val):
    send(h, [CMD, SET_FLAG, bit, 1 if int(val) else 0])


def set_param(h, pid, val):
    val = int(val)
    send(h, [CMD, SET_PARAM, pid, val & 0xFF, (val >> 8) & 0xFF])


def show_features(h):
    r = send(h, [CMD, GET_FEATURES])
    flags = r[2] | (r[3] << 8)
    quicktap = r[4] | (r[5] << 8)
    astimeout = r[6] | (r[7] << 8)
    cwtimeout = r[8] | (r[9] << 8)
    def on(b):
        return "on" if flags & (1 << b) else "off"
    print(f"caps_word={on(FF_CAPS_WORD)}  cw_timeout={cwtimeout}")
    print(f"permissive_hold={on(FF_PERMISSIVE)}  hold_on_other_key={on(FF_HOKP)}  "
          f"retro={on(FF_RETRO)}  quick_tap={quicktap}")
    print(f"auto_shift={on(FF_AUTOSHIFT)}  as_timeout={astimeout}")


def ind_index(name):
    if name.isdigit():
        return int(name)
    if name.lower() in IND_NAMES:
        return IND_NAMES.index(name.lower())
    sys.exit(f"Unknown indicator: {name} (use {', '.join(IND_NAMES)})")


def parse_color(tokens):
    """Accept 'R G B' (three ints) or a single '#rrggbb'. Returns (r, g, b)."""
    if len(tokens) == 1 and tokens[0].startswith("#"):
        v = int(tokens[0][1:], 16)
        return (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF
    if len(tokens) == 3:
        return tuple(int(t) & 0xFF for t in tokens)
    sys.exit("color must be 'R G B' (0-255 each) or '#rrggbb'")


def show_indicator(h, i):
    r = send(h, [CMD, GET_INDICATOR, i])
    en = "on " if r[3] else "off"
    print(f"  [{i}] {IND_NAMES[i]:<9} {en}  color=({r[4]},{r[5]},{r[6]})  #{r[4]:02X}{r[5]:02X}{r[6]:02X}")


def set_indicator(h, i, enabled, rgb):
    send(h, [CMD, SET_INDICATOR, i, 1 if enabled else 0, rgb[0], rgb[1], rgb[2]])


# ----- Presets (filesystem, shared JSON schema with the WebHID GUI) -----
def read_config(h):
    """Snapshot the device's full 0xAC config into a preset dict (keycode names)."""
    g = send(h, [CMD, GET_GLOBAL])
    f = send(h, [CMD, GET_FEATURES])
    flags = f[2] | (f[3] << 8)
    preset = {
        "schema": 1,
        "tapping_term": g[2] | (g[3] << 8),
        "quick_tap_term": f[4] | (f[5] << 8),
        "autoshift_timeout": f[6] | (f[7] << 8),
        "caps_word_timeout": f[8] | (f[9] << 8),
        "flags": {key: bool(flags & (1 << bit)) for bit, key in enumerate(FLAG_KEYS)},
        "tap_dance": [],
        "indicators": [],
    }
    for i in range(TD_SLOT_COUNT):
        r = send(h, [CMD, GET_TD, i])
        preset["tap_dance"].append({
            "tap": name_of(r[3] | (r[4] << 8)),
            "secondary": name_of(r[5] | (r[6] << 8)),
            "mode": "hold" if r[8] else "double",
            "enabled": bool(r[7]),
        })
    for i in range(len(IND_NAMES)):
        r = send(h, [CMD, GET_INDICATOR, i])
        preset["indicators"].append({
            "enabled": bool(r[3]),
            "color": f"#{r[4]:02X}{r[5]:02X}{r[6]:02X}",
        })
    return preset


def write_config(h, preset):
    """Apply a preset, writing only values that differ from the device (saves EEPROM)."""
    cur = read_config(h)
    n = 0

    def chg(label, old, new):
        nonlocal n
        if old != new:
            n += 1
        return old != new

    if chg("tt", cur["tapping_term"], preset["tapping_term"]):
        send(h, [CMD, SET_TT, preset["tapping_term"] & 0xFF, (preset["tapping_term"] >> 8) & 0xFF])
    for pid, key in ((PARAM_QUICKTAP, "quick_tap_term"), (PARAM_ASTIMEOUT, "autoshift_timeout"),
                     (PARAM_CWTIMEOUT, "caps_word_timeout")):
        if chg(key, cur[key], preset[key]):
            set_param(h, pid, preset[key])
    for bit, key in enumerate(FLAG_KEYS):
        if chg(key, cur["flags"][key], preset["flags"].get(key, False)):
            set_flag(h, bit, preset["flags"].get(key, False))
    for i, td in enumerate(preset["tap_dance"][:TD_SLOT_COUNT]):
        c = cur["tap_dance"][i]
        if c["tap"] != td["tap"] or c["secondary"] != td["secondary"]:
            n += 1
            tap, sec = kc(td["tap"]), kc(td["secondary"])
            send(h, [CMD, SET_TD_KC, i, tap & 0xFF, tap >> 8, sec & 0xFF, sec >> 8])
        if chg("mode", c["mode"], td["mode"]):
            send(h, [CMD, SET_TD_MODE, i, 1 if td["mode"] == "hold" else 0])
        if chg("en", c["enabled"], td["enabled"]):
            send(h, [CMD, SET_TD_EN, i, 1 if td["enabled"] else 0])
    for i, ind in enumerate(preset["indicators"][:len(IND_NAMES)]):
        c = cur["indicators"][i]
        if c["enabled"] != ind["enabled"] or c["color"].upper() != ind["color"].upper():
            n += 1
            r, g, b = parse_color([ind["color"]])
            set_indicator(h, i, ind["enabled"], (r, g, b))
    return n


def preset_path(name):
    return PRESET_DIR / (name + ".json")


def save_preset(h, name):
    PRESET_DIR.mkdir(exist_ok=True)
    preset = read_config(h)
    preset["name"] = name
    preset_path(name).write_text(json.dumps(preset, indent=2))
    print(f"Saved {preset_path(name)}")


def load_preset(h, name):
    p = preset_path(name)
    if not p.exists():
        sys.exit(f"No preset '{name}' at {p}. Available: {', '.join(list_presets()) or '(none)'}")
    preset = json.loads(p.read_text())
    n = write_config(h, preset)
    print(f"Loaded '{name}' ({n} change(s) written).")
    show_features(h)


def list_presets():
    if not PRESET_DIR.exists():
        return []
    return sorted(p.stem for p in PRESET_DIR.glob("*.json"))


def main():
    h = open_dev()
    a = sys.argv[1:]
    op = a[0] if a else "get"

    if op == "get":
        r = send(h, [CMD, GET_GLOBAL])
        count = r[4]
        enabled = int.from_bytes(bytes(r[5:9]), "little")
        mode = int.from_bytes(bytes(r[9:13]), "little")
        print(f"tapping_term={r[2] | (r[3] << 8)}  slots={count}")
        print(f"td_enabled=0x{enabled:08X}  td_mode=0x{mode:08X}")
    elif op == "tt":
        r = send(h, [CMD, SET_TT, int(a[1]) & 0xFF, (int(a[1]) >> 8) & 0xFF])
        print(f"tapping_term={r[2] | (r[3] << 8)}")
    elif op == "list":
        r = send(h, [CMD, GET_GLOBAL])
        for i in range(r[4]):
            show_slot(h, i)
    elif op == "show":
        show_slot(h, int(a[1]))
    elif op == "en":
        send(h, [CMD, SET_TD_EN, int(a[1]), int(a[2])])
        show_slot(h, int(a[1]))
    elif op == "mode":
        m = 1 if a[2].lower() in ("hold", "1") else 0
        send(h, [CMD, SET_TD_MODE, int(a[1]), m])
        show_slot(h, int(a[1]))
    elif op == "kc":
        i, tap, sec = int(a[1]), kc(a[2]), kc(a[3])
        send(h, [CMD, SET_TD_KC, i,
                 tap & 0xFF, (tap >> 8) & 0xFF, sec & 0xFF, (sec >> 8) & 0xFF])
        show_slot(h, i)
    elif op == "reset":
        send(h, [CMD, RESET])
        print("Defaults restored.")
    elif op == "keyget":
        l, rw, c = int(a[1]), int(a[2]), int(a[3])
        code = via_get_keycode(h, l, rw, c)
        print(f"L{l} R{rw} C{c} = {name_of(code)} (0x{code:04X})")
    elif op == "keyset":
        l, rw, c, code = int(a[1]), int(a[2]), int(a[3]), kc(a[4])
        via_set_keycode(h, l, rw, c, code)
        print(f"L{l} R{rw} C{c} -> {name_of(code)}")
    elif op == "assign":
        l, rw, c, slot = int(a[1]), int(a[2]), int(a[3]), int(a[4])
        via_set_keycode(h, l, rw, c, QK_TAP_DANCE | slot)
        print(f"L{l} R{rw} C{c} -> TD{slot} ({TD_NAMES[slot]})")
    elif op == "unassign":
        l, rw, c, code = int(a[1]), int(a[2]), int(a[3]), kc(a[4])
        via_set_keycode(h, l, rw, c, code)
        print(f"L{l} R{rw} C{c} -> {name_of(code)}")
    elif op == "dump":
        l = int(a[1])
        print(f"Layer {l}:      " + "".join(f"C{c:<6}" for c in range(COLS)))
        for rw in range(ROWS):
            cells = "".join(f"{name_of(via_get_keycode(h, l, rw, c)):<7}"
                            for c in range(COLS))
            print(f"  R{rw}        {cells}")
    elif op == "id":
        send(h, [CMD, IDENTIFY])  # ack
        print("Press the key you want to identify...")
        r = h.read(32, timeout_ms=15000)
        if not r or r[0] != CMD or r[1] != IDENTIFY:
            sys.exit("No keypress captured (timed out).")
        code = r[4] | (r[5] << 8)
        print(f"row={r[2]} col={r[3]}  current keycode={name_of(code)} (0x{code:04X})")
    elif op == "kmreset":
        send_raw(h, [VIA_KEYMAP_RESET])
        print("Whole keymap reset to compiled defaults.")
    elif op == "features":
        show_features(h)
    elif op == "capsword":
        set_flag(h, FF_CAPS_WORD, a[1]); show_features(h)
    elif op == "permissive":
        set_flag(h, FF_PERMISSIVE, a[1]); show_features(h)
    elif op == "hokp":
        set_flag(h, FF_HOKP, a[1]); show_features(h)
    elif op == "retro":
        set_flag(h, FF_RETRO, a[1]); show_features(h)
    elif op == "autoshift":
        set_flag(h, FF_AUTOSHIFT, a[1]); show_features(h)
    elif op == "cwtimeout":
        set_param(h, PARAM_CWTIMEOUT, a[1]); show_features(h)
    elif op == "quicktap":
        set_param(h, PARAM_QUICKTAP, a[1]); show_features(h)
    elif op == "astimeout":
        set_param(h, PARAM_ASTIMEOUT, a[1]); show_features(h)
    elif op == "indicators":
        for i in range(len(IND_NAMES)):
            show_indicator(h, i)
    elif op == "indicator":
        i = ind_index(a[1])
        enabled = a[2].lower() in ("on", "1")
        if len(a) > 3:
            rgb = parse_color(a[3:])
        else:
            r = send(h, [CMD, GET_INDICATOR, i])  # keep stored color
            rgb = (r[4], r[5], r[6])
        set_indicator(h, i, enabled, rgb)
        show_indicator(h, i)
    elif op == "presets":
        names = list_presets()
        print("Presets in", PRESET_DIR)
        print("  " + ("  ".join(names) if names else "(none)"))
    elif op == "save":
        save_preset(h, a[1])
    elif op == "load":
        load_preset(h, a[1])
    elif op == "mine":
        load_preset(h, "mine")  # convenience alias for `load mine`
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
