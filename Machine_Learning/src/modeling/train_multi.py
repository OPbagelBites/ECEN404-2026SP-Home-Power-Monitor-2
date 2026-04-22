import argparse
import json
import logging
from pathlib import Path
from typing import List, Tuple, Dict

import joblib
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns

from sklearn.base import clone                          
from sklearn.ensemble import HistGradientBoostingClassifier
from sklearn.metrics import (
    f1_score,
    classification_report,
    multilabel_confusion_matrix,
    accuracy_score,
)
from sklearn.model_selection import train_test_split
from sklearn.multioutput import MultiOutputClassifier

from src.config import MODEL_FEATURES, MODEL_CLASSES

# PATCH: import the shared feature extraction module
from src.feature_patch import (
    patch_zero_features,
    add_derived_features,
    get_positive_proba,
)

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s",
)
logger = logging.getLogger(__name__)


# ======================================================================
# DATA LOADING
# ======================================================================

def load_multilabel_data(
    data_path: Path, features: List[str], classes: List[str]
) -> Tuple[pd.DataFrame, np.ndarray]:
    """
    Load samples from a JSON file and convert labels into a multi-hot format.
    """
    with open(data_path, "r") as f:
        data = json.load(f)

    # ── Handle both flat list and nested Firebase export ──────────
    # PATCH: Firebase exports are nested dicts; flatten if needed
    if isinstance(data, dict) and "labels" not in data:
        flat = []
        for frame in _flatten_firebase(data):
            flat.append(frame)
        data = flat

    if isinstance(data, list):
        df = pd.DataFrame(data)
    else:
        df = pd.DataFrame([data])

    if "labels" not in df.columns:
        raise ValueError("Expected a 'labels' column in the input data")

    # ── PATCH: recompute zeros from raw samples row-by-row ────────
    logger.info("Patching zero-valued harmonic features from raw samples...")
    patched_rows = []
    for _, row in df.iterrows():
        frame = row.to_dict()
        frame = patch_zero_features(frame)

        # Flatten nested h_i dict into columns: h_i_120, h_i_180, ...
        h_i = frame.pop("h_i", {})
        if isinstance(h_i, dict):
            for freq_key, val in h_i.items():
                frame[f"h_i_{freq_key}"] = val

        # Flatten nested dbg if present (we only needed i_samp for patching)
        frame.pop("dbg", None)
        frame.pop("ml", None)

        patched_rows.append(frame)

    df = pd.DataFrame(patched_rows)

    # ── PATCH: add derived features ───────────────────────────────
    df = add_derived_features(df)

    # ── PATCH: O(1) label lookup instead of O(n) ─────────────────
    class_to_idx = {cls: i for i, cls in enumerate(classes)}

    for col in features:
        if col not in df.columns:
            df[col] = 0.0

    df[features] = df[features].astype(float).fillna(0.0)

    Y = np.zeros((len(df), len(classes)), dtype=int)
    for idx, row_labels in enumerate(df["labels"]):
        if not isinstance(row_labels, list):
            continue
        for lbl in row_labels:
            if lbl in class_to_idx:
                Y[idx, class_to_idx[lbl]] = 1

    X = df[features]
    return X, Y


def _flatten_firebase(data: dict) -> list:
    """
    Recursively walk a nested Firebase RTDB export and yield
    every dict that looks like a frame (has 'rms_i' or 'p' key).
    """
    results = []
    if isinstance(data, dict):
        if "rms_i" in data or "p" in data:
            results.append(data)
        else:
            for v in data.values():
                results.extend(_flatten_firebase(v))
    elif isinstance(data, list):
        for item in data:
            results.extend(_flatten_firebase(item))
    return results


# ======================================================================
# SAMPLE WEIGHTING
# ======================================================================

# PATCH: inverse-frequency weighting per binary column
def _compute_sample_weights(y_col: np.ndarray) -> np.ndarray:
    """Inverse-frequency weights so rare appliances get equal importance."""
    n = len(y_col)
    n_pos = int(y_col.sum())
    n_neg = n - n_pos
    if n_pos == 0 or n_neg == 0:
        return np.ones(n)
    return np.where(y_col == 1, n / (2.0 * n_pos), n / (2.0 * n_neg))


# ======================================================================
# THRESHOLD TUNING
# ======================================================================

