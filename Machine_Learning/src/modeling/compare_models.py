import argparse
import json
from typing import Optional
from pathlib import Path

import joblib
import numpy as np
import pandas as pd
from sklearn.metrics import accuracy_score, f1_score, classification_report

from src.config import MODEL_FEATURES, MODEL_CLASSES


def load_rows(path: str) -> list[dict]:
    data = json.loads(Path(path).read_text())
    if not isinstance(data, list):
        raise ValueError("Expected dataset JSON to be a list of rows.")
    return data


def build_xy(rows: list[dict]) -> tuple[pd.DataFrame, np.ndarray]:
    df = pd.DataFrame(rows).copy()

    for col in MODEL_FEATURES:
        if col not in df.columns:
            df[col] = 0.0

    X = df[MODEL_FEATURES].astype(float).fillna(0.0)

    # supports either "labels" list or single "appliance_type"
    Y = np.zeros((len(df), len(MODEL_CLASSES)), dtype=int)

    for i, row in enumerate(rows):
        labels = row.get("labels")
        if labels is None:
            appliance_type = row.get("appliance_type")
            labels = [appliance_type] if appliance_type else []

        for label in labels:
            if label in MODEL_CLASSES:
                Y[i, MODEL_CLASSES.index(label)] = 1

    return X, Y


def load_thresholds(path: Optional[str]) -> dict:
    if not path:
        return {}
    p = Path(path)
    if not p.exists():
        return {}
    return json.loads(p.read_text())


def predict_multilabel(model, X: pd.DataFrame, thresholds: dict) -> np.ndarray:
    proba_list = model.predict_proba(X)
    Y_pred = np.zeros((len(X), len(MODEL_CLASSES)), dtype=int)

    for class_idx, class_name in enumerate(MODEL_CLASSES):
        probs = proba_list[class_idx]

        if probs.ndim == 1:
            pos_prob = probs
        elif probs.shape[1] == 1:
            pos_prob = np.zeros(len(X))
        else:
            pos_prob = probs[:, 1]

        threshold = float(thresholds.get(class_name, 0.5))
        Y_pred[:, class_idx] = (pos_prob >= threshold).astype(int)

    return Y_pred


def summarize(name: str, Y_true: np.ndarray, Y_pred: np.ndarray) -> dict:
    subset_acc = accuracy_score(Y_true, Y_pred)
    f1_macro = f1_score(Y_true, Y_pred, average="macro", zero_division=0)
    print(f"\n=== {name} ===")
    print(f"Subset Accuracy: {subset_acc:.4f}")
    print(f"Macro F1:        {f1_macro:.4f}")

    per_class = classification_report(
        Y_true,
        Y_pred,
        target_names=MODEL_CLASSES,
        zero_division=0,
        output_dict=True,
    )
    return {
        "subset_accuracy": subset_acc,
        "f1_macro": f1_macro,
        "per_class": per_class,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data_path", required=True)
    ap.add_argument("--model_a", required=True)
    ap.add_argument("--model_b", required=True)
    ap.add_argument("--thresholds_a", default=None)
    ap.add_argument("--thresholds_b", default=None)
    ap.add_argument("--out", default="models/model_compare.json")
    args = ap.parse_args()

    rows = load_rows(args.data_path)
    X, Y_true = build_xy(rows)

    model_a = joblib.load(args.model_a)
    model_b = joblib.load(args.model_b)

    thresholds_a = load_thresholds(args.thresholds_a)
    thresholds_b = load_thresholds(args.thresholds_b)

    Y_pred_a = predict_multilabel(model_a, X, thresholds_a)
    Y_pred_b = predict_multilabel(model_b, X, thresholds_b)

    result_a = summarize("MODEL A", Y_true, Y_pred_a)
    result_b = summarize("MODEL B", Y_true, Y_pred_b)

    diff_rows = []
    for i in range(len(rows)):
        a_labels = [MODEL_CLASSES[j] for j in range(len(MODEL_CLASSES)) if Y_pred_a[i, j] == 1]
        b_labels = [MODEL_CLASSES[j] for j in range(len(MODEL_CLASSES)) if Y_pred_b[i, j] == 1]
        true_labels = [MODEL_CLASSES[j] for j in range(len(MODEL_CLASSES)) if Y_true[i, j] == 1]

        if a_labels != b_labels:
            diff_rows.append({
                "index": i,
                "event_id": rows[i].get("event_id", rows[i].get("frame_id", i)),
                "true": true_labels,
                "model_a": a_labels,
                "model_b": b_labels,
            })

    output = {
        "model_a": args.model_a,
        "model_b": args.model_b,
        "metrics_a": result_a,
        "metrics_b": result_b,
        "prediction_differences": diff_rows[:100],  # cap for readability
    }

    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_text(json.dumps(output, indent=2))
    print(f"\nSaved comparison report to {args.out}")


if __name__ == "__main__":
    main()