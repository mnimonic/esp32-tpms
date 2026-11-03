# One-off helper used to split ESP-TPMS.ino into tabs (kept for reference).
# Arduino compiles the main .ino first and then the other .ino tabs in
# alphabetical order, so numbered tab names keep the original code order.
import io, os, sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
main = os.path.join(root, "ESP-TPMS.ino")
src = io.open(main, encoding="utf-8-sig").read()

# (section marker that starts the tab, tab file name)
TABS = [
    ("// ---------------- Decoders ----------------",                            "10_ble.ino"),
    ("// ---------------- Positions (stored in flash) ----------------",         "20_storage.ino"),
    ("// ---------------- Rapid pressure loss / battery ----------------",       "30_alerts.ino"),
    ("// ---------------- Button actions ----------------",                      "40_pairing.ino"),
    ("// ---------------- Settings menu (MIN/MAX per position) ----------------","50_menu.ino"),
    ("// ---------------- Reading handling ----------------",                    "60_readings.ino"),
    ("// ---------------- Display ----------------",                             "70_display.ino"),
    ("// ---------------- Backlight / screen sleep ----------------",            "80_screen.ino"),
    ("// ---------------- Watchdog / reset reason ----------------",             "90_system.ino"),
    ("// ---------------- Setup / loop ----------------",                        "99_setup_loop.ino"),
]

starts = []
for marker, name in TABS:
    assert src.count(marker) == 1, marker
    starts.append((src.index(marker), name))
starts.sort()

head = src[:starts[0][0]]
pieces = []
for k, (pos, name) in enumerate(starts):
    end = starts[k + 1][0] if k + 1 < len(starts) else len(src)
    pieces.append((name, src[pos:end]))

# Reassembled text must be identical to the original
assert head + "".join(t for _, t in pieces) == src

note = ("// Code is split into tabs (compiled in this order after this file):\n"
        + "".join("//   %-18s %s\n" % (n, m.strip("/ -")) for m, n in TABS)
        + "\n")
io.open(main, "w", encoding="utf-8", newline="\n").write(head.rstrip("\n") + "\n\n" + note)
for name, text in pieces:
    io.open(os.path.join(root, name), "w", encoding="utf-8", newline="\n").write(text)
print("split into", len(pieces), "tabs; main file", len(head.splitlines()), "lines")
