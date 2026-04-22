import argparse
import json
import math
from pathlib import Path


def var_p(p, s):
    if s <= 0 or abs(p) > s:
        return 0.0
    v = s * s - p * p
    return math.sqrt(v) if v > 0 else 0.0


def f(x, default=0.0):
    try:
        return float(x)
    except Exception:
        return default


def get_h(frame):
    # Prefer already-flattened harmonic aliases, else use h_i map
    if "h2_i_norm" in frame:
        return (
            f(frame.get("h2_i_norm", 0.0)),
            f(frame.get("h3_i_norm", 0.0)),
            f(frame.get("h4_i_norm", 0.0)),
            f(frame.get("h5_i_norm", 0.0)),
        )

    h_i = frame.get("h_i", {}) or {}
    return (
        f(h_i.get("120", 0.0)),
        f(h_i.get("180", 0.0)),
        f(h_i.get("240", 0.0)),
        f(h_i.get("300", 0.0)),
    )


def iter_frames(raw):
    """
    Supports either:
    1. Flat map of frames:
       { frameId: {...}, frameId2: {...} }

    2. Nested RTDB export:
       {
         "devices": {
           deviceId: {
             "streams": {
               fw: {
                 "frames": {
                   frameId: {...}
                 }
               }
             }
           }
         }
       }
    """
    if not isinstance(raw, dict):
        return

    # Case 1: already a flat frame map
    flat_like = all(isinstance(v, dict) for v in raw.values()) if raw else False
    if flat_like and "devices" not in raw:
        for frame_key, frame in raw.items():
            yield {
                "device_id": None,
                "stream_id": None,
                "frame_key": frame_key,
                "frame": frame,
            }
        return

    # Case 2: nested RTDB export
    devices = raw.get("devices", {})
    if not isinstance(devices, dict):
        return

    for device_id, device_obj in devices.items():
        if not isinstance(device_obj, dict):
            continue
        streams = device_obj.get("streams", {})
        if not isinstance(streams, dict):
            continue

        for stream_id, stream_obj in streams.items():
            if not isinstance(stream_obj, dict):
                continue
            frames = stream_obj.get("frames", {})
            if not isinstance(frames, dict):
                continue

            for frame_key, frame in frames.items():
                if not isinstance(frame, dict):
                    continue
                yield {
                    "device_id": device_id,
                    "stream_id": stream_id,
                    "frame_key": frame_key,
                    "frame": frame,
                }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="in_path", required=True, help="Input RTDB export JSON")
    ap.add_argument("--out", dest="out_path", required=True, help="Output flattened multilabel JSON")
    ap.add_argument(
        "--labels",
        required=True,
        help='Comma-separated labels, e.g. "Laptop,Hairdryer" or "" for off',
    )
    ap.add_argument("--min-p", type=float, default=0.0, help="Minimum |p| to keep")
    ap.add_argument("--on-only", action="store_true", help='Keep only frames with state == "on"')
    args = ap.parse_args()

    raw = json.loads(Path(args.in_path).read_text())

    labels = [x.strip() for x in args.labels.split(",") if x.strip()]

    rows = []
    total_seen = 0

    for item in iter_frames(raw):
        total_seen += 1
        frame = item["frame"]
        frame_key = item["frame_key"]

        if args.on_only and frame.get("state") != "on":
            continue

        p = abs(f(frame.get("p", 0.0)))
        pf_true = abs(f(frame.get("pf_true", 1.0), 1.0))

        s = abs(f(frame.get("s", 0.0)))
        if s == 0.0:
            s = abs(p / pf_true) if pf_true != 0 else 0.0

        if p < args.min_p:
            continue

        h2, h3, h4, h5 = get_h(frame)

        row = {
            "labels": labels,
            "event_id": str(frame.get("frame_id", frame_key)),
            "frame_key": str(frame_key),
            "device_id": item["device_id"],
            "stream_id": item["stream_id"],
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

    print(f"Scanned {total_seen} frames")
    print(f"Wrote {len(rows)} rows -> {args.out_path}")
    print(f"Labels used: {labels}")


if __name__ == "__main__":
    main()