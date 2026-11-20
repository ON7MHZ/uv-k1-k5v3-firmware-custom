#!/usr/bin/env python3
# DTMF MEMORY read-only assets: UI text and the multi-tap key groups.
import os
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
from app_assets import Assets

TITLE = "DTMF MEMORY"

UI = [
    ("T_TITLE", TITLE),
    ("T_SENDING", "SENDING..."),
    ("T_SENT", "SENT"),
    ("T_DENIED", "TX DENIED"),
    ("T_EMPTY", "EMPTY"),
    ("T_ABORT", "ABORTED"),
    ("T_HINT", "MENU/PTT SEND *EDIT F:"),
    ("T_EDIT", "EDIT SLOT "),
    ("T_NUM", "0-9 * F=#"),
    ("T_LET", "1-4=A-D"),
    ("T_NMODE", "2-9 LETTERS"),
    ("T_EHINT", "MENU ABCD DN DEL UP NAME EXIT"),
    ("T_NHINT", "TAP KEYS  DN DEL  UP CODE  EXIT"),
]

a = Assets("DTMFmemory")
ui_size = 0
for name, text in UI:
    pad = -(len(text) + 1) % 4
    a.text(name, text + "\0" * pad)
    ui_size += len(text) + 1 + pad
a.const("T_TITLE_CHARS", len(TITLE))
a.const("UI_SIZE", ui_size)

# Multi-tap groups for the name field: 5 bytes per key 0-9, padded with 0.
KEYS = [" 0", "1-.", "ABC2", "DEF3", "GHI4", "JKL5", "MNO6", "PQRS7", "TUV8", "WXYZ9"]
keymap = []
for group in KEYS:
    keymap += [ord(c) for c in group] + [0] * (5 - len(group))
a.u8("KEYMAP", keymap)

if __name__ == "__main__":
    a.main()
