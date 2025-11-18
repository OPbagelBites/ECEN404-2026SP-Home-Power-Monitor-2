# src/modeling/train.py
import argparse
import json
import logging
from pathlib import Path
from typing import Tuple, List, Dict, Optional

import joblib
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns

from sklearn.base import clone
from sklearn.calibration import CalibratedClassifierCV
from sklearn.ensemble import RandomForestClassifier, ExtraTreesClassifier
from sklearn.metrics import (
    accuracy_score,
    classification_report,
    confusion_matrix,
    f1_score,
)
from sklearn.model_selection import (
    StratifiedKFold,
    RandomizedSearchCV,
    train_test_split,
    GroupShuffleSplit,
)

from src.config import MODEL_FEATURES, TARGET_LABEL, DEFAULT_MODEL_PATH


# ----------------------------
# Utilities
# ----------------------------
def load_data(
    data_path: Path,
    features: List[str],
    target: str,
    group_column: Optional[str] = None,
) -> Tuple[pd.DataFrame, pd.Series, Optional[pd.Series]]:
    with open(data_path, "r") as f:
        data = json.load(f)
    df = pd.DataFrame(data)

    # target + features hygiene
    df = df.dropna(subset=[target]).reset_index(drop=True)
    for c in features:
        if c not in df.columns:
            df[c] = 0.0
    df[features] = df[features].astype(float).fillna(0.0)

    X = df[features]
    y = df[target].astype(str)
    groups = df[group_column].astype(str) if group_column and group_column in df.columns else None
    return X, y, groups


def save_feature_stats(X_train: pd.DataFrame, out_path: Path) -> None:
    stats = pd.DataFrame({"mean": X_train.mean(), "std": X_train.std(ddof=0)})
    out_path.parent.mkdir(parents=True, exist_ok=True)
    stats.to_json(out_path, orient="columns")


def plot_confusion(cm: np.ndarray, labels: List[str], out_path: Path) -> None:
    plt.figure(figsize=(10, 8))
    sns.heatmap(cm, annot=False, fmt="d", cmap="Blues",
                xticklabels=labels, yticklabels=labels)
    plt.xlabel("Predicted")
    plt.ylabel("True")
    plt.tight_layout()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    plt.savefig(out_path, dpi=150)
    plt.close()


def _make_calibrator(base_clf, method: str):
    """Handle sklearn API change (estimator vs base_estimator)."""
    try:
        return CalibratedClassifierCV(estimator=base_clf, method=method, cv=5)  # sklearn >=1.1
    except TypeError:
        return CalibratedClassifierCV(base_estimator=base_clf, method=method, cv=5)  # sklearn <1.1


def calibrate_model(base_clf, X: pd.DataFrame, y: pd.Series):
    """Isotonic with fallback to sigmoid."""
    cal = _make_calibrator(base_clf, "isotonic")
    try:
        cal.fit(X, y)
        return cal, "isotonic"
    except ValueError as e:
        logging.getLogger(__name__).warning("Isotonic failed (%s). Falling back to sigmoid.", e)
        cal2 = _make_calibrator(base_clf, "sigmoid")
        cal2.fit(X, y)
        return cal2, "sigmoid"


# ----------------------------
# Hyperparameter search
# ----------------------------
def tune_rf(
    X: pd.DataFrame, y: pd.Series, n_iter: int, seed: int
) -> Tuple[RandomForestClassifier, Dict, float]:
    param_dist = {
        "n_estimators": [300, 500, 700, 900],
        "max_depth": [10, 12, 14, 16, 18, 20, None],
        "min_samples_split": [2, 5, 10],
        "min_samples_leaf": [1, 2, 4],
        "max_features": ["sqrt", "log2", 0.3, 0.5],
        "bootstrap": [True, False],
        "class_weight": [None, "balanced"],
    }
    rf = RandomForestClassifier(random_state=seed, n_jobs=-1)
    cv = StratifiedKFold(n_splits=5, shuffle=True, random_state=seed)
    search = RandomizedSearchCV(
        rf,
        param_distributions=param_dist,
        n_iter=n_iter,
        cv=cv,
        scoring="f1_macro",
        n_jobs=-1,
        verbose=1,
        random_state=seed,
        refit=True,
    )
    search.fit(X, y)
    return search.best_estimator_, search.best_params_, float(search.best_score_)


