import sys
from pathlib import Path

# --- FIX: Add project root to path so we can import src ---
# This finds the folder one level up from "tools" (which is "Machine_Learning")
root_dir = Path(__file__).resolve().parents[1]
if str(root_dir) not in sys.path:
    sys.path.append(str(root_dir))

import matplotlib.pyplot as plt
import joblib
import pandas as pd
import numpy as np
from sklearn.metrics import precision_recall_curve

# Now this import will work
from src.config import MODEL_FEATURES, MODEL_CLASSES

def plot_tradeoff(model_path, data_path, target_class="Heater"):
    """
    Generates a Precision-Recall vs Threshold curve for a specific appliance.
    This visualizes why we pick a specific threshold (e.g. 0.25 for Heater).
    """
    # 1. Load Data & Model
    print(f"Loading model from {model_path}...")
    model = joblib.load(model_path)
    
    print(f"Loading data from {data_path}...")
    df = pd.read_json(data_path)
    
    # 2. Prepare Features
    X = df[MODEL_FEATURES]
    # Create binary ground truth for this specific class
    y_true = df["labels"].apply(lambda l: 1 if target_class in l else 0)
    
    # 3. Get Probabilities
    if target_class not in MODEL_CLASSES:
        print(f"Error: '{target_class}' not found in model classes: {MODEL_CLASSES}")
        return

    class_idx = MODEL_CLASSES.index(target_class)
    # predict_proba returns a list of arrays (one for each class)
    # We want the array for our class, and the probability of "1" (index 1)
    y_scores = model.predict_proba(X)[class_idx][:, 1]
    
    # 4. Calculate Curves
    precisions, recalls, thresholds = precision_recall_curve(y_true, y_scores)
    
    # 5. Plot
    plt.figure(figsize=(10, 6))
    # Thresholds array is 1 shorter than precision/recall arrays
    plt.plot(thresholds, precisions[:-1], "b--", label="Precision (Trustworthiness)")
    plt.plot(thresholds, recalls[:-1], "g-", label="Recall (Catch Rate)")
    
    plt.xlabel("Threshold Probability")
    plt.ylabel("Score")
    plt.title(f"Tuning Trade-off for {target_class}")
    plt.legend(loc="center left")
    plt.grid(True)
    
    # Save
    output_file = f"threshold_curve_{target_class}.png"
    plt.savefig(output_file)
    print(f"Successfully saved plot to {output_file}")

if __name__ == "__main__":
    # You can change "Heater" to "Microwave", "Fan", etc.
    plot_tradeoff(
        "models/appliance_model_multi.joblib",
        "data/processed/appliance_features_multilabel_sim.json",
        target_class="Heater" 
    )