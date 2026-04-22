import argparse, json, math
from pathlib import Path

def var_p(p, s):
    if s <= 0 or abs(p) > s:
        return 0.0
    v = s*s - p*p
    return math.sqrt(v) if v > 0 else 0.0

def get_h(frame):
    # prefer top-level aliases, else h_i map
    if "h2_i_norm" in frame:
        return (
            float(frame.get("h2_i_norm", 0.0)),
            float(frame.get("h3_i_norm", 0.0)),
            float(frame.get("h4_i_norm", 0.0)),
            float(frame.get("h5_i_norm", 0.0)),
        )
    h_i = frame.get("h_i", {}) or {}
    return (
        float(h_i.get("120", 0.0)),
        float(h_i.get("180", 0.0)),
        float(h_i.get("240", 0.0)),
        float(h_i.get("300", 0.0)),
    )

def f(x, default=0.0):
    try:
        return float(x)
    except Exception:
        return default

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="in_path", required=True)
    ap.add_argument("--out", dest="out_path", required=True)
    ap.add_argument("--label", required=True)
    ap.add_argument("--min-p", type=float, default=10.0)
    ap.add_argument("--on-only", action="store_true")
    args = ap.parse_args()

    raw = json.loads(Path(args.in_path).read_text())

    rows = []
    for rtdb_key, frame in raw.items():
        if not isinstance(frame, dict):
            continue

        if args.on_only and frame.get("state") != "on":
            continue

        p = f(frame.get("p", 0.0))
        pf_true = f(frame.get("pf_true", 1.0), 1.0)

        s = f(frame.get("s", 0.0))
        if s == 0.0:
            s = (p / pf_true) if pf_true != 0 else 0.0

        if p < args.min_p:
            continue

        h2, h3, h4, h5 = get_h(frame)

        row = {
            "appliance_type": args.label,
            "event_id": str(frame.get("frame_id", rtdb_key)),
            "p": p,
            "s": s,
            "var_p": f(frame.get("var_p", var_p(p, s))),
            "pf_true": pf_true,
            "rms_v": f(frame.get("rms_v", 0.0)),
            "rms_i": f(frame.get("rms_i", 0.0)),
            "thd_i": f(frame.get("thd_i", 0.0)),
            "crest_i": f(frame.get("crest_i", 0.0)),
            "h2_i_norm": h2,
            "h3_i_norm": h3,
            "h4_i_norm": h4,
            "h5_i_norm": h5,
        }
        rows.append(row)

    Path(args.out_path).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out_path).write_text(json.dumps(rows, indent=2))
    print(f"Wrote {len(rows)} rows -> {args.out_path}")

if __name__ == "__main__":
    main()