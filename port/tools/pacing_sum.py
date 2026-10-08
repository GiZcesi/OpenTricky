#!/usr/bin/env python3
"""pacing_sum.py -- frame pacing smoothness.

    python port/tools/pacing_sum.py run.csv [--from S] [--to S] [--hz 60]

Reads an XBOX_PACING_CSV CSV (one line per Present) or an XBOX_PERF CSV
(P lines: t, itv), either one. Window in seconds from the first line
(--from / --to). Output: frames/s, mean interval, standard deviation, p50 /
p95 / p99 / max, share of intervals > 1.5x the median; with --hz, the share
of intervals that are not a whole multiple of the display period (within
10 %); with a pacing CSV in flip model, the number of refreshes between two
new frames on screen (DXGI statistics).
"""
import argparse, statistics, sys


def load(path):
    rows = []
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith("#") or len(line) < 3:
            continue
        f = line.strip().split(",")
        try:
            if f[0] == "P":                       # XBOX_PERF: P,t,itv,...
                rows.append({"t": float(f[1]), "itv": float(f[2])})
            elif f[0][0].isdigit() or f[0][0] == "-":
                r = {"t": float(f[0]), "itv": float(f[1]), "wait": float(f[2]), "pres": float(f[3])}
                if len(f) >= 9:
                    r["pc"], r["pr"] = int(f[6]), int(f[7])
                rows.append(r)
        except (ValueError, IndexError):
            pass
    return rows


def pct(s, p):
    return s[min(len(s) - 1, int(p * (len(s) - 1)))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--from", dest="t_from", type=float, default=0.0)
    ap.add_argument("--to", dest="t_to", type=float, default=1e12)
    ap.add_argument("--hz", type=float, default=0.0)
    ap.add_argument("--auto", action="store_true",
                    help="window = 5 s after the last interval > 250 ms (end of loading) up to 2 s before the end")
    a = ap.parse_args()
    rows = load(a.csv)
    if not rows:
        sys.exit("no line read: %s" % a.csv)
    t0 = rows[0]["t"]
    unit = 1000.0                          # time in ms in both formats
    if a.auto:
        gaps = [r["t"] for r in rows[1:] if r["itv"] > 250.0]
        a.t_from = ((gaps[-1] - t0) / unit + 5.0) if gaps else 5.0
        a.t_to = (rows[-1]["t"] - t0) / unit - 2.0
        print("auto window: %.1f s -> %.1f s" % (a.t_from, a.t_to))
    sel = [r for r in rows[1:] if a.t_from <= (r["t"] - t0) / unit <= a.t_to and r["itv"] > 0]
    if len(sel) < 10:
        sys.exit("too few frames in the window")
    itv = [r["itv"] for r in sel]
    s = sorted(itv)
    mean = statistics.fmean(itv)
    med = pct(s, 0.5)
    dur = (sel[-1]["t"] - sel[0]["t"]) / unit
    print("%s: %d frames over %.1f s, %.1f fps" % (a.csv, len(itv), dur, len(itv) / dur if dur else 0))
    print("  mean interval %.2f ms, std dev %.2f, p50 %.2f, p95 %.2f, p99 %.2f, max %.1f"
          % (mean, statistics.pstdev(itv), med, pct(s, 0.95), pct(s, 0.99), s[-1]))
    slow = sum(1 for x in itv if x > 1.5 * med)
    print("  > 1.5x median: %d (%.2f %%)" % (slow, 100.0 * slow / len(itv)))
    # change from one frame to the next: what the eye sees as stutter
    d = [abs(itv[i] - itv[i - 1]) for i in range(1, len(itv))]
    print("  |change between successive intervals| mean %.2f ms, p99 %.2f" % (statistics.fmean(d), pct(sorted(d), 0.99)))
    if "wait" in sel[0]:
        print("  mean latency wait %.2f ms, mean Present %.2f ms"
              % (statistics.fmean(r["wait"] for r in sel), statistics.fmean(r["pres"] for r in sel)))
    if a.hz:
        per = 1000.0 / a.hz
        off = sum(1 for x in itv if abs(x / per - round(x / per)) > 0.10 or round(x / per) == 0)
        print("  not a multiple of the period %.2f ms (within 10 %%): %d (%.1f %%)" % (per, off, 100.0 * off / len(itv)))
    if "pc" in sel[0]:
        hist, multi, pc0, pr0 = {}, 0, None, None
        for r in sel:
            if not r["pc"]:
                continue
            if pc0 is not None and r["pc"] != pc0:
                dp, dr = r["pc"] - pc0, r["pr"] - pr0
                if dp == 1:
                    k = min(dr, 4)
                    hist[k] = hist.get(k, 0) + 1
                else:
                    multi += 1
            if r["pc"] != pc0:
                pc0, pr0 = r["pc"], r["pr"]
        # summary per pair of readings: refreshes (dr) against new frames (dp)
        held = dropped = refr = 0
        pc0 = pr0 = None
        for r in sel:
            if not r["pc"]:
                continue
            if pc0 is not None and r["pc"] != pc0:
                dp, dr = r["pc"] - pc0, r["pr"] - pr0
                refr += dr
                held += max(0, dr - dp)
                dropped += max(0, dp - dr)
            if r["pc"] != pc0:
                pc0, pr0 = r["pc"], r["pr"]
        if refr:
            print("  display: %d refreshes; frame held one refresh longer: %d (%.2f %%);"
                  " frames never shown: %d" % (refr, held, 100.0 * held / refr, dropped))
        if hist or multi:
            n = sum(hist.values())
            print("  display: new frame after %s; several frames between two readings: %d"
                  % (", ".join("%s refr. %d (%.1f %%)" % ("4+" if k == 4 else k, v, 100.0 * v / n if n else 0)
                               for k, v in sorted(hist.items())), multi))


if __name__ == "__main__":
    main()