def tune_per_class_thresholds(
    model: MultiOutputClassifier,
    X_val: pd.DataFrame,
    Y_val: np.ndarray,
    classes: List[str],
    t_min: float = 0.10,       # PATCH: lowered from 0.20
    t_max: float = 0.85,       # PATCH: raised from 0.80
    n_steps: int = 31,         # PATCH: finer grid from 13
) -> Dict[str, float]:
    """
    Find the optimal probability threshold for EACH appliance class.
    """
    logger.info("Tuning per-class thresholds on validation set...")
    thresholds: Dict[str, float] = {}

    t_grid = np.linspace(t_min, t_max, n_steps)
    proba_list = model.predict_proba(X_val)

    for j, cls in enumerate(classes):
        y_true = Y_val[:, j]
        # PATCH: safe extraction — prevents crash when class was absent
        probs = get_positive_proba(proba_list[j])

        if np.sum(y_true) == 0:
            thresholds[cls] = 0.5
            logger.warning("  Class '%s' has 0 positives in val set — defaulting to 0.5", cls)
            continue

        best_f1 = -1.0
        best_t = 0.5

        for t in t_grid:
            y_pred = (probs >= t).astype(int)
            f1 = f1_score(y_true, y_pred, zero_division=0)
            if f1 > best_f1:
                best_f1 = f1
                best_t = t

        thresholds[cls] = float(best_t)
        logger.info("  Class '%s': best_t=%.3f, F1=%.3f", cls, best_t, best_f1)

    return thresholds


# ======================================================================
# VISUALIZATION
# ======================================================================

def save_confusion_matrix_plot(
    mcm: np.ndarray, classes: List[str], output_path: Path
) -> None:
    n_classes = len(classes)
    cols = 4
    rows = (n_classes + cols - 1) // cols

    fig, axes = plt.subplots(rows, cols, figsize=(cols * 4, rows * 3.5))
    axes = axes.flatten() if n_classes > 1 else [axes]

    for i, cls in enumerate(classes):
        cm = mcm[i]
        tn, fp, fn, tp = cm.ravel()
        total = np.sum(cm)
        sns.heatmap(
            cm, annot=True, fmt="d", cmap="Blues", cbar=False,
            xticklabels=["OFF", "ON"], yticklabels=["OFF", "ON"],
            ax=axes[i],
        )
        axes[i].set_title(f"{cls}\n(Acc: {(tp + tn) / total:.2f})")
        axes[i].set_ylabel("True")
        axes[i].set_xlabel("Pred")

    for j in range(i + 1, len(axes)):
        axes[j].axis("off")

    plt.tight_layout()
    plt.savefig(output_path)
    plt.close()
    logger.info("Confusion matrix plot saved to %s", output_path)


# ======================================================================
# MAIN
# ======================================================================

