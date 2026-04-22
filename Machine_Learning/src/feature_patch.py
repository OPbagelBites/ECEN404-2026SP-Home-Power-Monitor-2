"""
Server-side feature extraction from raw current/voltage samples.

The ESP firmware reports fft_ok=true but writes zeros for all harmonic
fields (thd_i, h_i, crest_i, i1_rms).  The raw ADC samples ARE present
in dbg.i_samp, so we compute everything ourselves.

Usage:
    from src.feature_patch import patch_zero_features, add_derived_features

Call patch_zero_features() on every raw frame from Firebase BEFORE
building the feature vector — in both training data loading AND the
Cloud Run inference endpoint.
"""

import numpy as np
import pandas as pd
from typing import Dict, Any


# ---------------------------------------------------------------------------
# 1.  Frame-level patch 
# ---------------------------------------------------------------------------

def patch_zero_features(frame: Dict[str, Any]) -> Dict[str, Any]:
    """
    Recompute harmonic / waveform features from dbg.i_samp when the
    firmware reports them as zero.

    Overwrites: crest_i, thd_i, i1_rms, h_i.{120,180,240,300}
    Only touches fields that are currently 0 — safe to call on every frame.
    """
    dbg = frame.get("dbg", {})
    i_samp = np.array(dbg.get("i_samp", []), dtype=float)
    fs = float(dbg.get("fs_eff", 996.82))
    freq = float(frame.get("freq_hz", 60.0))

    if len(i_samp) < 4:
        return frame
    

    # ── Crest Factor ──────────────────────────────────────────────
    rms_from_samples = np.sqrt(np.mean(i_samp ** 2))
    peak = np.max(np.abs(i_samp))
    crest_i = float(peak / rms_from_samples) if rms_from_samples > 1e-9 else 0.0

    # ── FFT ───
    pad_len = max(128, len(i_samp))
    padded = np.zeros(pad_len)
    padded[: len(i_samp)] = i_samp

    fft_vals = np.fft.rfft(padded)
    magnitudes = np.abs(fft_vals) * 2.0 / pad_len
    freqs = np.fft.rfftfreq(pad_len, d=1.0 / fs)

    # Fundamental 
    fund_idx = int(np.argmin(np.abs(freqs - freq)))
    fund_mag = magnitudes[fund_idx]

    # ── i1_rms  ───────────────────
    i1_rms = float(fund_mag / np.sqrt(2)) if fund_mag > 1e-9 else 0.0

    # ── Individual harmonics  ────────────
    harmonic_freqs = [120, 180, 240, 300]  # 2nd through 5th
    h_i: Dict[str, float] = {}
    h_mags_abs = []

    for h_freq in harmonic_freqs:
        h_idx = int(np.argmin(np.abs(freqs - h_freq)))
        abs_mag = magnitudes[h_idx]
        h_mags_abs.append(abs_mag)
        if fund_mag > 1e-9:
            h_i[str(h_freq)] = float(abs_mag / fund_mag)
        else:
            h_i[str(h_freq)] = 0.0

    # ── THD_i ─────────────────────────────────────────────────────
    if fund_mag > 1e-9:
        thd_i = float(np.sqrt(sum(m ** 2 for m in h_mags_abs)) / fund_mag)
    else:
        thd_i = 0.0

    # ── Write back (only overwrite zeros) ─────────────────────────
    if frame.get("crest_i", 0) == 0:
        frame["crest_i"] = round(crest_i, 6)

    if frame.get("thd_i", 0) == 0:
        frame["thd_i"] = round(thd_i, 6)

    if frame.get("i1_rms", 0) == 0:
        frame["i1_rms"] = round(i1_rms, 6)

    existing_h = frame.get("h_i", {})
    if all(existing_h.get(str(f), 0) == 0 for f in harmonic_freqs):
        frame["h_i"] = {k: round(v, 6) for k, v in h_i.items()}

    # ── Also patch p1 and s1 if zero ──────────────────────────────
    if frame.get("p1", 0) == 0 and i1_rms > 0:
        v1_rms = frame.get("rms_v", 120.0)  # approximate
        frame["p1"] = round(float(v1_rms * i1_rms * frame.get("pf_true", 1.0)), 4)

    if frame.get("s1", 0) == 0 and i1_rms > 0:
        v1_rms = frame.get("rms_v", 120.0)
        frame["s1"] = round(float(v1_rms * i1_rms), 4)

    # Ensure power features are non-negative
    for field in ["p", "s", "p1", "s1", "q1"]:
        if field in frame and frame[field] is not None:
            frame[field] = abs(frame[field])    

    return frame


# ---------------------------------------------------------------------------
# 2.  DataFrame-level derived features  
# ---------------------------------------------------------------------------

def add_derived_features(df: pd.DataFrame) -> pd.DataFrame:
    """
    Create columns that help separate:
      - Fan        (inductive, moderate harmonics)
      - Laptop     (SMPS, HIGH harmonics, low power)
      - Hairdryer  (resistive, low harmonics, high power)

    Call AFTER patch_zero_features has been applied to every row.
    """
    # ── Reactive Power ────────────────────────────────────────────
    # Fan draws significant reactive power; laptop and hairdryer do not
    if "s" in df.columns and "p" in df.columns:
        ap = df["s"].astype(float).fillna(0.0)
        rp = df["p"].astype(float).fillna(0.0)
        df["Reactive_Power"] = np.sqrt(np.clip(ap ** 2 - rp ** 2, 0, None))

    # ── THD ratio (redundant safety — should already be patched) ──
    harmonic_cols = [c for c in df.columns if c in ("h_i_120", "h_i_180", "h_i_240", "h_i_300")]
    if harmonic_cols:
        h_sum = df[harmonic_cols].astype(float).fillna(0.0).sum(axis=1)
        rms = df.get("rms_i", pd.Series(np.zeros(len(df)))).astype(float).fillna(0.0)
        df["THD_Ratio"] = h_sum / (rms + 1e-9)

    # ── Power-Factor × Power interaction ──────────────────────────
    # Amplifies the PF difference between fan (~0.7) and laptop (~0.55)
    if "pf_true" in df.columns and "p" in df.columns:
        df["PF_x_Power"] = df["pf_true"].astype(float) * df["p"].astype(float)

    return df


# ---------------------------------------------------------------------------
# 3.  Safe predict_proba helper
# ---------------------------------------------------------------------------

def get_positive_proba(proba_array: np.ndarray) -> np.ndarray:
    """
    Safely extract P(class=1) from a predict_proba result.

    HistGradientBoostingClassifier returns a SINGLE column when it only
    saw one class during training.  Indexing [:, 1] crashes with IndexError.
    """
    if proba_array.ndim == 1:
        return proba_array
    if proba_array.shape[1] == 1:
        # Model only saw class 0 → P(class=1) ≈ 0
        return np.zeros(proba_array.shape[0])
    return proba_array[:, 1]