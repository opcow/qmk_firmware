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

Keycodes may be names (A, F12, SCLN, COLN, HOME, ENT, ...), shifted form
S(SCLN), a tap-dance slot TD3 / TD(3), or raw numbers (0x0233 / 563).
Layers: 0 MAC_BASE  1 MAC_FN  2 WIN_BASE  3 WIN_FN. Matrix is 6 rows x 16 cols.
Tap dance slot indices:
    0 NO_CAPS  1 HOME_END  2 ESC_CW  3 SCLN_CLN
    4 F_PSCR   5 F_SCRL    6 F_PAUS  7 F_NUM   (8..31 generic)
"""
import sys
import hid

VID, PID = 0x3434, 0x0610
USAGE_PAGE, USAGE = 0xFF60, 0x61
CMD = 0xAC
GET_GLOBAL, SET_TT, SET_TD_EN, SET_TD_MODE, RESET, GET_TD, SET_TD_KC, IDENTIFY = \
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08
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
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