def main() -> None:
    parser = argparse.ArgumentParser(description="Train Multi-label Model")
    parser.add_argument("--data_path", type=Path, required=True)
    parser.add_argument(
        "--model_output_path",
        type=Path,
        default=Path("models/appliance_model_multi.joblib"),
    )
    parser.add_argument("--test_size", type=float, default=0.2)
    parser.add_argument("--val_size", type=float, default=0.2)
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args()

    # ── 1. Load Data ──────────────────────────────────────────────
    X, Y = load_multilabel_data(args.data_path, MODEL_FEATURES, MODEL_CLASSES)
    logger.info(
        "Loaded %d samples, %d features, %d classes",
        X.shape[0], X.shape[1], Y.shape[1],
    )

    # PATCH: log per-class counts so you can spot problems immediately
    for j, cls in enumerate(MODEL_CLASSES):
        logger.info("  %s — positive samples: %d / %d", cls, int(Y[:, j].sum()), len(Y))

        # ── 2. Split Data ─────────────────────────────────────────────
    X_train_full, X_test, Y_train_full, Y_test = train_test_split(
        X, Y, test_size=args.test_size, random_state=args.seed, shuffle=True
    )
    X_train, X_val, Y_train, Y_val = train_test_split(
        X_train_full, Y_train_full, test_size=args.val_size, random_state=args.seed, shuffle=True
    )

    # PATCH: verify every class appears in every split
    for j, cls in enumerate(MODEL_CLASSES):
        tr = Y_train[:, j].sum()
        va = Y_val[:, j].sum()
        te = Y_test[:, j].sum()
        logger.info("  %s — train:%d  val:%d  test:%d", cls, int(tr), int(va), int(te))
        if tr == 0 or va == 0 or te == 0:
            logger.warning("  ⚠ '%s' has zero positives in a split!", cls)

    # ── 3. Define Base Model ──────────────────────────────────────
    base_model = HistGradientBoostingClassifier(
        max_iter=500,
        learning_rate=0.1,
        max_depth=None,
        l2_regularization=1.0,
        early_stopping=True,
        scoring="loss",
        validation_fraction=0.1,
        n_iter_no_change=10,
        random_state=args.seed,
    )

    # ── 4. Train Per-Class With Sample Weighting ──────────────────
    logger.info("Training per-class estimators with sample weighting...")
    estimators = []
    for j, cls in enumerate(MODEL_CLASSES):
        est = clone(base_model)
        sw = _compute_sample_weights(Y_train[:, j])
        est.fit(X_train, Y_train[:, j], sample_weight=sw)
        logger.info(
            "  Trained '%s' (pos=%d, neg=%d)",
            cls, int(Y_train[:, j].sum()), int(len(Y_train) - Y_train[:, j].sum()),
        )
        estimators.append(est)

    model = MultiOutputClassifier(base_model)
    model.estimators_ = estimators
    model.classes_ = [np.array([0, 1]) for _ in range(Y_train.shape[1])]

    # ── 5. Tune Thresholds ────────────────────────────────────────
    thresholds = tune_per_class_thresholds(model, X_val, Y_val, MODEL_CLASSES)

    thresholds_path = args.model_output_path.parent / "thresholds_multi_B.json"
    thresholds_path.parent.mkdir(parents=True, exist_ok=True)
    with open(thresholds_path, "w") as f:
        json.dump(thresholds, f, indent=2)
    logger.info("Saved thresholds to %s", thresholds_path)

    # ── 6. Evaluate on Test Set ───────────────────────────────────
    Y_pred_tuned = np.zeros_like(Y_test)
    proba_list_test = model.predict_proba(X_test)

    for j, cls in enumerate(MODEL_CLASSES):
        probs = get_positive_proba(proba_list_test[j])
        t = thresholds.get(cls, 0.5)
        Y_pred_tuned[:, j] = (probs >= t).astype(int)

    tuned_f1_macro = f1_score(Y_test, Y_pred_tuned, average="macro", zero_division=0)
    tuned_subset_acc = accuracy_score(Y_test, Y_pred_tuned)
    logger.info("TEST subset_acc=%.4f | F1_macro=%.4f", tuned_subset_acc, tuned_f1_macro)

    # ── 7. Confusion Matrices ─────────────────────────────────────
    mcm = multilabel_confusion_matrix(Y_test, Y_pred_tuned)

    cm_plot_path = args.model_output_path.parent / "confusion_matrix_multi.png"
    save_confusion_matrix_plot(mcm, MODEL_CLASSES, cm_plot_path)

    cm_data = {}
    for i, cls in enumerate(MODEL_CLASSES):
        tn, fp, fn, tp = mcm[i].ravel()
        cm_data[cls] = {
            "tn": int(tn), "fp": int(fp),
            "fn": int(fn), "tp": int(tp),
            "precision": float(tp / (tp + fp)) if (tp + fp) > 0 else 0.0,
            "recall": float(tp / (tp + fn)) if (tp + fn) > 0 else 0.0,
        }
    cm_json_path = args.model_output_path.parent / "confusion_matrix_multi.json"
    with open(cm_json_path, "w") as f:
        json.dump(cm_data, f, indent=2)

    # ── 8. Save Artifacts ─────────────────────────────────────────
    joblib.dump(model, args.model_output_path)
    logger.info("Model saved to %s", args.model_output_path)

    report = classification_report(
        Y_test, Y_pred_tuned, target_names=MODEL_CLASSES, zero_division=0, output_dict=True
    )
    with open(args.model_output_path.parent / "classification_report_multi.json", "w") as f:
        json.dump(report, f, indent=2)

    with open(args.model_output_path.parent / "metrics_summary_multi.json", "w") as f:
        json.dump({
            "subset_accuracy": float(tuned_subset_acc),
            "f1_macro": float(tuned_f1_macro),
            "n_samples": int(X.shape[0]),
            "classes": MODEL_CLASSES,
        }, f, indent=2)

    logger.info("Done.")


if __name__ == "__main__":
    main()