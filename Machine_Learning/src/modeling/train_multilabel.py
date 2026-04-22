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

# Scikit-learn imports for modern gradient boosting
from sklearn.ensemble import HistGradientBoostingClassifier
from sklearn.metrics import f1_score, classification_report, multilabel_confusion_matrix, accuracy_score
from sklearn.model_selection import train_test_split
from sklearn.multioutput import MultiOutputClassifier

# Shared configuration (ensures training uses same feature list as the API)
from src.config import MODEL_FEATURES, MODEL_CLASSES

# Setup logging to track training progress
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s",
)
logger = logging.getLogger(__name__)


def load_multilabel_data(
    data_path: Path, features: List[str], classes: List[str]
) -> Tuple[pd.DataFrame, np.ndarray]:
    """
    Load samples from a JSON file and convert labels into a multi-hot format.
    
    Args:
        data_path: Path to the .json dataset.
        features: List of column names to use as inputs (X).
        classes: List of all possible appliance names.

    Returns:
        X: DataFrame of features.
        Y: Binary matrix (n_samples x n_classes) where 1 means the appliance is ON.
    """
    with open(data_path, "r") as f:
        data = json.load(f)

    df = pd.DataFrame(data)

    if "labels" not in df.columns:
        raise ValueError("Expected a 'labels' column in the input data")

    # --- Feature Engineering & Safety ---
    # Ensure every expected feature exists (fill with 0.0 if missing)
    for col in features:
        if col not in df.columns:
            df[col] = 0.0
    
    # Convert to float to ensure compatibility with scikit-learn
    df[features] = df[features].astype(float).fillna(0.0)

    # --- Label Encoding (Multi-Hot) ---
    # Convert list of strings ["Fridge", "Lamp"] -> [0, 1, 0, 1, ...]
    # Y has one column per appliance class.
    Y = np.zeros((len(df), len(classes)), dtype=int)
    for idx, row_labels in enumerate(df["labels"]):
        if not isinstance(row_labels, list):
            continue
        for lbl in row_labels:
            if lbl in classes:
                j = classes.index(lbl)
                Y[idx, j] = 1  # Set the bit for this appliance

    X = df[features]
    return X, Y


def tune_per_class_thresholds(
    model: MultiOutputClassifier,
    X_val: pd.DataFrame,
    Y_val: np.ndarray,
    classes: List[str],
    t_min: float = 0.2,
    t_max: float = 0.8,
    n_steps: int = 13,
) -> Dict[str, float]:
    """
    Find the optimal probability threshold for EACH appliance class.
    
    Why: Some appliances (like Fans) are 'quiet' and need a low threshold (e.g. 0.3).
         Others (like Vacuums) are loud and distinctive, so we can demand high confidence (e.g. 0.7).
         Using a flat 0.5 threshold for everything is suboptimal.
    """
    logger.info("Tuning per-class thresholds on validation set...")
    thresholds: Dict[str, float] = {}
    
    # Create a grid of thresholds to test: [0.2, 0.25, 0.3 ... 0.8]
    t_grid = np.linspace(t_min, t_max, n_steps)

    # Get raw probability scores for the validation set
    # model.predict_proba returns a list of arrays, one for each class
    proba_list = model.predict_proba(X_val)

    for j, cls in enumerate(classes):
        y_true = Y_val[:, j]
        # Extract probability of "Positive" (class 1)
        probs = proba_list[j][:, 1]

        # Skip tuning if this appliance never appears in validation set
        if np.sum(y_true) == 0:
            thresholds[cls] = 0.5
            continue

        best_f1 = -1.0
        best_t = 0.5
        
        # Grid Search: Try every threshold and pick the one with best F1 score
        for t in t_grid:
            y_pred = (probs >= t).astype(int)
            f1 = f1_score(y_true, y_pred, zero_division=0)
            if f1 > best_f1:
                best_f1 = f1
                best_t = t
        
        thresholds[cls] = float(best_t)
        logger.info("Class '%s': best_t=%.3f, F1=%.3f", cls, best_t, best_f1)

    return thresholds


def save_confusion_matrix_plot(
    mcm: np.ndarray, classes: List[str], output_path: Path
) -> None:
    """
    Generates a grid of 2x2 Confusion Matrices (one per appliance).
    
    Visualizes:
      - True Positives (Hit)
      - True Negatives (Correct Rejection)
      - False Positives (Ghost/Hallucination)
      - False Negatives (Miss)
    """
    n_classes = len(classes)
    cols = 4
    rows = (n_classes + cols - 1) // cols
    
    fig, axes = plt.subplots(rows, cols, figsize=(cols * 4, rows * 3.5))
    axes = axes.flatten()

    for i, cls in enumerate(classes):
        # mcm[i] is a 2x2 matrix: [[TN, FP], [FN, TP]]
        cm = mcm[i]
        
        # Calculate accuracy just for this specific appliance
        tn, fp, fn, tp = cm.ravel()
        total = np.sum(cm)
        
        # Create a heatmap for this appliance
        sns.heatmap(
            cm, annot=True, fmt="d", cmap="Blues", cbar=False,
            xticklabels=["OFF", "ON"], yticklabels=["OFF", "ON"],
            ax=axes[i]
        )
        axes[i].set_title(f"{cls}\n(Acc: {(tp+tn)/total:.2f})")
        axes[i].set_ylabel("True")
        axes[i].set_xlabel("Pred")

    # Turn off empty subplots if we have them (e.g. 11 classes in a 4x3 grid)
    for j in range(i + 1, len(axes)):
        axes[j].axis("off")

    plt.tight_layout()
    plt.savefig(output_path)
    plt.close()
    logger.info("Confusion matrix plot saved to %s", output_path)