def tune_extratrees(
    X: pd.DataFrame, y: pd.Series, n_iter: int, seed: int
) -> Tuple[ExtraTreesClassifier, Dict, float]:
    param_dist = {
        "n_estimators": [300, 500, 700, 900],
        "max_depth": [10, 12, 14, 16, 18, 20, None],
        "min_samples_split": [2, 5, 10],
        "min_samples_leaf": [1, 2, 4],
        "max_features": ["sqrt", "log2", 0.3, 0.5],
        "bootstrap": [False],
        "class_weight": [None, "balanced"],
    }
    et = ExtraTreesClassifier(random_state=seed, n_jobs=-1)
    cv = StratifiedKFold(n_splits=5, shuffle=True, random_state=seed)
    search = RandomizedSearchCV(
        et,
        param_distributions=param_dist,
        n_iter=n_iter,
        cv=cv,
        scoring="f1_macro",
        n_jobs=-1,
        verbose=1,
        random_state=seed,
        refit=True,
    )
    search.fit(X, y)
    return search.best_estimator_, search.best_params_, float(search.best_score_)


# ----------------------------
# OOF probabilities & thresholds
# ----------------------------
def compute_oof_probas_calibrated(
    estimator, X: pd.DataFrame, y: pd.Series, seed: int
) -> Tuple[np.ndarray, List[str]]:
    """
    Compute out-of-fold probabilities with calibration inside each fold.
    Returns:
        proba_oof: shape (n_samples, n_classes)
        classes: class order used
    """
    n = len(X)
    proba_oof = None
    classes_: Optional[List[str]] = None

    cv = StratifiedKFold(n_splits=5, shuffle=True, random_state=seed)
    for train_idx, val_idx in cv.split(X, y):
        X_tr, X_va = X.iloc[train_idx], X.iloc[val_idx]
        y_tr = y.iloc[train_idx]

        est = clone(estimator)
        est.fit(X_tr, y_tr)
        cal, _ = calibrate_model(est, X_tr, y_tr)

        pr = cal.predict_proba(X_va)
        if proba_oof is None:
            proba_oof = np.zeros((n, pr.shape[1]), dtype=float)
            classes_ = list(cal.classes_)
        proba_oof[val_idx] = pr

    assert proba_oof is not None and classes_ is not None
    return proba_oof, classes_


def find_per_class_thresholds(
    y_true: pd.Series,
    proba: np.ndarray,
    classes: List[str],
    grid: np.ndarray,
    unknown_label: str = "Unknown",
) -> Dict[str, float]:
    thresholds: Dict[str, float] = {}
    for idx, cls in enumerate(classes):
        best_t, best_f1 = 0.60, -1.0
        p_cls = proba[:, idx]
        argmax = proba.argmax(axis=1)
        for t in grid:
            preds = []
            for i in range(len(p_cls)):
                if argmax[i] == idx and p_cls[i] >= t:
                    preds.append(cls)
                else:
                    preds.append(unknown_label)
            f1 = f1_score(y_true, preds, labels=classes, average="macro", zero_division=0)
            if f1 > best_f1:
                best_f1, best_t = f1, float(t)
        thresholds[cls] = best_t
    return thresholds


def apply_thresholds(
    proba: np.ndarray, classes: List[str], thresholds: Dict[str, float], unknown_label: str = "Unknown"
) -> List[str]:
    argmax = proba.argmax(axis=1)
    maxp = proba.max(axis=1)
    out = []
    for i, k in enumerate(argmax):
        label = classes[k]
        thr = thresholds.get(label, 0.60)
        out.append(label if maxp[i] >= thr else unknown_label)
    return out


