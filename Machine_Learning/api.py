import json
import logging
from pathlib import Path

import joblib
import pandas as pd
from flask import Flask, request, jsonify
from sklearn.multioutput import MultiOutputClassifier

from src.config import MODEL_FEATURES, MODEL_CLASSES, DEFAULT_MODEL_PATH
from src.feature_patch import patch_zero_features, add_derived_features, get_positive_proba

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)
app = Flask(__name__)

MODEL_PATH = Path(DEFAULT_MODEL_PATH)
THRESHOLDS_PATH = Path("models") / "thresholds_multi.json"

IS_MULTI = False
model = None
thresholds = {}

logger.info("Loading model from %s...", MODEL_PATH)
model = joblib.load(MODEL_PATH)
IS_MULTI = isinstance(model, MultiOutputClassifier)
logger.info("Model loaded. IS_MULTI=%s", IS_MULTI)

if THRESHOLDS_PATH.exists():
    try:
        thresholds = json.loads(THRESHOLDS_PATH.read_text())
        logger.info("Loaded thresholds from %s", THRESHOLDS_PATH)
    except Exception:
        logger.exception("Failed to load thresholds file; falling back to defaults")
        thresholds = {}
else:
    logger.warning("Thresholds file not found at %s; using default 0.5", THRESHOLDS_PATH)
    thresholds = {}


def _safe_float(x, default=0.0):
    try:
        return float(x)
    except (TypeError, ValueError):
        return default


def _compute_var_p(p: float, s: float) -> float:
    if s <= 0 or abs(p) > s:
        return 0.0
    val = s * s - p * p
    return float(val ** 0.5) if val > 0 else 0.0


def _patch_rows(raw_rows):
    patched = []
    for x in raw_rows:
        frame = dict(x)
        frame = patch_zero_features(frame)

        # flatten h_i -> model columns
        h_i = frame.get("h_i", {})
        if isinstance(h_i, dict):
            frame["h2_i_norm"] = h_i.get("120", frame.get("h2_i_norm", 0.0))
            frame["h3_i_norm"] = h_i.get("180", frame.get("h3_i_norm", 0.0))
            frame["h4_i_norm"] = h_i.get("240", frame.get("h4_i_norm", 0.0))
            frame["h5_i_norm"] = h_i.get("300", frame.get("h5_i_norm", 0.0))

        patched.append(frame)
    return patched


def _preprocess_df(df: pd.DataFrame) -> pd.DataFrame:
    df = df.copy()

    if "p" not in df.columns:
        df["p"] = 0.0
    if "pf_true" not in df.columns:
        df["pf_true"] = 1.0
    if "rms_i" not in df.columns:
        df["rms_i"] = 0.0
    if "crest_i" not in df.columns:
        df["crest_i"] = 0.0
    if "rms_v" not in df.columns:
        df["rms_v"] = 120.0

    df["p"] = df["p"].apply(lambda x: abs(_safe_float(x, 0.0)))
    df["pf_true"] = df["pf_true"].apply(lambda x: abs(_safe_float(x, 1.0)))

    if "s" in df.columns:
        df["s"] = df["s"].apply(lambda x: abs(_safe_float(x, 0.0)))
    else:
        def compute_s(row):
            p = _safe_float(row.get("p", 0.0))
            pf_true = abs(_safe_float(row.get("pf_true", 1.0), 1.0))
            return float(p / pf_true) if pf_true != 0 else 0.0
        df["s"] = df.apply(compute_s, axis=1)

    if "var_p" not in df.columns:
        df["var_p"] = df.apply(
            lambda row: _compute_var_p(
                _safe_float(row.get("p", 0.0)),
                abs(_safe_float(row.get("s", 0.0))),
            ),
            axis=1,
        )

    for col in ["h2_i_norm", "h3_i_norm", "h4_i_norm", "h5_i_norm"]:
        if col not in df.columns:
            df[col] = 0.0

    if "thd_i" not in df.columns:
        def approx_thd(row):
            numsq = 0.0
            for k in [2, 3, 4, 5]:
                val = _safe_float(row.get(f"h{k}_i_norm", 0.0))
                numsq += val * val
            return float(numsq ** 0.5)
        df["thd_i"] = df.apply(approx_thd, axis=1)

    # keep training/inference feature engineering aligned
    df = add_derived_features(df)

    for col in MODEL_FEATURES:
        if col not in df.columns:
            df[col] = 0.0

    df[MODEL_FEATURES] = df[MODEL_FEATURES].astype(float).fillna(0.0)
    return df[MODEL_FEATURES]


def _predict_multilabel(df: pd.DataFrame):
    proba_list = model.predict_proba(df)
    safe_probs = [get_positive_proba(p) for p in proba_list]

    preds = []
    for row_idx in range(len(df)):
        active = []
        for class_idx, class_name in enumerate(MODEL_CLASSES):
            positive_prob = float(safe_probs[class_idx][row_idx])
            threshold = float(thresholds.get(class_name, 0.5))
            if positive_prob >= threshold:
                active.append(class_name)
        preds.append(active)

    return preds


@app.route("/health", methods=["GET"])
def health():
    return jsonify(
        {
            "status": "ok",
            "is_multi": IS_MULTI,
            "model_path": str(MODEL_PATH),
            "classes": MODEL_CLASSES,
            "thresholds_path": str(THRESHOLDS_PATH),
        }
    ), 200


@app.route("/predict", methods=["POST"])
def predict():
    try:
        payload = request.get_json(force=True, silent=False)
        logger.info("Incoming payload: %s", payload)

        if isinstance(payload, dict) and "instances" in payload:
            raw_rows = payload["instances"]
        elif isinstance(payload, dict):
            raw_rows = [payload]
        else:
            return jsonify({"error": "Invalid JSON format"}), 400

        patched_rows = _patch_rows(raw_rows)
        df_raw = pd.DataFrame(patched_rows)
        df = _preprocess_df(df_raw)

        if len(raw_rows) == 1:
            measured_p = float(df.iloc[0]["p"])
            measured_rms_i = float(df.iloc[0].get("rms_i", 0.0))
            if measured_p < 10.0 and measured_rms_i < 0.05:
                return jsonify({"appliance_predictions": ["Off"]})

        if IS_MULTI:
            preds = _predict_multilabel(df)

            if len(raw_rows) == 1:
                if len(preds[0]) == 0:
                    return jsonify({"appliance_predictions": ["Off"]})
                return jsonify({"appliance_predictions": preds[0]})

            return jsonify(
                {
                    "predictions": [
                        pred if len(pred) > 0 else ["Off"]
                        for pred in preds
                    ]
                }
            )

        pred = model.predict(df)
        res = [str(x) for x in pred]

        if len(raw_rows) == 1:
            return jsonify({"appliance_predictions": [res[0]]})
        return jsonify({"predictions": [[x] for x in res]})

    except Exception as exc:
        logger.exception("Error during prediction")
        return jsonify({"error": str(exc)}), 500


# if __name__ == "__main__":
#     app.run(host="0.0.0.0", port=5001, debug=True)