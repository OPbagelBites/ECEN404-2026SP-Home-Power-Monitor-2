"""
Central configuration for the Home Power Monitor machine-learning pipeline.

This module defines constants that are shared across training and inference,
including the ordered list of feature names expected by the models.
"""

from typing import List

# Ordered list of feature names used by both the training script and the API.
# These must match what the ESP / DSP pipeline computes per frame.
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

# ---------------------------------------------------------------------------
# Label configuration
# ---------------------------------------------------------------------------

# Default name of the target column in the *single-label* processed dataset
# (e.g. PLAID-derived features). Used by the existing RandomForest trainer.
TARGET_LABEL: str = "appliance_type"

# Multi-label configuration: ordered list of appliance classes.
# These names must match:
#   1) The appliance names in your ESP TEST_MODE PROFILES[]
#   2) The labels that appear in the "labels" array in Firebase frames
MODEL_CLASSES: List[str] = [
    "Air Conditioner",
    "Compact Fluorescent Lamp",
    "Fan",
    "Fridge",
    "Hairdryer",
    "Heater",
    "Incandescent Light Bulb",
    "Laptop",
    "Microwave",
    "Vacuum",
    "Washing Machine",
]

# Path to the trained model file relative to the project root.
# The multi-label training script can also save to this path so the API
# continues to load models from a single well-known location.
DEFAULT_MODEL_PATH: str = "models/appliance_model_rf.joblib"
