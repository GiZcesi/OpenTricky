#!/usr/bin/env python3
"""pacing_sum.py -- frame pacing smoothness.

    python port/tools/pacing_sum.py run.csv [--from S] [--to S] [--auto] [--hz 60]
                                    [--target-ms MS] [--trace TRACE] [--json FILE]

Reads an XBOX_PACING_CSV CSV (one line per Present) or an XBOX_PERF CSV
(P lines: t, itv), either one. Window in seconds from the first line
(--from / --to), or --auto: 8 s after the last interval > 250 ms (end of the
last loading) up to the last line.

Output, on the times the presenting thread returned from Present (CPU side):
frames/s, mean interval, standard deviation, p50 / p95 / p99 / p99.9 / max,
"1 % / 0.1 % low" (frames/s over the slowest 1 % / 0.1 % of the intervals),
intervals > 2x the target (stutters) and < 0.5x the target (two frames too
close), change between successive intervals, distribution (0.5 ms bins),
repeating patterns (autocorrelation of the intervals; spacing of the
stutters), clock (frames/s per 10 s window against the target).
The target is --target-ms, else 1000 / --hz, else the median interval.

With a pacing CSV in flip model, the same on the times the frames reached the
screen (DXGI frame statistics: PresentRefreshCount / SyncRefreshCount /
SyncQPCTime): refreshes between two new frames, frames held one refresh
longer, frames never shown, measured refresh period and whether it varies
(variable refresh rate).

--trace: an XBOX_FPS_INTERP_TRACE file of the same run (race renders, game
thread: start time, tick, alpha). Each render is matched to its Present (the
index offset that makes the render -> Present delay the steadiest) and the
motion shown is compared with the time it is shown: content step (tick +
alpha, in ms) minus the step on screen (DXGI time, else the Present time).
That error is the judder the interpolation leaves; also the spread of the
render -> Present delay. The files have different time origins: only
differences are used.
"""
import argparse, json, math, statistics, sys

import numpy as np

TICK_MS = 1000.0 / 60.0


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
                    r["sync"], r["pc"], r["pr"], r["sr"] = int(f[4]), int(f[6]), int(f[7]), int(f[8])
                if len(f) >= 11:
                    r["sq"], r["hz"] = float(f[9]), int(f[10])
                if len(f) >= 12 and int(f[11]):
                    r["lpc"] = int(f[11])             # DXGI PresentCount of this very Present
                rows.append(r)
        except (ValueError, IndexError):
            pass
    for i, r in enumerate(rows):
        r["i"] = i                                # Present index since the chain was created
    return rows


def load_trace(path):
    """R lines of XBOX_FPS_INTERP_TRACE: t_start t_end present tick race_state extra mode alpha ..."""
    out = []
    for line in open(path, encoding="utf-8", errors="replace"):
        if not line.startswith("R "):
            continue
        f = line.split()
        try:
            out.append({"t0": float(f[1]), "t1": float(f[2]), "seq": int(f[3]), "tick": int(f[4]),
                        "st": int(f[5]), "extra": int(f[6]), "mode": int(f[7]), "alpha": float(f[8])})
        except (ValueError, IndexError):
            pass
    return out


def pct(s, p):
    return s[min(len(s) - 1, max(0, int(round(p * (len(s) - 1)))))]


def window(rows, t_from=0.0, t_to=1e12, auto=False):
    t0 = rows[0]["t"]
    if auto:
        gaps = [r["t"] for r in rows[1:] if r["itv"] > 250.0]
        t_from = ((gaps[-1] - t0) / 1000.0 + 8.0) if gaps else 5.0
        t_to = (rows[-1]["t"] - t0) / 1000.0
    sel = [r for r in rows[1:] if t_from <= (r["t"] - t0) / 1000.0 <= t_to and r["itv"] > 0]
    return sel, t_from, t_to


