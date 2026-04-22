#!/usr/bin/env python3
"""
convert_raw_frames_folder.py

Convert a folder of raw RTDB frame JSON files into appliance_features.json-style rows.

Expected input:
  Machine_Learning/data/real/raw_frames/
    - can contain:
        * single-frame JSON files (a dict representing one frame)
        * RTDB-export JSON files (nested dicts containing many frames)

Output:
  - JSON array of rows matching your training schema.

Usage:
  python convert_raw_frames_folder.py \
    --in-dir Machine_Learning/data/real/raw_frames \
    --out Machine_Learning/data/real/features_rows/features.json

Optional:
  --allow-unlabeled   keep frames without labels (appliance_type="Unknown")
  --min-p 10          filter out low-power frames
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple


# -----------------------------
# Helpers
# -----------------------------
def safe_float(x: Any, default: float = 0.0) -> float:
    try:
        if x is None:
            return default
        return float(x)
    except (TypeError, ValueError):
        return default


def compute_var_p(p: float, s: float) -> float:
    if s <= 0.0 or abs(p) > s:
        return 0.0
    val = s * s - p * p
    return math.sqrt(val) if val > 0.0 else 0.0


def get_harmonics(frame: Dict[str, Any]) -> Tuple[float, float, float, float]:
    # Prefer top-level aliases if present
    if any(k in frame for k in ("h2_i_norm", "h3_i_norm", "h4_i_norm", "h5_i_norm")):
        return (
            safe_float(frame.get("h2_i_norm", 0.0)),
            safe_float(frame.get("h3_i_norm", 0.0)),
            safe_float(frame.get("h4_i_norm", 0.0)),
            safe_float(frame.get("h5_i_norm", 0.0)),
        )

    h_i = frame.get("h_i")
    if isinstance(h_i, dict):
        return (
            safe_float(h_i.get("120", 0.0)),
            safe_float(h_i.get("180", 0.0)),
            safe_float(h_i.get("240", 0.0)),
            safe_float(h_i.get("300", 0.0)),
        )

    return (0.0, 0.0, 0.0, 0.0)


def extract_label(frame: Dict[str, Any]) -> Optional[str]:
    labels = frame.get("labels")
    if isinstance(labels, list) and labels:
        if isinstance(labels[0], str) and labels[0].strip():
            return labels[0].strip()
    if isinstance(frame.get("appliance_type"), str) and frame["appliance_type"].strip():
        return frame["appliance_type"].strip()
    return None


def looks_like_frame(d: Dict[str, Any]) -> bool:
    # Heuristic: contains at least some of the feature keys your frames carry.
    return any(k in d for k in ("p", "pf_true", "rms_i", "thd_i", "h_i", "labels"))


def make_event_id(frame: Dict[str, Any], fallback: str) -> str:
    # Prefer explicit frame_id, else use fallback (filename/path-based)
    fid = frame.get("frame_id")
    if fid is not None:
        return str(fid)
    return fallback


def make_row(frame: Dict[str, Any], event_id: str, allow_unlabeled: bool) -> Optional[Dict[str, Any]]:
    label = extract_label(frame)
    if not label and not allow_unlabeled:
        return None

    appliance_type = label or "Unknown"

    p = safe_float(frame.get("p", 0.0))
    pf_true = safe_float(frame.get("pf_true", 1.0), default=1.0)

    # s: use provided or compute from p/pf_true
    if "s" in frame:
        s = safe_float(frame.get("s", 0.0))
    else:
        s = (p / pf_true) if pf_true != 0.0 else 0.0

    # var_p: use provided or compute
    if "var_p" in frame:
        var_p = safe_float(frame.get("var_p", 0.0))
    else:
        var_p = compute_var_p(p, s)

    rms_v = safe_float(frame.get("rms_v", 0.0))
    rms_i = safe_float(frame.get("rms_i", 0.0))
    thd_i = safe_float(frame.get("thd_i", 0.0))
    crest_i = safe_float(frame.get("crest_i", 0.0))

    h2, h3, h4, h5 = get_harmonics(frame)

    return {
        "appliance_type": appliance_type,
        "event_id": event_id,
        "p": p,
        "s": s,
        "var_p": var_p,
        "pf_true": pf_true,
        "rms_v": rms_v,
        "rms_i": rms_i,
        "thd_i": thd_i,
        "crest_i": crest_i,
        "h2_i_norm": h2,
        "h3_i_norm": h3,
        "h4_i_norm": h4,
        "h5_i_norm": h5,
    }


def find_frames(obj: Any, path: str = "") -> List[Tuple[str, Dict[str, Any]]]:
    """
    Recursively walk nested JSON and collect (fallback_event_id, frame_dict) pairs.
    If a dict looks like a frame, treat it as a frame.
    """
    out: List[Tuple[str, Dict[str, Any]]] = []

    if isinstance(obj, dict):
        if looks_like_frame(obj):
            fallback_id = path.rsplit("/", 1)[-1] if path else "frame"
            out.append((fallback_id, obj))

        for k, v in obj.items():
            new_path = f"{path}/{k}" if path else str(k)
            out.extend(find_frames(v, new_path))

    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            out.extend(find_frames(v, f"{path}[{i}]"))

    return out


# -----------------------------
# Main
# -----------------------------
def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--in-dir", required=True, help="Folder containing raw frame JSON files")
    ap.add_argument("--out", required=True, help="Output JSON file (appliance_features-style)")
    ap.add_argument("--allow-unlabeled", action="store_true", help='Keep frames without labels (appliance_type="Unknown")')
    ap.add_argument("--min-p", type=float, default=None, help="Optional: only keep rows with p >= min_p")
    args = ap.parse_args()

    in_dir = Path(args.in_dir)
    if not in_dir.exists() or not in_dir.is_dir():
        raise SystemExit(f"Input directory not found: {in_dir}")

    json_files = sorted([p for p in in_dir.rglob("*.json") if p.is_file()])
    if not json_files:
        raise SystemExit(f"No .json files found under: {in_dir}")

    rows: List[Dict[str, Any]] = []
    skipped_files: List[str] = []

    for fp in json_files:
        try:
            with fp.open("r") as f:
                data = json.load(f)
        except Exception:
            skipped_files.append(str(fp))
            continue

        frames = find_frames(data, path=fp.stem)

        for fallback_id, frame in frames:
            # Make event_id stable across folder: prefix with filename stem
            event_id = make_event_id(frame, fallback=f"{fp.stem}:{fallback_id}")

            row = make_row(frame, event_id=event_id, allow_unlabeled=args.allow_unlabeled)
            if row is None:
                continue
            if args.min_p is not None and row["p"] < args.min_p:
                continue
            rows.append(row)

    # Sort for stable output
    rows.sort(key=lambda r: (r["appliance_type"], str(r["event_id"])))

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(rows, indent=2))

    print(f"Found {len(json_files)} JSON files.")
    if skipped_files:
        print(f"Skipped {len(skipped_files)} unreadable JSON files (first 3 shown):")
        for s in skipped_files[:3]:
            print("  -", s)
    print(f"Wrote {len(rows)} rows to {out_path}")


if __name__ == "__main__":
    main()