# ----------------------------
# Main
# ----------------------------
def main() -> None:
    parser = argparse.ArgumentParser(
        description="Train tuned (and calibrated) appliance classifier with OOF thresholds and optional ensemble"
    )
    parser.add_argument("--data_path", type=Path, required=True, help="Path to JSON features.")
    parser.add_argument("--model_output_path", type=Path, default=Path(DEFAULT_MODEL_PATH), help="Model output path (.joblib).")
    parser.add_argument("--target", type=str, default=TARGET_LABEL, help="Target column.")
    parser.add_argument("--group-column", type=str, default=None, help="Optional group column (e.g., house/session) for test split.")
    parser.add_argument("--test_size", type=float, default=0.20, help="Test fraction.")
    parser.add_argument("--seed", type=int, default=42, help="Random seed.")
    parser.add_argument("--n_iter", type=int, default=60, help="RandomizedSearchCV iterations.")
    parser.add_argument("--try-extratrees", action="store_true", help="Also tune ExtraTrees and compare.")
    parser.add_argument("--ensemble", action="store_true", help="If ET tried, build RF+ET probability ensemble and save both models + ensemble.json.")
    parser.add_argument("--no_calibration", action="store_true", help="Disable probability calibration.")
    parser.add_argument("--save_importances", action="store_true", help="Save feature_importances.csv if available.")
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    log = logging.getLogger(__name__)

    # Load
    X, y, groups = load_data(args.data_path, MODEL_FEATURES, args.target, args.group_column)
    log.info("Loaded %d samples, %d features", len(X), len(MODEL_FEATURES))

    # Test split (group-aware if available)
    if groups is not None:
        gss = GroupShuffleSplit(n_splits=1, test_size=args.test_size, random_state=args.seed)
        tr_idx, te_idx = next(gss.split(X, y, groups=groups))
        X_train_full, X_test = X.iloc[tr_idx], X.iloc[te_idx]
        y_train_full, y_test = y.iloc[tr_idx], y.iloc[te_idx]
        log.info("Group split on '%s' used. Train_full=%d, Test=%d", args.group_column, len(X_train_full), len(X_test))
    else:
        X_train_full, X_test, y_train_full, y_test = train_test_split(
            X, y, test_size=args.test_size, stratify=y, random_state=args.seed
        )
        log.info("Random stratified split. Train_full=%d, Test=%d", len(X_train_full), len(X_test))

    # Feature stats for API OOD checks
    feature_stats_path = Path("models/feature_stats.json")
    save_feature_stats(X_train_full, feature_stats_path)
    log.info("Saved feature stats to %s", feature_stats_path)

    # HPO (on train_full)
    rf_best, rf_params, rf_cv = tune_rf(X_train_full, y_train_full, n_iter=args.n_iter, seed=args.seed)
    log.info("RF best params: %s | CV f1_macro=%.4f", rf_params, rf_cv)

    et_best = None
    if args.try_extratrees:
        et_best, et_params, et_cv = tune_extratrees(X_train_full, y_train_full, n_iter=args.n_iter, seed=args.seed)
        log.info("ET best params: %s | CV f1_macro=%.4f", et_params, et_cv)

    # Candidate(s)
    candidates = [("rf", rf_best)]
    if et_best is not None:
        candidates.append(("et", et_best))

    # Calibrate candidate(s) on train_full (or leave raw if disabled)
    calibrated: Dict[str, object] = {}
    cal_method: Dict[str, str] = {}
    for name, est in candidates:
        if args.no_calibration:
            calibrated[name] = est.fit(X_train_full, y_train_full)
            cal_method[name] = "none"
            log.info("%s: calibration disabled.", name.upper())
        else:
            est.fit(X_train_full, y_train_full)
            cal, m = calibrate_model(est, X_train_full, y_train_full)
            calibrated[name] = cal
            cal_method[name] = m
            log.info("%s: applied %s calibration.", name.upper(), m)

    # Choose champion by test hard F1 of the single models (no thresholds yet)
    scores = []
    for name, mdl in calibrated.items():
        y_pred = mdl.predict(X_test)
        f1m = f1_score(y_test, y_pred, average="macro")
        scores.append((name, f1m))
        log.info("%s TEST (hard) F1_macro: %.4f", name.upper(), f1m)

    champion_name, _ = max(scores, key=lambda x: x[1])
    model = calibrated[champion_name]
    log.info("Champion: %s", champion_name.upper())

    # Optional: build ensemble if requested and ET available
    ensemble_used = False
    ensemble_weights = [0.5, 0.5]
    if args.ensemble and "rf" in calibrated and "et" in calibrated:
        ensemble_used = True
        log.info("Ensemble enabled: RF+ET (avg probs).")

        # Evaluate ensemble on test
        pr_rf = calibrated["rf"].predict_proba(X_test)
        pr_et = calibrated["et"].predict_proba(X_test)

        # Align class order just in case
        classes_rf = list(calibrated["rf"].classes_)
        classes_et = list(calibrated["et"].classes_)
        if classes_rf != classes_et:
            # map ET probs to RF class order
            idx_map = [classes_et.index(c) for c in classes_rf]
            pr_et = pr_et[:, idx_map]
            classes = classes_rf
        else:
            classes = classes_rf

        pr_ens = ensemble_weights[0] * pr_rf + ensemble_weights[1] * pr_et
        y_pred_ens = [classes[i] for i in pr_ens.argmax(axis=1)]
        f1_ens = f1_score(y_test, y_pred_ens, average="macro")
        log.info("ENSEMBLE TEST (hard) F1_macro: %.4f", f1_ens)

        # Use ensemble for subsequent thresholding/evaluation if it wins
        if f1_ens >= max(s for _, s in scores):
            champion_name = "ensemble"
            model = None  # ensemble is meta; not a single sklearn object
            log.info("Champion: ENSEMBLE (RF+ET)")

    # OOF calibrated probabilities on train_full (using the CHAMPION recipe)
    # We need an estimator spec to compute OOF; choose according to champion_name
    if champion_name == "ensemble":
        # OOF for ensemble = average of OOF probs of RF and ET with calibration in each fold
        proba_oof_rf, classes_rf = compute_oof_probas_calibrated(rf_best, X_train_full, y_train_full, args.seed)
        proba_oof_et, classes_et = compute_oof_probas_calibrated(et_best, X_train_full, y_train_full, args.seed)

        # align ET to RF class order
        if classes_rf != classes_et:
            idx_map = [classes_et.index(c) for c in classes_rf]
            proba_oof_et = proba_oof_et[:, idx_map]
        proba_oof = ensemble_weights[0] * proba_oof_rf + ensemble_weights[1] * proba_oof_et
        classes = classes_rf
    else:
        base_estimator = rf_best if champion_name == "rf" else et_best
        proba_oof, classes = compute_oof_probas_calibrated(base_estimator, X_train_full, y_train_full, args.seed)

    # Per-class thresholds from OOF probs
    thr_grid = np.linspace(0.50, 0.70, 17)  # gentler band usually helps macro-F1
    thresholds = find_per_class_thresholds(y_train_full, proba_oof, classes, thr_grid)
    thresholds_path = Path("models/thresholds.json")
    thresholds_path.parent.mkdir(parents=True, exist_ok=True)
    with open(thresholds_path, "w") as f:
        json.dump(thresholds, f, indent=2)
    log.info("Saved per-class thresholds to %s", thresholds_path)

    # ---- Final evaluation on TEST ----
    def predict_proba_any(Xd: pd.DataFrame) -> Tuple[np.ndarray, List[str]]:
        if champion_name == "ensemble":
            pr_rf = calibrated["rf"].predict_proba(Xd)
            pr_et = calibrated["et"].predict_proba(Xd)
            # align class order
            classes_rf = list(calibrated["rf"].classes_)
            classes_et = list(calibrated["et"].classes_)
            if classes_rf != classes_et:
                idx_map = [classes_et.index(c) for c in classes_rf]
                pr_et = pr_et[:, idx_map]
            return ensemble_weights[0] * pr_rf + ensemble_weights[1] * pr_et, classes_rf
        else:
            pr = calibrated[champion_name].predict_proba(Xd) if cal_method.get(champion_name, "none") != "none" or not args.no_calibration else model.predict_proba(Xd)
            cls = list(calibrated[champion_name].classes_) if champion_name in calibrated else list(model.classes_)  # fallback
            return pr, cls

    # Hard labels (no Unknown)
    if champion_name == "ensemble":
        y_pred_hard = y_pred_ens  # already computed above
    else:
        y_pred_hard = calibrated[champion_name].predict(X_test)
    acc_hard = accuracy_score(y_test, y_pred_hard)
    f1_hard = f1_score(y_test, y_pred_hard, average="macro")
    log.info("TEST (hard)  Acc: %.4f | F1_macro: %.4f", acc_hard, f1_hard)

    # Thresholded predictions (Unknown allowed)
    proba_test, cls_order = predict_proba_any(X_test)
    if cls_order != classes:
        # align to thresholds class order
        idx_map = [cls_order.index(c) for c in classes]
        proba_test = proba_test[:, idx_map]
    y_pred_thr = apply_thresholds(proba_test, classes, thresholds, unknown_label="Unknown")
    f1_thr = f1_score(y_test, y_pred_thr, labels=classes, average="macro", zero_division=0)
    log.info("TEST (thresholded) F1_macro: %.4f", f1_thr)

    # ---- Save artifacts ----
    out_dir = Path(args.model_output_path).parent
    out_dir.mkdir(parents=True, exist_ok=True)

    # Save primary model
    if champion_name == "ensemble":
        # Save both base models and an ensemble manifest
        rf_path = out_dir / "appliance_model_rf.joblib"
        et_path = out_dir / "appliance_model_et.joblib"
        joblib.dump(calibrated["rf"], rf_path)
        joblib.dump(calibrated["et"], et_path)
        ensemble_meta = {
            "models": [str(rf_path.name), str(et_path.name)],
            "weights": ensemble_weights,
            "classes": classes,
        }
        with open(out_dir / "ensemble.json", "w") as f:
            json.dump(ensemble_meta, f, indent=2)
        # Also write the expected DEFAULT_MODEL_PATH for backward compat (use RF)
        joblib.dump(calibrated["rf"], Path(args.model_output_path))
    else:
        joblib.dump(model, Path(args.model_output_path))
    log.info("Model artifact(s) saved. Primary path: %s", args.model_output_path)

    # Reports
    report = classification_report(y_test, y_pred_hard, output_dict=True)
    with open(out_dir / "classification_report.json", "w") as f:
        json.dump(report, f, indent=2)
    cm = confusion_matrix(y_test, y_pred_hard, labels=classes)
    plot_confusion(cm, classes, out_dir / "confusion_matrix.png")

    with open(out_dir / "classes.json", "w") as f:
        json.dump({"classes": classes}, f, indent=2)

    # Optional: feature importances (only for single-tree ensembles)
    if args.save_importances and champion_name != "ensemble" and hasattr(model, "feature_importances_"):
        imp = pd.DataFrame({"feature": MODEL_FEATURES, "importance": model.feature_importances_})
        imp.sort_values("importance", ascending=False).to_csv(out_dir / "feature_importances.csv", index=False)

    # Metrics summary
    summary = {
        "champion": champion_name,
        "calibration": None if args.no_calibration else cal_method.get(champion_name, "mixed" if champion_name == "ensemble" else "none"),
        "test_accuracy_hard": acc_hard,
        "test_f1_macro_hard": f1_hard,
        "test_f1_macro_thresholded": f1_thr,
        "classes": classes,
        "model_path": str(Path(args.model_output_path)),
        "thresholds_path": str((Path("models") / "thresholds.json").resolve()),
        "feature_stats_path": str((Path("models") / "feature_stats.json").resolve()),
        "ensemble_used": ensemble_used,
    }
    with open(out_dir / "metrics_summary.json", "w") as f:
        json.dump(summary, f, indent=2)
    log.info("Metrics summary saved to %s", out_dir / "metrics_summary.json")


if __name__ == "__main__":
    main()
