#!/usr/bin/env python
"""
Simulate breaker-level multi-label frames from single-appliance PLAID features.
UPDATED: Calculates and includes Reactive Power (var_p).
"""

import argparse
import json
import logging
import random
from pathlib import Path
from typing import Dict, List

import numpy as np
import pandas as pd
import sys

# Ensure repo root is in path to import src.config
ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from src.config import MODEL_FEATURES, MODEL_CLASSES

logger = logging.getLogger(__name__)


def _load_base_features(path: Path) -> pd.DataFrame:
    df = pd.read_json(path)
    if "appliance_type" not in df.columns:
        raise ValueError("Expected 'appliance_type' column. Got: " + str(df.columns.tolist()))

    # Ensure standard features exist (we will add var_p later if missing)
    # We don't force var_p here because the source data might not have it yet.
    # We only ensure the basic numeric columns exist to avoid KeyErrors.
    base_cols = [c for c in MODEL_FEATURES if c != "var_p"]
    for col in base_cols:
        if col not in df.columns:
            df[col] = 0.0

    df[base_cols] = df[base_cols].astype(float).fillna(0.0)
    df["appliance_type"] = df["appliance_type"].astype(str)
    return df


def _build_class_index(df: pd.DataFrame) -> Dict[str, pd.DataFrame]:
    by_class: Dict[str, pd.DataFrame] = {}
    for cls in MODEL_CLASSES:
        sub = df[df["appliance_type"] == cls].copy()
        if len(sub) == 0:
            logger.warning("No rows found for class '%s'", cls)
        by_class[cls] = sub
    return by_class


def _mix_rows(rows: List[pd.Series]) -> Dict[str, float]:
    """
    Combine k single-appliance feature rows into one synthetic breaker frame.
    UPDATED: Sums Active (P) and Reactive (Q) power separately.
    """
    if not rows:
        raise ValueError("No rows to mix")

    # 1. Extract base P and S to calculate Q per appliance
    p_vals = np.array([r["p"] for r in rows], dtype=float)
    s_vals = np.array([r["s"] for r in rows], dtype=float)

    noise_factor = np.random.normal(1.0, 0.05, size=len(p_vals)) # Mean 1.0, StdDev 5%
    p_vals = p_vals * noise_factor
    s_vals = s_vals * noise_factor

    # Calculate Reactive Power (Q) = sqrt(S^2 - P^2)
    # Use max(0, ...) to prevent NaN from tiny floating point errors
    q_vals = np.sqrt(np.maximum(0, s_vals**2 - p_vals**2))

    # 2. Sum components (Physics-correct mixing)
    p_total = float(p_vals.sum())
    q_total = float(q_vals.sum())
    
    # Recalculate Total S
    s_total = float(np.sqrt(p_total**2 + q_total**2))

    # RMS Current (approximate scalar sum for safety, though vector sum is more accurate)
    rms_i_vals = np.array([r["rms_i"] for r in rows], dtype=float)
    rms_i_total = float(rms_i_vals.sum())

    # Power Factor
    pf_true = p_total / s_total if s_total > 0 else 1.0

    # 3. Weighted averages for harmonic features (weighted by S of each device)
    def wavg(feat: str) -> float:
        vals = np.array([r.get(feat, 0.0) for r in rows], dtype=float)
        if s_total <= 0 or np.allclose(s_vals.sum(), 0.0):
            return float(vals.mean())
        weights = s_vals / s_vals.sum()
        return float(np.sum(vals * weights))

    out = {
        "p": p_total,
        "s": s_total,
        "var_p": q_total,  # <--- NEW FEATURE
        "pf_true": pf_true,
        "rms_i": rms_i_total,
        "thd_i": wavg("thd_i"),
        "crest_i": wavg("crest_i"),
        "h2_i_norm": wavg("h2_i_norm"),
        "h3_i_norm": wavg("h3_i_norm"),
        "h4_i_norm": wavg("h4_i_norm"),
        "h5_i_norm": wavg("h5_i_norm"),
    }
    return out


def simulate_multilabel(
    base_df: pd.DataFrame,
    n_samples: int,
    single_frac: float = 0.5,
    pair_frac: float = 0.3,
    triple_frac: float = 0.2,
    seed: int = 42,
) -> List[dict]:
    rng = random.Random(seed)
    np_rng = np.random.default_rng(seed)
    by_class = _build_class_index(base_df)

    if not (abs(single_frac + pair_frac + triple_frac - 1.0) < 1e-6):
        raise ValueError("Fractions must sum to 1.0")

    samples: List[dict] = []
    for _ in range(n_samples):
        r = rng.random()
        if r < single_frac:
            k = 1
        elif r < single_frac + pair_frac:
            k = 2
        else:
            k = 3

        chosen_classes = [rng.choice(MODEL_CLASSES) for _ in range(k)]
        rows = []
        labels = []
        for cls in chosen_classes:
            pool = by_class.get(cls)
            if pool is None or len(pool) == 0:
                continue
            ridx = np_rng.integers(0, len(pool))
            row = pool.iloc[int(ridx)]
            rows.append(row)
            labels.append(cls)

        if not rows:
            row = base_df.iloc[int(np_rng.integers(0, len(base_df)))]
            rows = [row]
            labels = [str(row["appliance_type"])]

        feat = _mix_rows(rows)

        # Ensure we only export what is in MODEL_FEATURES (plus labels)
        record = {k: float(feat.get(k, 0.0)) for k in MODEL_FEATURES}
        record["labels"] = sorted(set(labels))
        samples.append(record)

    return samples


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="in_path", type=Path, required=True)
    ap.add_argument("--out", dest="out_path", type=Path, required=True)
    ap.add_argument("--n", dest="n_samples", type=int, default=4000)
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")

    df = _load_base_features(args.in_path)
    logging.info("Loaded %d base rows from %s", len(df), args.in_path)

    samples = simulate_multilabel(df, args.n_samples, seed=args.seed)
    logging.info("Generated %d synthetic multi-label frames with var_p", len(samples))

    args.out_path.parent.mkdir(parents=True, exist_ok=True)
    args.out_path.write_text(json.dumps(samples, indent=2))
    logging.info("Wrote multi-label dataset to %s", args.out_path)


if __name__ == "__main__":
    main()