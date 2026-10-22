#!/usr/bin/env python3
"""Checks the pacing options against a real run (needs the game, a disc image and a display): the loading screen is held to OT_MIN_LOAD_MS and the
start countdown (race state 3) lasts about 2.5 s times OT_COUNTDOWN_SLOW. Exits 1 with the numbers if either is off.

    python3 macos/tests/pacing_check.py            # slow 1.5, min load 3000, and the original (1, 0) as a control
"""
import os, re, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = "wait 12; press START 200; wait 2; until level_load A 4 140; until 3_->_4 A 3 100; wait 1"

def run(slow, minload, port):
    out = tempfile.mkdtemp()
    r = subprocess.run([sys.executable, os.path.join(HERE, "drive.py"), "--out", out, "--port", str(port), "--env", "OT_COUNTDOWN_SLOW=%s" % slow,
                        "--env", "OT_MIN_LOAD_MS=%s" % minload, SCRIPT], capture_output=True, text=True, timeout=400)
    log = open(os.path.join(out, "game.log"), errors="replace").read()
    m = re.search(r"race state 3 -> 4 after (\d+) ms", log)
    l = re.search(r"level load took (\d+) ms, screen held for (\d+) ms in all \(minimum (\d+)\)", log)
    return (int(m.group(1)) if m else None), (int(l.group(2)) if l else None), r.returncode

bad = 0
# the control uses 1.001: a factor of exactly 1 installs no clock hook, and the hook is what logs the race states the countdown is measured from
for slow, minload, port in (("1.5", "3000", 47301), ("1.001", "0", 47302)):
    cd, held, rc = run(slow, minload, port)
    want_cd = 2500 * float(slow)       # 1.001 is within the tolerance of 1
    ok_cd = cd is not None and 0.9 * want_cd <= cd <= 1.1 * want_cd
    ok_hold = (held is None) if int(minload) == 0 else (held is not None and held >= int(minload) - 1)     # no hook (no log line) when the hold is off
    print("slow=%s min_load=%s: countdown %s ms (want %.0f +-10%%) %s; load screen %s ms (want >= %s, none when 0) %s" %
          (slow, minload, cd, want_cd, "ok" if ok_cd else "FAIL", held, minload, "ok" if ok_hold else "FAIL"))
    bad += (not ok_cd) + (not ok_hold)
sys.exit(1 if bad else 0)