def main() -> None:
    parser = argparse.ArgumentParser(description="Train Multi-label Model")
    parser.add_argument("--data_path", type=Path, required=True)
    parser.add_argument("--model_output_path", type=Path, default=Path("models/appliance_model_multi.joblib"))
    parser.add_argument("--test_size", type=float, default=0.2, help="Fraction of data for final testing")
    parser.add_argument("--val_size", type=float, default=0.2, help="Fraction of training data for threshold tuning")
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args()

    # 1. Load Data
    X, Y = load_multilabel_data(args.data_path, MODEL_FEATURES, MODEL_CLASSES)
    logger.info("Loaded %d samples, %d features, %d classes", X.shape[0], X.shape[1], Y.shape[1])

    # 2. Split Data (Train / Validation / Test)
    # First split: separate out the final Test set
    X_train_full, X_test, Y_train_full, Y_test = train_test_split(
        X, Y, test_size=args.test_size, random_state=args.seed, shuffle=True
    )
    # Second split: separate Train into Train/Val (Val is used for threshold tuning)
    X_train, X_val, Y_train, Y_val = train_test_split(
        X_train_full, Y_train_full, test_size=args.val_size, random_state=args.seed, shuffle=True
    )

    # 3. Define Model Architecture
    # We use HistGradientBoosting because it is optimized for dense numerical features (Power, PF, Harmonics).
    # It handles non-linear relationships better than Random Forest for this data.
    logger.info("Training MultiOutput HistGradientBoostingClassifier...")
    base_model = HistGradientBoostingClassifier(
        max_iter=500,              # Maximum number of trees
        learning_rate=0.1,         # Step size
        max_depth=None,            # Allow trees to grow deep (limited by leaf nodes)
        l2_regularization=1.0,     # Prevents overfitting on noise
        early_stopping=True,       # Stop if validation score doesn't improve
        scoring='loss',
        validation_fraction=0.1,
        n_iter_no_change=10,
        random_state=args.seed
    )
    
    # MultiOutputClassifier wraps the base model to handle multiple labels (Y is a matrix, not a vector)
    model = MultiOutputClassifier(base_model, n_jobs=-1)
    model.fit(X_train, Y_train)

    # 4. Tune Thresholds
    # Calculate the best probability cutoff for each appliance using the Validation set
    thresholds = tune_per_class_thresholds(model, X_val, Y_val, MODEL_CLASSES)
    
    # Save thresholds to JSON so the API can load them later
    thresholds_path = args.model_output_path.parent / "thresholds_multi.json"
    thresholds_path.parent.mkdir(parents=True, exist_ok=True)
    with open(thresholds_path, "w") as f:
        json.dump(thresholds, f, indent=2)
    logger.info("Saved thresholds to %s", thresholds_path)

    # 5. Evaluate on Test Set (Using Tuned Thresholds)
    Y_pred_tuned = np.zeros_like(Y_test)
    proba_list_test = model.predict_proba(X_test)
    
    # Apply the learned thresholds to the test predictions
    for j, cls in enumerate(MODEL_CLASSES):
        probs = proba_list_test[j][:, 1]
        t = thresholds.get(cls, 0.5)
        Y_pred_tuned[:, j] = (probs >= t).astype(int)

    # Calculate final metrics
    # F1 Macro: Average reliability across all appliances
    tuned_f1_macro = f1_score(Y_test, Y_pred_tuned, average="macro", zero_division=0)
    # Subset Accuracy: Percentage of frames where ALL appliances were predicted correctly
    tuned_subset_acc = accuracy_score(Y_test, Y_pred_tuned)
    logger.info("TEST subset_acc=%.4f | F1_macro=%.4f", tuned_subset_acc, tuned_f1_macro)

    # 6. Visualize Results (Confusion Matrices)
    mcm = multilabel_confusion_matrix(Y_test, Y_pred_tuned)
    
    # Save PNG Plot
    cm_plot_path = args.model_output_path.parent / "confusion_matrix_multi.png"
    save_confusion_matrix_plot(mcm, MODEL_CLASSES, cm_plot_path)
    
    # Save Raw Stats to JSON (useful for programmatic checks)
    cm_data = {}
    for i, cls in enumerate(MODEL_CLASSES):
        tn, fp, fn, tp = mcm[i].ravel()
        cm_data[cls] = {
            "tn": int(tn), "fp": int(fp), 
            "fn": int(fn), "tp": int(tp),
            "precision": float(tp / (tp + fp)) if (tp+fp) > 0 else 0.0,
            "recall": float(tp / (tp + fn)) if (tp+fn) > 0 else 0.0
        }
    
    cm_json_path = args.model_output_path.parent / "confusion_matrix_multi.json"
    with open(cm_json_path, "w") as f:
        json.dump(cm_data, f, indent=2)
    logger.info("Confusion matrix JSON saved to %s", cm_json_path)

    # 7. Save Final Artifacts
    # Save the trained model
    joblib.dump(model, args.model_output_path)
    logger.info("Model saved to %s", args.model_output_path)
    
    # Save text classification report
    report = classification_report(Y_test, Y_pred_tuned, target_names=MODEL_CLASSES, zero_division=0, output_dict=True)
    with open(args.model_output_path.parent / "classification_report_multi.json", "w") as f:
        json.dump(report, f, indent=2)
        
    # Save summary metrics
    with open(args.model_output_path.parent / "metrics_summary_multi.json", "w") as f:
        json.dump({
            "subset_accuracy": float(tuned_subset_acc),
            "f1_macro": float(tuned_f1_macro),
            "n_samples": int(X.shape[0]),
            "classes": MODEL_CLASSES
        }, f, indent=2)


if __name__ == "__main__":
    main()