def interval_stats(itv, t, target=None):
    """itv: intervals (ms); t: their end times (ms)."""
    s = sorted(itv)
    n = len(itv)
    med = pct(s, 0.5)
    tgt = target if target else med
    dur = (t[-1] - t[0]) / 1000.0 if n > 1 else 0.0
    mins = max(dur / 60.0, 1e-9)

    def low(frac):
        k = max(1, int(math.ceil(frac * n)))
        return 1000.0 / statistics.fmean(s[-k:])

    d = [abs(itv[i] - itv[i - 1]) for i in range(1, n)]
    slow2 = sum(1 for x in itv if x > 2.0 * tgt)
    close = sum(1 for x in itv if x < 0.5 * tgt)
    hist = {}
    for x in itv:
        b = round(math.floor(x / 0.5) * 0.5, 1)
        hist[b] = hist.get(b, 0) + 1
    modes = sorted(hist.items(), key=lambda kv: -kv[1])[:6]
    return dict(n=n, secs=dur, fps=(n / dur if dur else 0.0), target=tgt, mean=statistics.fmean(itv),
                std=float(np.std(itv)), p50=med, p95=pct(s, 0.95), p99=pct(s, 0.99), p999=pct(s, 0.999),
                max=s[-1], low1=low(0.01), low01=low(0.001),
                slow2=slow2, slow2_min=slow2 / mins, close=close, close_pct=100.0 * close / n,
                gt15med=sum(1 for x in itv if x > 1.5 * med),
                dmean=statistics.fmean(d) if d else 0.0, dp99=pct(sorted(d), 0.99) if d else 0.0,
                modes=[[b, c, 100.0 * c / n] for b, c in sorted(modes)])


