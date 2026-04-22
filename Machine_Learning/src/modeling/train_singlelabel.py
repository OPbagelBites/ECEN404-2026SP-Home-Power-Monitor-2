import argparse
import json
import logging
from pathlib import Path
from typing import List, Tuple

import joblib
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns

from sklearn.ensemble import HistGradientBoostingClassifier
from sklearn.metrics import (
    accuracy_score,
    classification_report,
    confusion_matrix,
    f1_score,
)
from sklearn.model_selection import train_test_split

from src.config import MODEL_FEATURES, TARGET_LABEL, SINGLE_LABEL_CLASSES

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s",
)
logger = logging.getLogger(__name__)


def load_singlelabel_data(
    data_path: Path,
    features: List[str],
    target_label: str,
    allowed_classes: List[str],
) -> Tuple[pd.DataFrame, pd.Series]:
    """
    Load single-label appliance data from JSON.

    Accepts rows shaped like:
      {
        "appliance_type": "Fan",
        "p": ...,
        ...
      }

    or, if needed, rows with:
      {
        "labels": ["Fan"],
        ...
      }

    Filters rows to only the allowed single-label classes.
    """
    with open(data_path, "r") as f:
        data = json.load(f)

    df = pd.DataFrame(data)

    # Support either appliance_type or labels[0]
    if target_label not in df.columns:
        if "labels" in df.columns:
            df[target_label] = df["labels"].apply(
                lambda x: x[0] if isinstance(x, list) and len(x) > 0 else None
            )
        else:
            raise ValueError(
                f"Expected '{target_label}' or 'labels' in the input data"
            )

    # Drop unlabeled rows
    df = df[df[target_label].notna()].copy()
    df[target_label] = df[target_label].astype(str)

    # Keep only in-scope classes
    df = df[df[target_label].isin(allowed_classes)].copy()

    if df.empty:
        raise ValueError("No rows left after filtering to SINGLE_LABEL_CLASSES")

    # Ensure all expected features exist
    for col in features:
        if col not in df.columns:
            df[col] = 0.0

    df[features] = df[features].astype(float).fillna(0.0)

    X = df[features]
    y = df[target_label]

    return X, y


def save_confusion_matrix_plot(
    cm: np.ndarray,
    class_names: List[str],
    output_path: Path,
) -> None:
    plt.figure(figsize=(10, 8))
    sns.heatmap(
        cm,
        annot=True,
        fmt="d",
        cmap="Blues",
        xticklabels=class_names,
        yticklabels=class_names,
        cbar=False,
    )
    plt.title("Single-Label Confusion Matrix")
    plt.ylabel("True Label")
    plt.xlabel("Predicted Label")
    plt.tight_layout()
    plt.savefig(output_path)
    plt.close()
    logger.info("Confusion matrix plot saved to %s", output_path)


def main() -> None:
    parser = argparse.ArgumentParser(description="Train Single-Label Appliance Model")
    parser.add_argument("--data_path", type=Path, required=True)
    parser.add_argument(
        "--model_output_path",
        type=Path,
        default=Path("models/appliance_model_single.joblib"),
    )
    parser.add_argument(
        "--test_size",
        type=float,
        default=0.2,
        help="Fraction of data for final testing",
    )
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args()

    # 1. Load data
    X, y = load_singlelabel_data(
        args.data_path,
        MODEL_FEATURES,
        TARGET_LABEL,
        SINGLE_LABEL_CLASSES,
    )
    class_names = SINGLE_LABEL_CLASSES

    logger.info(
        "Loaded %d samples, %d features, %d classes",
        X.shape[0],
        X.shape[1],
        len(class_names),
    )
    logger.info("Classes: %s", class_names)

    # 2. Split data
    X_train, X_test, y_train, y_test = train_test_split(
        X,
        y,
        test_size=args.test_size,
        random_state=args.seed,
        shuffle=True,
        stratify=y,
    )

    logger.info(
        "Split: train=%d, test=%d",
        len(X_train),
        len(X_test),
    )

    # 3. Train model
    logger.info("Training single-label HistGradientBoostingClassifier...")
    model = HistGradientBoostingClassifier(
        max_iter=500,
        learning_rate=0.1,
        max_depth=None,
        l2_regularization=1.0,
        early_stopping=True,
        validation_fraction=0.1,
        n_iter_no_change=10,
        random_state=args.seed,
    )
    model.fit(X_train, y_train)

    # 4. Evaluate
    y_pred = model.predict(X_test)

    acc = accuracy_score(y_test, y_pred)
    f1_macro = f1_score(y_test, y_pred, average="macro", zero_division=0)

    logger.info("TEST accuracy=%.4f | F1_macro=%.4f", acc, f1_macro)

    # 5. Confusion matrix
    cm = confusion_matrix(y_test, y_pred, labels=class_names)

    cm_plot_path = args.model_output_path.parent / "confusion_matrix_single.png"
    save_confusion_matrix_plot(cm, class_names, cm_plot_path)

    cm_json = {
        "labels": class_names,
        "matrix": cm.tolist(),
    }
    cm_json_path = args.model_output_path.parent / "confusion_matrix_single.json"
    cm_json_path.parent.mkdir(parents=True, exist_ok=True)
    with open(cm_json_path, "w") as f:
        json.dump(cm_json, f, indent=2)
    logger.info("Confusion matrix JSON saved to %s", cm_json_path)

    # 6. Classification report
    report = classification_report(
        y_test,
        y_pred,
        labels=class_names,
        target_names=class_names,
        zero_division=0,
        output_dict=True,
    )
    report_path = args.model_output_path.parent / "classification_report_single.json"
    with open(report_path, "w") as f:
        json.dump(report, f, indent=2)
    logger.info("Classification report saved to %s", report_path)

    # 7. Metrics summary
    metrics_summary = {
        "accuracy": float(acc),
        "f1_macro": float(f1_macro),
        "n_samples": int(X.shape[0]),
        "n_classes": int(len(class_names)),
        "classes": class_names,
        "features": MODEL_FEATURES,
        "target_label": TARGET_LABEL,
    }
    metrics_path = args.model_output_path.parent / "metrics_summary_single.json"
    with open(metrics_path, "w") as f:
        json.dump(metrics_summary, f, indent=2)
    logger.info("Metrics summary saved to %s", metrics_path)

    # 8. Save model
    args.model_output_path.parent.mkdir(parents=True, exist_ok=True)
    joblib.dump(model, args.model_output_path)
    logger.info("Model saved to %s", args.model_output_path)


if __name__ == "__main__":
    main()