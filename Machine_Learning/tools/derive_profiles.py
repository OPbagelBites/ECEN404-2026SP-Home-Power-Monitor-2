#!/usr/bin/env python
import argparse
import math
from pathlib import Path

import pandas as pd


def main():
    ap = argparse.ArgumentParser(
        description="Derive ESP ApplianceProfile suggestions from labeled feature data"
    )
    ap.add_argument(
        "--data_path",
        type=Path,
        default=Path("data/processed/appliance_features.json"),
        help="Path to JSON with extracted features (list of dicts).",
    )
    ap.add_argument(
        "--target_col",
        type=str,
        default="appliance_type",
        help="Name of the label column (e.g. 'appliance_type').",
    )
    ap.add_argument(
        "--p_on_threshold",
        type=float,
        default=50.0,
        help="Minimum active power (W) to treat a row as 'appliance ON'.",
    )
    args = ap.parse_args()

    if not args.data_path.exists():
        raise SystemExit(f"Data file not found: {args.data_path}")

    # Load JSON as DataFrame (list-of-dicts)
    df = pd.read_json(args.data_path)

    if args.target_col not in df.columns:
        raise SystemExit(
            f"Target column '{args.target_col}' not found. "
            f"Columns: {list(df.columns)}"
        )

    # Required features
    for col in ("p", "rms_i"):
        if col not in df.columns:
            raise SystemExit(f"Expected '{col}' column in data.")

    # PF column (optional)
    pf_col = None
    for c in ("pf_true", "PF", "pf"):
        if c in df.columns:
            pf_col = c
            break

    if pf_col is None:
        print("WARNING: no PF column found; PHASE_DEG will default to 0.")
    else:
        print(f"Using '{pf_col}' as PF feature.")

    # Filter to "on" rows only using power threshold
    df_on = df[df["p"] >= args.p_on_threshold].copy()
    print(
        f"Total rows: {len(df)}, using {len(df_on)} rows with "
        f"p >= {args.p_on_threshold} W"
    )

    if df_on.empty:
        raise SystemExit("No rows pass the p_on_threshold filter.")

    g = df_on.groupby(args.target_col)

    if "h2_i_norm" not in df.columns:
        print("WARNING: no 'h2_i_norm' column; H2_ON will default to 0.05.")

    profiles = []

    for label, group in g:
        label_str = str(label)

        irms_med = float(group["rms_i"].median())
        p_med = float(group["p"].median())

        # H2
        if "h2_i_norm" in group.columns:
            h2_med = float(group["h2_i_norm"].median())
            # clamp to a sane range [0, 0.8]
            h2_med = max(0.0, min(h2_med, 0.8))
        else:
            h2_med = 0.05

        # Phase from PF if available
        if pf_col is not None:
            pf_med = float(group[pf_col].median())
            # clamp PF to [-0.99, 0.99] for safety
            pf_clamped = max(min(pf_med, 0.99), -0.99)
            try:
                phi_rad = math.acos(pf_clamped)
                phase_deg = math.degrees(phi_rad)
            except ValueError:
                phase_deg = 0.0
        else:
            phase_deg = 0.0

        # final sanity clamps
        irms_med = max(0.01, min(irms_med, 30.0))   # don't let it be insane
        phase_deg = max(0.0, min(phase_deg, 90.0))

        profiles.append(
            {
                "label": label_str,
                "irms_on": irms_med,
                "h2_on": h2_med,
                "phase_deg": phase_deg,
                "p_med": p_med,
            }
        )

    # Print summary + C++ snippet
    print("\n=== Derived Appliance Profiles (ON-only, using rms_i) ===\n")
    for p in profiles:
        print(
            f"// {p['label']}: I_RMS_med ≈ {p['irms_on']:.2f} A, "
            f"H2_ON ≈ {p['h2_on']:.3f}, PHASE ≈ {p['phase_deg']:.1f}° "
            f"(P_med ≈ {p['p_med']:.1f} W)"
        )

    print("\nstatic const ApplianceProfile PROFILES[] = {")
    for p in profiles:
        print(
            f'  {{"{p["label"]}", '
            f'{p["irms_on"]:.2f}f, {p["h2_on"]:.3f}f, {p["phase_deg"]:.1f}f}},'
        )
    print("};")
    print("static constexpr int NUM_PROFILES = sizeof(PROFILES)/sizeof(PROFILES[0]);")


if __name__ == "__main__":
    main()
