#!/usr/bin/env python
"""
Recompute per-appliance power estimates from PLAID single-appliance data
and sanity-check them against the synthetic multi-label dataset.

Usage:
    python tools/recompute_power_estimates.py
"""

import json
from pathlib import Path

import numpy as np
import pandas as pd


# Paths relative to repo root
PLAID_PATH = Path("data/processed/appliance_features.json")
MULTI_PATH = Path("data/processed/appliance_features_multilabel_sim.json")


def compute_power_estimates_from_plaid() -> dict:
    """
    Load single-appliance PLAID features and compute a robust per-appliance
    power estimate in watts (using median of |p|).
    """
    if not PLAID_PATH.exists():
        raise FileNotFoundError(f"PLAID file not found: {PLAID_PATH}")

    df = pd.read_json(PLAID_PATH)
    if "appliance_type" not in df.columns or "p" not in df.columns:
        raise ValueError("Expected 'appliance_type' and 'p' columns in PLAID data")

    # Use absolute power in case of sign conventions
    df["p_abs"] = df["p"].abs()

    # Median is robust to outliers; you could swap to mean / clipped mean if desired
    medians = (
        df.groupby("appliance_type")["p_abs"]
          .median()
          .round()
          .astype(int)
    )

    print("=== Per-appliance |p| stats from PLAID (median in watts) ===")
    stats = (
        df.groupby("appliance_type")["p_abs"]
          .agg(["count", "mean", "median", "std", "min", "max"])
          .round(1)
    )
    print(stats.to_string())
    print()

    power_estimates = medians.to_dict()
    return power_estimates


def check_against_multi(power_estimates: dict) -> None:
    """
    Compare estimated power (sum of POWER_ESTIMATES over labels) against
    actual synthetic frame power in appliance_features_multilabel_sim.json.
    """
    if not MULTI_PATH.exists():
        print(f"[INFO] Multi-label file {MULTI_PATH} not found; skipping consistency check.")
        return

    with open(MULTI_PATH, "r") as f:
        data = json.load(f)

    df = pd.DataFrame(data)
    if "labels" not in df.columns or "p" not in df.columns:
        print("[WARN] Multi-label file missing 'labels' or 'p'; skipping consistency check.")
        return

    ratios = []
    errors = []

    for _, row in df.iterrows():
        labels = row["labels"]
        if not labels:
            continue

        p_meas = abs(row["p"])
        if p_meas <= 1.0:
            # Avoid divide-by-zero / near-zero frames
            continue

        p_est = sum(power_estimates.get(lbl, 0) for lbl in labels)
        ratios.append(p_est / p_meas)
        errors.append(p_est - p_meas)

    if not ratios:
        print("[WARN] No usable frames for ratio check.")
        return

    ratios = np.array(ratios, dtype=float)
    errors = np.array(errors, dtype=float)

    print("=== Consistency of POWER_ESTIMATES vs synthetic multi-label frames ===")
    print("p_est = sum POWER_ESTIMATES[label], p_meas = |frame p|")
    print("p_est / p_meas (ratio):")
    print(f"  mean = {ratios.mean():.3f}")
    print(f"  std  = {ratios.std():.3f}")
    print(f"  5%   = {np.quantile(ratios, 0.05):.3f}")
    print(f"  50%  = {np.quantile(ratios, 0.50):.3f}")
    print(f"  95%  = {np.quantile(ratios, 0.95):.3f}")
    print()
    print("|p_est - p_meas| (absolute error in watts):")
    print(f"  mean = {np.abs(errors).mean():.1f}")
    print(f"  95%  = {np.quantile(np.abs(errors), 0.95):.1f}")
    print()

    # Optional: suggest a global rescale if ratio mean is far from 1
    ratio_mean = ratios.mean()
    if 0.8 <= ratio_mean <= 1.2:
        print("[INFO] Ratio mean is close to 1.0; POWER_ESTIMATES are reasonably calibrated.")
    else:
        scale = 1.0 / ratio_mean
        print("[INFO] Ratio mean is far from 1.0; you *may* want to rescale POWER_ESTIMATES.")
        print(f"       Suggested global scale factor: {scale:.3f}")
        print("       (Multiply each power estimate by this factor and round to nearest int.)")
    print()


def main() -> None:
    power_estimates = compute_power_estimates_from_plaid()

    print("=== Suggested POWER_ESTIMATES dict for api.py ===")
    print("POWER_ESTIMATES = {")
    for k in sorted(power_estimates.keys()):
        v = int(power_estimates[k])
        print(f'    "{k}": {v},')
    print("}")
    print()

    # Cross-check with synthetic multi-label frames if available
    check_against_multi(power_estimates)


if __name__ == "__main__":
    main()
