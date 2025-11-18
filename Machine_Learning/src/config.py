"""
Central configuration for the Home Power Monitor machine‑learning pipeline.

This module defines constants that are shared across training and inference,
including the ordered list of feature names expected by the models.
"""

from typing import List

# Ordered list of feature names used by both the training script and the API.
MODEL_FEATURES: List[str] = [
    "p",
    "s",
    "pf_true",
    "rms_i",
    "thd_i",
    "crest_i",
    "h2_i_norm",
    "h3_i_norm",
    "h4_i_norm",
    "h5_i_norm",
]

# Default name of the target column in the processed dataset.
TARGET_LABEL: str = "appliance_type"

# Path to the trained model file relative to the project root.  This path is
# used by the API to locate the tuned model produced by the training script.
DEFAULT_MODEL_PATH: str = "models/appliance_model_rf.joblib"