def patterns(itv, t, med, target, maxlag=300):
    """Repeating patterns: autocorrelation peaks of the intervals, spacing of the stutters."""
    n = len(itv)
    out = {"acf": [], "stutter_gap_p50": None, "stutter_gap_cv": None, "stutters": 0}
    if n >= 50:
        x = np.asarray(itv, dtype=float)
        x = x - x.mean()
        var = float(np.dot(x, x))
        if var > 0:
            acf = [(lag, float(np.dot(x[:-lag], x[lag:])) / var) for lag in range(1, min(maxlag, n // 4) + 1)]
            # local maxima above 0.15 (or the strongest negative lag-1: alternation)
            peaks = [(l, r) for k, (l, r) in enumerate(acf)
                     if abs(r) >= 0.15 and (k == 0 or abs(r) >= abs(acf[k - 1][1]))
                     and (k + 1 >= len(acf) or abs(r) >= abs(acf[k + 1][1]))]
            out["acf"] = [[l, round(r, 3)] for l, r in sorted(peaks, key=lambda p: -abs(p[1]))[:4]]
    thr = max(1.5 * med, 2.0 * target if target else 0.0) if target else 1.5 * med
    ev = [t[i] for i in range(n) if itv[i] > thr]
    out["stutters"] = len(ev)
    if len(ev) >= 3:
        g = [ev[i] - ev[i - 1] for i in range(1, len(ev))]
        gm = statistics.fmean(g)
        out["stutter_gap_p50"] = statistics.median(g)
        out["stutter_gap_cv"] = statistics.pstdev(g) / gm if gm else None
    return out


def clock(sel, target):
    """Frames/s per 10 s window; mean interval against the target."""
    w, out, t0, k = [], [], sel[0]["t"], 0
    for r in sel:
        if r["t"] - t0 >= 10000.0:
            out.append(k / ((r["t"] - t0) / 1000.0))
            t0, k = r["t"], 0
        k += 1
    mean = statistics.fmean(r["itv"] for r in sel)
    return dict(win_fps=[round(v, 2) for v in out],
                drift_pct=(100.0 * (mean - target) / target) if target else None)


def display(sel):
    """Times the frames reached the screen, from the DXGI frame statistics read after each Present.
    A reading gives (pc, pr): the last frame shown and the refresh it was shown at, and (sr, sq): a
    refresh count and its time. Shown time of frame pc = sq - (sr - pr) * period."""
    if not sel or "sq" not in sel[0]:
        return None
    rd = [r for r in sel if r.get("pc") and r.get("sq")]
    if len(rd) < 20:
        return None
    # refresh period: time between two readings over the refreshes between them (distinct sr)
    per = []
    prev = None
    for r in rd:
        if prev is not None and r["sr"] > prev["sr"]:
            per.append((r["sq"] - prev["sq"]) / (r["sr"] - prev["sr"]))
        if prev is None or r["sr"] != prev["sr"]:
            prev = r
    if len(per) < 10:
        return None
    sp = sorted(per)
    period = pct(sp, 0.5)
    shown = {}
    hist, multi, held, dropped, refr = {}, 0, 0, 0, 0
    pc0 = pr0 = None
    for r in rd:
        if r["pc"] not in shown:
            shown[r["pc"]] = r["sq"] - (r["sr"] - r["pr"]) * period
        if pc0 is not None and r["pc"] != pc0:
            dp, dr = r["pc"] - pc0, r["pr"] - pr0
            refr += dr
            held += max(0, dr - dp)
            dropped += max(0, dp - dr)
            if dp == 1:
                hist[min(dr, 4)] = hist.get(min(dr, 4), 0) + 1
            else:
                multi += 1
        if r["pc"] != pc0:
            pc0, pr0 = r["pc"], r["pr"]
    pcs = sorted(shown)
    itv, t = [], []
    for a, b in zip(pcs, pcs[1:]):
        if b == a + 1:
            itv.append(shown[b] - shown[a])
            t.append(shown[b])
    nh = sum(hist.values())
    return dict(period=period, period_p1=pct(sp, 0.01), period_p99=pct(sp, 0.99),
                vrr=(pct(sp, 0.99) - pct(sp, 0.01)) > 0.10 * period,
                coverage=100.0 * len(itv) / max(1, len(sel) - 1),
                refreshes=refr, held=held, held_pct=(100.0 * held / refr) if refr else 0.0, dropped=dropped,
                after=[[k, v, 100.0 * v / nh if nh else 0.0] for k, v in sorted(hist.items())], multi=multi,
                shown=shown, itv=itv, t=t)


def best_offset(a_idx, a_t, b_t, base, span):
    """Offset o (index of b = index of a + o) that makes b - a steadiest; with the runner-up."""
    res = []
    for o in range(base - span, base + span + 1):
        d = [b_t[i + o] - ta for i, ta in zip(a_idx, a_t) if 0 <= i + o < len(b_t)]
        if len(d) >= 0.9 * len(a_idx) and len(d) > 20:
            res.append((float(np.std(d)), o, d))
    res.sort(key=lambda x: x[0])
    return res[0] if res else None, (res[1][0] if len(res) > 1 else None)


def content(trace, rows, sel, disp):
    """Render (game thread) -> Present (pump) matching, and motion shown against time shown."""
    if not trace or not sel:
        return None
    # renders whose Present falls in the window: race renders in race state 4 only
    tr = [r for r in trace if r["st"] == 4]
    if len(tr) < 100:
        return None
    pt = [r["t"] for r in rows]
    base = int(statistics.median(r["seq"] - k for k, r in enumerate(tr)))
    # trace index k -> Present row index k + o; search around the Present count seen at the render's end
    best, second = best_offset(list(range(len(tr))), [r["t0"] for r in tr], pt, base, 6)
    if not best:
        return None
    sd, o, _ = best
    lo, hi = sel[0]["t"], sel[-1]["t"]
    keep = [k for k in range(len(tr)) if 0 <= k + o < len(rows) and lo <= pt[k + o] <= hi]
    if len(keep) < 50:
        return None
    # time shown of each matched frame: DXGI (frame id = row index + c, c found the same way), else Present
    src, c_off, shown_t = "present", None, None
    if disp and disp["shown"]:
        pcs = sorted(disp["shown"])
        rows_i = [rows[k + o]["i"] for k in keep]
        rows_t = [rows[k + o]["t"] for k in keep]
        cb = int(statistics.median(pcs)) - int(statistics.median(rows_i))
        cand = []
        for c in range(cb - 4, cb + 5):
            d = [disp["shown"][i + c] - tt for i, tt in zip(rows_i, rows_t) if (i + c) in disp["shown"]]
            if len(d) > 0.8 * len(rows_i):
                cand.append((float(np.std(d)), c))
        if cand:
            cand.sort()
            c_off = cand[0][1]
            src = "dxgi"
            shown_t = {k: disp["shown"].get(rows[k + o]["i"] + c_off) for k in keep}
    if shown_t is None:
        shown_t = {k: pt[k + o] for k in keep}
    err, back, dup = [], 0, 0
    for k0, k1 in zip(keep, keep[1:]):
        a, b = tr[k0], tr[k1]
        if k1 != k0 + 1 or a["mode"] > 1 or b["mode"] > 1:
            continue
        if shown_t.get(k0) is None or shown_t.get(k1) is None:
            continue
        ca = (a["tick"] - 1 + min(a["alpha"], 1.0)) * TICK_MS
        cb_ = (b["tick"] - 1 + min(b["alpha"], 1.0)) * TICK_MS
        dc, ds = cb_ - ca, shown_t[k1] - shown_t[k0]
        if dc < -0.01:
            back += 1
        elif abs(dc) <= 0.01:
            dup += 1
        err.append(dc - ds)
    if len(err) < 30:
        return None
    se = sorted(abs(e) for e in err)
    latk = [pt[k + o] - tr[k]["t0"] for k in keep]
    ls = sorted(latk)
    return dict(offset=o, offset_sd=sd, runner_up_sd=second, shown_src=src, dxgi_offset=c_off,
                pairs=len(err), err_std=statistics.pstdev(err), err_p95=pct(se, 0.95), err_p99=pct(se, 0.99),
                back=back, dup=dup, lat_spread_p1_p99=pct(ls, 0.99) - pct(ls, 0.01), lat_std=statistics.pstdev(latk))


def analyze(csv, t_from=0.0, t_to=1e12, auto=False, target=None, hz=0.0, trace=None):
    rows = load(csv)
    if not rows:
        raise SystemExit("no line read: %s" % csv)
    sel, a, b = window(rows, t_from, t_to, auto)
    if len(sel) < 10:
        raise SystemExit("too few frames in the window: %s" % csv)
    if not target and hz:
        target = 1000.0 / hz
    itv = [r["itv"] for r in sel]
    t = [r["t"] for r in sel]
    res = dict(csv=csv, win=[a, b], cpu=interval_stats(itv, t, target))
    res["cpu"]["pat"] = patterns(itv, t, res["cpu"]["p50"], target)
    res["clock"] = clock(sel, res["cpu"]["target"])
    if "wait" in sel[0]:
        res["wait_mean"] = statistics.fmean(r["wait"] for r in sel)
        res["present_mean"] = statistics.fmean(r["pres"] for r in sel)
    if hz:
        per = 1000.0 / hz
        off = sum(1 for x in itv if abs(x / per - round(x / per)) > 0.10 or round(x / per) == 0)
        res["not_multiple_pct"] = 100.0 * off / len(itv)
    dsp = display(sel)
    if dsp:
        res["display"] = {k: v for k, v in dsp.items() if k not in ("shown", "itv", "t")}
        if len(dsp["itv"]) >= 20:
            res["display"]["stats"] = interval_stats(dsp["itv"], dsp["t"], res["cpu"]["target"])
            res["display"]["stats"]["pat"] = patterns(dsp["itv"], dsp["t"], res["display"]["stats"]["p50"],
                                                      res["cpu"]["target"])
    if trace:
        res["content"] = content(load_trace(trace), rows, sel, dsp)
    return res


def fmt_stats(s, indent="  "):
    o = [indent + "%d intervals over %.1f s, %.1f fps (target %.2f ms)" % (s["n"], s["secs"], s["fps"], s["target"]),
         indent + "mean interval %.2f ms, std dev %.2f, p50 %.2f, p95 %.2f, p99 %.2f, p99.9 %.2f, max %.1f"
         % (s["mean"], s["std"], s["p50"], s["p95"], s["p99"], s["p999"], s["max"]),
         indent + "1 %% low %.1f fps, 0.1 %% low %.1f fps" % (s["low1"], s["low01"]),
         indent + "> 2x target: %d (%.2f / min); < 0.5x target (too close): %d (%.2f %%); > 1.5x median: %d"
         % (s["slow2"], s["slow2_min"], s["close"], s["close_pct"], s["gt15med"]),
         indent + "|change between successive intervals| mean %.2f ms, p99 %.2f" % (s["dmean"], s["dp99"]),
         indent + "most frequent intervals: " + ", ".join("%.1f-%.1f ms %.1f %%" % (b, b + 0.5, p)
                                                      for b, _, p in s["modes"])]
    p = s.get("pat")
    if p:
        o.append(indent + "patterns: autocorrelation peaks %s; stutters %d, spacing p50 %s, CV %s"
                 % (", ".join("lag %d r=%+.2f" % (l, r) for l, r in p["acf"]) or "none",
                    p["stutters"], "%.0f ms" % p["stutter_gap_p50"] if p["stutter_gap_p50"] else "-",
                    "%.2f" % p["stutter_gap_cv"] if p["stutter_gap_cv"] is not None else "-"))
    return o


def report(res):
    o = ["%s: window %.1f s -> %.1f s" % (res["csv"], res["win"][0], min(res["win"][1], 1e9)),
         " CPU (Present returned, presenting thread):"]
    o += fmt_stats(res["cpu"])
    c = res["clock"]
    o.append("  clock: fps per 10 s %s; mean interval vs target %s"
             % (c["win_fps"], "%+.2f %%" % c["drift_pct"] if c["drift_pct"] is not None else "-"))
    if "wait_mean" in res:
        o.append("  mean latency wait %.2f ms, mean Present %.2f ms" % (res["wait_mean"], res["present_mean"]))
    if "not_multiple_pct" in res:
        o.append("  not a multiple of the display period (within 10 %%): %.1f %%" % res["not_multiple_pct"])
    d = res.get("display")
    if d:
        o.append(" SCREEN (DXGI frame statistics):")
        o.append("  refresh period %.3f ms (p1 %.3f, p99 %.3f)%s" % (d["period"], d["period_p1"], d["period_p99"],
                                                                     " -> VARIABLE (VRR?)" if d["vrr"] else ""))
        o.append("  %d refreshes; frame held one refresh longer: %d (%.2f %%); frames never shown: %d"
                 % (d["refreshes"], d["held"], d["held_pct"], d["dropped"]))
        o.append("  new frame after %s; several frames between two readings: %d"
                 % (", ".join("%s refr. %d (%.1f %%)" % ("4+" if k == 4 else k, v, p) for k, v, p in d["after"]),
                    d["multi"]))
        o.append("  frames with a known time on screen: %.1f %% of the Presents%s" % (
            d["coverage"], "" if d["coverage"] >= 90 else " -> PARTIAL (the readings skip frames): the screen"
            " intervals below are a sample, read them with the refresh counts above"))
        if "stats" in d:
            o += fmt_stats(d["stats"])
    ct = res.get("content")
    if ct:
        o.append(" MOTION (trace: content step - screen step, %s times):" % ct["shown_src"])
        o.append("  render -> Present offset %d (std %.2f ms, runner-up %s)%s; %d pairs"
                 % (ct["offset"], ct["offset_sd"], "%.2f" % ct["runner_up_sd"] if ct["runner_up_sd"] else "-",
                    "" if ct["dxgi_offset"] is None else ", DXGI id offset %d" % ct["dxgi_offset"], ct["pairs"]))
        o.append("  judder: error std %.2f ms, |error| p95 %.2f, p99 %.2f; content backwards %d, repeated %d"
                 % (ct["err_std"], ct["err_p95"], ct["err_p99"], ct["back"], ct["dup"]))
        o.append("  render -> Present delay: std %.2f ms, p1-p99 spread %.2f ms" % (ct["lat_std"], ct["lat_spread_p1_p99"]))
    elif res.get("content", 0) is None:
        o.append(" MOTION: trace not usable (too few race renders matched)")
    return "\n".join(o)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv")
    ap.add_argument("--from", dest="t_from", type=float, default=0.0)
    ap.add_argument("--to", dest="t_to", type=float, default=1e12)
    ap.add_argument("--hz", type=float, default=0.0)
    ap.add_argument("--target-ms", type=float, default=0.0)
    ap.add_argument("--auto", action="store_true",
                    help="window = 8 s after the last interval > 250 ms (end of loading) up to the end")
    ap.add_argument("--trace", help="XBOX_FPS_INTERP_TRACE file of the same run")
    ap.add_argument("--json", help="write the numbers there too")
    a = ap.parse_args()
    res = analyze(a.csv, a.t_from, a.t_to, a.auto, a.target_ms or None, a.hz, a.trace)
    print(report(res))
    if a.json:
        with open(a.json, "w") as f:
            json.dump(res, f, indent=1)


if __name__ == "__main__":
    main()
