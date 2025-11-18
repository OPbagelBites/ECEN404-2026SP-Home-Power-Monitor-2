import joblib
import logging
import pandas as pd
from flask import Flask, request, jsonify

# Import shared configuration.  The config module defines the ordered list of
# features expected by the model and the default path to the trained model.
from src.config import MODEL_FEATURES, DEFAULT_MODEL_PATH


"""
Flask API for the Home Power Monitor machine‑learning model.

This API exposes a single endpoint, `/predict`, which accepts a JSON object
containing values for each feature in `MODEL_FEATURES`.  The endpoint loads a
trained model from disk (falling back to a legacy model name if necessary),
ensures that all required features are present, and returns a predicted
appliance label as JSON.  Logging is used instead of print statements for
better integration with production systems.
"""

# Configure logging and initialise the Flask app
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)
app = Flask(__name__)

# Load the trained model from the configured path.  If the tuned model
# produced by the training script is not present, fall back to the legacy
# `appliance_model.joblib` file to maintain backwards compatibility.
logger.info("Loading model from %s...", DEFAULT_MODEL_PATH)
try:
    model = joblib.load(DEFAULT_MODEL_PATH)
    logging.getLogger(__name__).info("Classes: %s", getattr(model, "classes_", None))
    logging.getLogger(__name__).info("Feature order: %s", MODEL_FEATURES)
except FileNotFoundError:
    logger.warning(
        "Model file %s not found. Falling back to models/appliance_model.joblib.",
        DEFAULT_MODEL_PATH,
    )
    model = joblib.load("models/appliance_model.joblib")
logger.info("Model loaded successfully.")


@app.route('/predict', methods=['POST'])
def predict():
    """
    Accepts either:
      • Single sample: {p, s, pf_true, rms_i, thd_i, crest_i, h2_i_norm, h3_i_norm, h4_i_norm, h5_i_norm}
      • Batch: {"instances": [ { ... }, { ... }, ... ]}

    Optional query params:
      • proba=1   -> return top-k labels with probabilities instead of hard labels
      • topk=3    -> how many classes to return when proba=1
    """
    try:
        payload = request.get_json(force=True)
        if payload is None:
            return jsonify({"error": "No input data provided"}), 400

        # ---- options ----
        proba_flag = request.args.get("proba") == "1"
        try:
            topk = int(request.args.get("topk", 3))
        except ValueError:
            topk = 3

        # global threshold for abstaining; tune on validation (typical 0.55–0.70)
        UNKNOWN_THRESHOLD = 0.60

        # ---- helpers ----
        def _coerce_row(d: dict) -> dict:
            # ensure all MODEL_FEATURES exist and are numeric; fill missing with 0.0
            out = {}
            for k in MODEL_FEATURES:
                v = d.get(k, 0.0)
                try:
                    out[k] = float(v)
                except Exception:
                    out[k] = 0.0
            return out

        def _ranked(pred_proba):
            classes = list(getattr(model, "classes_", []))
            pairs = sorted(zip(classes, pred_proba), key=lambda x: x[1], reverse=True)
            return pairs[:topk]

        # ---- BATCH ----
        if isinstance(payload, dict) and "instances" in payload and isinstance(payload["instances"], list):
            rows = [_coerce_row(x) for x in payload["instances"]]
            df = pd.DataFrame(rows, columns=MODEL_FEATURES)

            if proba_flag and hasattr(model, "predict_proba"):
                probas = model.predict_proba(df)
                topk_out = [
                    [{"label": str(c), "p": float(p)} for c, p in _ranked(pr)]
                    for pr in probas
                ]
                return jsonify({"topk": topk_out})

            # hard labels with Unknown threshold if we have probabilities
            if hasattr(model, "predict_proba"):
                probas = model.predict_proba(df)
                preds = []
                max_ps = []
                for pr in probas:
                    pr = pr.astype(float)
                    max_p = float(pr.max())
                    label = str(model.classes_[pr.argmax()])
                    preds.append("Unknown" if max_p < UNKNOWN_THRESHOLD else label)
                    max_ps.append(max_p)
                return jsonify({"predictions": preds, "max_p": max_ps})

            # fallback: no predict_proba available
            preds = model.predict(df)
            return jsonify({"predictions": [str(p) for p in preds]})

        # ---- SINGLE ----
        if not isinstance(payload, dict):
            return jsonify({"error": "Invalid JSON format"}), 400

        row = _coerce_row(payload)
        df = pd.DataFrame([row], columns=MODEL_FEATURES)

        if proba_flag and hasattr(model, "predict_proba"):
            pr = model.predict_proba(df)[0].astype(float)
            topk_out = [{"label": str(c), "p": float(p)} for c, p in _ranked(pr)]
            return jsonify({"topk": topk_out})

        if hasattr(model, "predict_proba"):
            pr = model.predict_proba(df)[0].astype(float)
            max_p = float(pr.max())
            label = str(model.classes_[pr.argmax()])
            if max_p < UNKNOWN_THRESHOLD:
                return jsonify({"appliance_prediction": "Unknown", "max_p": max_p})
            return jsonify({"appliance_prediction": label, "max_p": max_p})

        # fallback if model has no predict_proba
        pred = model.predict(df)[0]
        return jsonify({"appliance_prediction": str(pred)})

    except Exception as exc:  # pragma: no cover
        logger.exception("Error during prediction")
        return jsonify({"error": str(exc)}), 500


# This allows the app to be run directly from the terminal for development
if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5001, debug=True)
