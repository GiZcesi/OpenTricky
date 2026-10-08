"""np_net_cmp -- compares the measurement logs of the two instances.

    python np_net_cmp.py HOST.bin GUEST.bin [--state 4] [--csv out.csv]

Logs written with XBOX_NET_LOG (port/src/netplay/np_net.h, np_net_logrec).
For each race (load number) and each direction (host -> copy on the guest,
guest -> copy on the host), pairs by shared tick k the position of the
Player on its owner (kind 0) and the position of its copy on the other side
(kind 1, BEFORE correction = what is displayed). Gives:
  - the p50 / p95 / max gap (units and m, 1 m = 39.4 u) in the requested
    state, leaving out ticks where the copy is handed back to the AI;
  - the start: QPC gap of the first tick after the barrier (k = 0) and of
    the first tick of state 4, in ms and in 16.67 ms frames;
  - the real-time offset between the two instances at the same k (p50 / p95 / max);
  - the share of exact / repeated commands and of applied snapshots.
"""
import struct, sys, argparse, math

REC = struct.Struct("<IiBBBB3fdiHBB")
F_EXACT, F_HELD, F_SNAP, F_AI, F_BARRIER = 1, 2, 4, 8, 16
M = 39.4


def load(path):
    with open(path, "rb") as f:
        b = f.read()
    magic, ver, role, rsz = struct.unpack_from("<4I", b, 0)
    assert magic == 0x4C4E504E and rsz == REC.size, (hex(magic), rsz, REC.size)
    out = []
    for off in range(16, len(b) - rsz + 1, rsz):
        load_, k, kind, st, fl, slot, x, y, z, q, rk, age, ev, mode = REC.unpack_from(b, off)
        out.append(dict(load=load_, k=k, kind=kind, st=st, fl=fl, slot=slot, p=(x, y, z), q=q, rk=rk, age=age,
                        ev=ev, mode=mode))
    return role, out


def pct(v, p):
    if not v:
        return float("nan")
    v = sorted(v)
    return v[min(len(v) - 1, int(round(p / 100.0 * (len(v) - 1))))]


def index(recs, kind):
    d = {}
    for r in recs:
        if r["kind"] == kind and r["k"] >= 0:
            d.setdefault((r["load"], r["k"]), r)
    return d


def direction(name, own, copy, state, csv):
    o = index(own, 0)
    c = index(copy, 1)
    loads = sorted({l for (l, _) in o})
    for ld in loads:
        errs, n_ai, n_ex, n_held, n_snap, ages, lag = [], 0, 0, 0, 0, [], []
        for (l, k), ro in sorted(o.items()):
            if l != ld or (l, k) not in c:
                continue
            rc = c[(l, k)]
            lag.append(rc["q"] - ro["q"])
            if state is not None and (ro["st"] != state or rc["st"] != state):
                continue
            if rc["fl"] & F_AI:
                n_ai += 1
                continue
            e = math.dist(ro["p"], rc["p"])
            errs.append(e)
            n_ex += bool(rc["fl"] & F_EXACT)
            n_held += bool(rc["fl"] & F_HELD)
            if rc["fl"] & F_SNAP:
                n_snap += 1
                ages.append(rc["age"])
            if csv:
                csv.write("%s,%d,%d,%d,%.3f,%d,%d,%d,%d,%d\n" % (name, l, k, ro["st"], e, rc["fl"],
                      ro["ev"], ro["mode"], rc["ev"], rc["mode"]))
        n = len(errs)
        print("[%s] race %d: %d paired ticks (state %s), copy AI %d" % (name, ld, n, state, n_ai))
        if n:
            big = sum(1 for e in errs if e > M)
            print("    gap  p50 %.1f u  p95 %.1f u (%.2f m)  max %.1f u (%.2f m)  > 1 m: %d ticks (%.2f %%)"
                  % (pct(errs, 50), pct(errs, 95), pct(errs, 95) / M, max(errs), max(errs) / M, big, 100.0 * big / n))
            print("    exact commands %.1f %%, repeated %.1f %%; snapshots applied %d (age p50 %s, max %s)"
                  % (100.0 * n_ex / n, 100.0 * n_held / n, n_snap, pct(ages, 50) if ages else "-", max(ages) if ages else "-"))
        if lag:
            print("    real-time offset (copy - owner, same k): p50 %.1f ms  p95 %.1f ms  max %.1f ms"
                  % (pct(lag, 50), pct([abs(x) for x in lag], 95), max(abs(x) for x in lag)))


def start(host, guest):
    def firsts(recs):
        d = {}
        for r in recs:
            if r["kind"] != 0 or r["k"] < 0:
                continue
            if r["k"] == 0:
                d.setdefault((r["load"], "k0"), r)
            if r["st"] == 4:
                d.setdefault((r["load"], "st4"), r)
        return d
    a, b = firsts(host), firsts(guest)
    for key in sorted(set(a) & set(b)):
        dq = b[key]["q"] - a[key]["q"]
        print("[start] race %d, %s: host k=%d, guest k=%d, QPC gap guest - host = %.1f ms = %.2f frames"
              % (key[0], key[1], a[key]["k"], b[key]["k"], dq, dq / 16.667))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("guest")
    ap.add_argument("--state", type=int, default=4)
    ap.add_argument("--csv")
    a = ap.parse_args()
    rh, h = load(a.host)
    rg, g = load(a.guest)
    assert rh == 0 and rg == 1, "expected order: HOST.bin GUEST.bin"
    csv = open(a.csv, "w") if a.csv else None
    if csv:
        csv.write("direction,race,k,state,gap,flags,ev_owner,mode_owner,ev_copy,mode_copy\n")
    start(h, g)
    direction("host->guest", h, g, a.state, csv)
    direction("guest->host", g, h, a.state, csv)


if __name__ == "__main__":
    main()
