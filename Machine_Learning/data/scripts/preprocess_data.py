"""
Patches zero-valued harmonic features by computing them from the raw
current samples (dbg.i_samp) that the ESP firmware DOES send.

Usage:
    python preprocess_data.py --input raw_firebase_export.json --output training_data.json
"""

import argparse
import json
import numpy as np
from pathlib import Path


def patch_frame(frame: dict) -> dict:
    """Compute crest_i, thd_i, i1_rms, and h_i from dbg.i_samp."""

    dbg = frame.get("dbg", {})
    i_samp = np.array(dbg.get("i_samp", []), dtype=float)
    fs = float(dbg.get("fs_eff", 996.82))
    freq = float(frame.get("freq_hz", 60.0))

    if len(i_samp) < 4:
        return frame
    
    # Absolute value for power features that should never be negative
    POWER_FIELDS = ["p", "s", "p1", "s1", "q1"]

    for field in POWER_FIELDS:
        if field in frame and frame[field] is not None:
            frame[field] = abs(frame[field])

    # ── Crest Factor ──────────────────────────────────────
    rms = np.sqrt(np.mean(i_samp ** 2))
    peak = np.max(np.abs(i_samp))
    crest_i = float(peak / rms) if rms > 1e-9 else 0.0

    # ── FFT (zero-pad 32 → 128 for better freq resolution) ──
    pad_len = max(128, len(i_samp))
    padded = np.zeros(pad_len)
    padded[: len(i_samp)] = i_samp

    fft_vals = np.fft.rfft(padded)
    magnitudes = np.abs(fft_vals) * 2.0 / pad_len
    freqs = np.fft.rfftfreq(pad_len, d=1.0 / fs)

    # Fundamental (closest bin to 60 Hz)
    fund_idx = int(np.argmin(np.abs(freqs - freq)))
    fund_mag = magnitudes[fund_idx]

    # i1_rms
    i1_rms = float(fund_mag / np.sqrt(2)) if fund_mag > 1e-9 else 0.0

    # Harmonics relative to fundamental
    harmonic_freqs = [120, 180, 240, 300]
    h_i = {}
    h_mags = []
    for h_freq in harmonic_freqs:
        h_idx = int(np.argmin(np.abs(freqs - h_freq)))
        h_mags.append(magnitudes[h_idx])
        if fund_mag > 1e-9:
            h_i[str(h_freq)] = round(float(magnitudes[h_idx] / fund_mag), 6)
        else:
            h_i[str(h_freq)] = 0.0

    # THD
    if fund_mag > 1e-9:
        thd_i = round(float(np.sqrt(sum(m ** 2 for m in h_mags)) / fund_mag), 6)
    else:
        thd_i = 0.0

    # ── Overwrite zeros ───────────────────────────────────
    if frame.get("crest_i", 0) == 0:
        frame["crest_i"] = round(crest_i, 6)
    if frame.get("thd_i", 0) == 0:
        frame["thd_i"] = round(thd_i, 6)
    if frame.get("i1_rms", 0) == 0:
        frame["i1_rms"] = round(i1_rms, 6)

    existing_h = frame.get("h_i", {})
    if all(existing_h.get(str(f), 0) == 0 for f in harmonic_freqs):
        frame["h_i"] = h_i

    return frame


def flatten_firebase(data, labels=None):
    """
    Walk a nested Firebase RTDB export and extract every frame
    that has 'rms_i' or 'p'.  Attach the given labels list.
    """
    results = []
    if isinstance(data, dict):
        if "rms_i" in data or "p" in data:
            frame = dict(data)
            if labels is not None:
                frame["labels"] = labels
            results.append(frame)
        else:
            for v in data.values():
                results.extend(flatten_firebase(v, labels))
    elif isinstance(data, list):
        for item in data:
            results.extend(flatten_firebase(item, labels))
    return results


HARMONIC_FREQ_TO_LABEL = {
    "120": "h2_i_norm",   # 2nd harmonic = 2 × 60 Hz = 120 Hz
    "180": "h3_i_norm",   # 3rd harmonic = 3 × 60 Hz = 180 Hz
    "240": "h4_i_norm",   # 4th harmonic = 4 × 60 Hz = 240 Hz
    "300": "h5_i_norm",   # 5th harmonic = 5 × 60 Hz = 300 Hz
}

def flatten_h_i(frame: dict) -> dict:
    """Convert nested h_i dict into flat columns using existing naming convention."""
    h_i = frame.pop("h_i", {})
    if isinstance(h_i, dict):
        for freq_key, val in h_i.items():
            col_name = HARMONIC_FREQ_TO_LABEL.get(freq_key, f"h_i_{freq_key}")
            frame[col_name] = val
    frame.pop("dbg", None)
    frame.pop("ml", None)
    return frame


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True,
                        help="Raw Firebase JSON export")
    parser.add_argument("--output", type=Path, required=True,
                        help="Output path for training-ready JSON")
    parser.add_argument("--labels", nargs="+", required=True,
                        help="Appliance labels for this file, e.g. --labels Hairdryer Laptop_Charger")
    args = parser.parse_args()

    with open(args.input) as f:
        raw = json.load(f)

    # Flatten nested Firebase structure into a list of frames
    frames = flatten_firebase(raw, labels=args.labels)
    print(f"Found {len(frames)} frames")

    # Patch every frame
    patched = []
    for frame in frames:
        frame = patch_frame(frame)
        frame = flatten_h_i(frame)
        patched.append(frame)

    # Print a sample so you can verify it worked
    if patched:
        sample = patched[0]
        print(f"\n--- Sample patched frame ---")
        print(f"  p:        {sample.get('p')}")
        print(f"  rms_i:    {sample.get('rms_i')}")
        print(f"  crest_i:  {sample.get('crest_i')}")
        print(f"  thd_i:    {sample.get('thd_i')}")
        print(f"  i1_rms:   {sample.get('i1_rms')}")
        print(f"  h2_i_norm:  {sample.get('h2_i_norm')}")
        print(f"  h3_i_norm:  {sample.get('h3_i_norm')}")
        print(f"  h4_i_norm:  {sample.get('h4_i_norm')}")
        print(f"  h5_i_norm:  {sample.get('h5_i_norm')}")
        print(f"  labels:   {sample.get('labels')}")

    with open(args.output, "w") as f:
        json.dump(patched, f, indent=2)

    print(f"\nSaved {len(patched)} patched frames to {args.output}")


if __name__ == "__main__":
    main()