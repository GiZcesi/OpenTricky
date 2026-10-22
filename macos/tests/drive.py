#!/usr/bin/env python3
"""Drive the game through its diag console (XBOX_DIAG_PORT): start it, run a script of steps, stop it, and report crashes.

    drive.py [--iso PATH] [--bin PATH] [--out DIR] [--env K=V ...] "wait 14; shot a; press START 200; wait 2; shot b"

Steps (separated by ;):  wait <seconds> | press <BUTTON> [ms] | shot <name> | hold <BUTTON> <seconds> | seq <BUTTON>@<every s>x<count> | burst <name> <count> <interval s> | waitlog <text_with_underscores> <timeout s> | until <text> <BUTTON> <every s> <timeout s>
Screenshots land in <out>/<name>.png. Exit status 1 if the game died (crash.txt in the run directory) before the script finished.
"""
import os, socket, subprocess, sys, time, argparse, signal

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("script")
    ap.add_argument("--iso", default=os.path.expanduser("~/Library/Application Support/OpenTricky/disc-image.txt"))
    ap.add_argument("--bin", default=None)
    ap.add_argument("--out", default="/tmp/ot_drive")
    ap.add_argument("--port", type=int, default=47011)
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--home", default=None, help="run directory (default: a fresh temp dir)")
    a = ap.parse_args()
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
    binp = a.bin or os.path.join(root, "build", "gpu", "SSX Tricky")
    iso = a.iso
    if os.path.isfile(iso) and iso.endswith(".txt"):
        iso = open(iso).read().strip()
    os.makedirs(a.out, exist_ok=True)
    run = a.home or os.path.join(a.out, "run")
    os.makedirs(run, exist_ok=True)
    env = dict(os.environ, XBOX_DIAG_PORT=str(a.port))
    for kv in a.env:
        k, v = kv.split("=", 1); env[k] = v
    log = open(os.path.join(a.out, "game.log"), "wb")
    p = subprocess.Popen([binp, "--direct", iso], cwd=run, env=env, stdout=log, stderr=log)
    sock = None
    for _ in range(100):
        time.sleep(0.3)
        if p.poll() is not None: break
        try: sock = socket.create_connection(("127.0.0.1", a.port), timeout=3); break
        except OSError: pass
    def alive(): return p.poll() is None
    def cmd(c):
        sock.sendall((c + "\n").encode()); time.sleep(0.15)
        try: return sock.recv(200).decode().strip()
        except OSError: return "ERR"
    ok = sock is not None
    try:
        for step in [s.strip() for s in a.script.split(";") if s.strip()]:
            if not alive() or not ok: ok = False; break
            w = step.split()
            if w[0] == "wait":
                end = time.time() + float(w[1])
                while time.time() < end and alive(): time.sleep(0.1)
            elif w[0] == "press": print(step, "->", cmd(step))
            elif w[0] == "hold": print(step, "->", cmd("press %s %d" % (w[1], int(float(w[2]) * 1000)))); time.sleep(float(w[2]))
            elif w[0] == "shot":
                path = os.path.join(a.out, w[1] + ".png")
                if os.path.exists(path): os.remove(path)
                print(step, "->", cmd("shot " + path)); time.sleep(1.2)
            elif w[0] == "burst":            # burst <name> <count> <interval s>: count screenshots <name>_<i>.png, interval apart (wall-clock stamps in <out>/<name>.times)
                n, iv = int(w[2]), float(w[3]); t0 = time.time(); times = []
                for k in range(n):
                    if not alive(): break
                    cmd("shot " + os.path.join(a.out, "%s_%02d.png" % (w[1], k))); times.append("%02d %.2f" % (k, time.time() - t0))
                    time.sleep(max(0.0, iv - 0.15))
                open(os.path.join(a.out, w[1] + ".times"), "w").write("\n".join(times))
            elif w[0] in ("waitlog", "until"):       # waitlog <text> <timeout s> | until <text> <BUTTON> <every s> <timeout s>: pressing BUTTON until the log has <text>
                text = w[1].replace("_", " ").encode(); gl = os.path.join(a.out, "game.log")
                if w[0] == "waitlog": btn, every, tmo = None, 0.5, float(w[2])
                else: btn, every, tmo = w[2], float(w[3]), float(w[4])
                end = time.time() + tmo; seen = False
                while time.time() < end and alive():
                    if text in open(gl, "rb").read(): seen = True; break
                    if btn: cmd("press %s 200" % btn)
                    time.sleep(every)
                print(step, "->", "seen" if seen else "TIMEOUT")
            elif w[0] == "seq":
                b, rest = w[1].split("@"); every, count = rest.split("x")
                for _ in range(int(count)):
                    if not alive(): break
                    cmd("press %s 150" % b); time.sleep(float(every))
            else: print("unknown step", step)
    finally:
        died = not alive()
        if alive(): p.send_signal(signal.SIGTERM); time.sleep(0.5)
        if p.poll() is None: p.kill()
    crash = os.path.join(run, "crash.txt")
    if died or os.path.exists(crash):
        print("GAME DIED (exit %s)" % p.returncode)
        if os.path.exists(crash): print(open(crash).read()[:1500])
        sys.exit(1)
    print("game ran to the end of the script")

main()
