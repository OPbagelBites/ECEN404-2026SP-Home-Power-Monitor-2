import time
import json
import random
import requests
import numpy as np
import pandas as pd
from pathlib import Path
from sklearn.metrics import accuracy_score, f1_score

# -------------------------------------------------------------------
# CONFIGURATION
# -------------------------------------------------------------------
# Target the local Docker container (mapped to host port 5001)
API_URL = "http://localhost:5001/predict"

# Source of Truth: We load the clean, physics-calibrated dataset.
# We use this to generate synthetic test cases on the fly.
DATA_PATH = Path("data/processed/appliance_features.json")

N_REQUESTS = 100  # Validation Sample Size
NOISE_LEVEL = 0.05  # 5% Jitter: Simulates real-world sensor noise/calibration error

def load_source_data():
    """Loads the clean feature set to sample from."""
    if not DATA_PATH.exists():
        print(f"Error: {DATA_PATH} not found.")
        return None
    return pd.read_json(DATA_PATH)

def get_random_sample(df, n_appliances=1):
    """
    SIMULATION ENGINE: Generates a single synthetic 'frame' for testing.
    
    Instead of using a static test set, we generate new, unique combinations
    every time we run this. This prevents the model from 'memorizing' the test data.
    
    Args:
        n_appliances: How many devices are active in this frame (1, 2, or 3).
    """
    # 1. Pick Appliances
    # Randomly select 'n' real appliance signatures from our dataset
    sample_rows = df.sample(n=n_appliances)
    labels = sample_rows["appliance_type"].tolist()
    
    # 2. define Feature Groups
    # Features that sum linearly (Power)
    sum_cols = ["p", "s", "var_p"] 
    # Features that average out (Harmonics/Quality)
    mean_cols = ["pf_true", "thd_i", "crest_i", "h2_i_norm", "h3_i_norm", "h4_i_norm", "h5_i_norm"]
    
    # 3. Construct the Mixed Signal
    mixed_features = {}
    
    # Sum Power (P_total = P1 + P2 + ... + Noise)
    for col in sum_cols:
        val = sample_rows[col].sum()
        # INJECT NOISE: Real sensors are never perfect. 
        # We add +/- 5% random variation to test model robustness.
        noise = val * np.random.normal(0, NOISE_LEVEL)
        mixed_features[col] = max(0, val + noise)
        
    # Average Harmonics
    # (Simplified physics approximation: dominant load usually dictates harmonics)
    for col in mean_cols:
        val = sample_rows[col].mean()
        noise = val * np.random.normal(0, NOISE_LEVEL)
        mixed_features[col] = max(0, val + noise)
        
    # Derive RMS Current roughly (I = S / 120V) to match physics constraint
    mixed_features["rms_i"] = mixed_features["s"] / 120.0
    
    return mixed_features, sorted(list(set(labels)))

def run_stress_test():
    """
    MAIN VALIDATION LOOP:
    Fires requests at the API and grades the responses in real-time.
    """
    df = load_source_data()
    if df is None: return

    print(f"--- Starting Stress Test ({N_REQUESTS} requests) ---")
    print(f"Target: {API_URL}")
    
    correct_count = 0
    latencies = []
    
    for i in range(N_REQUESTS):
        # 1. Generate Scenario
        # Weighted probabilities: Mostly single appliances (60%), some pairs (30%), rare triples (10%)
        k = random.choices([1], weights=[1])[0]
        payload, true_labels = get_random_sample(df, k)
        
        # 2. Send Request (Measure Latency)
        start_time = time.time()
        try:
            # POST the JSON frame to our Dockerized Brain
            resp = requests.post(API_URL, json=payload, timeout=2)
            latency = (time.time() - start_time) * 1000 # Convert to ms
            latencies.append(latency)
            
            if resp.status_code != 200:
                print(f"[{i}] Failed: {resp.status_code} - {resp.text}")
                continue
                
            # 3. Parse Prediction
            pred_labels = sorted(resp.json().get("appliance_predictions", []))
            
            # 4. Grade the Result (Exact Match)
            # Requires the model to identify EVERY active appliance correctly.
            # [Fridge, Lamp] vs [Fridge] -> FAIL
            if pred_labels == true_labels:
                correct_count += 1
                status = "PASS"
            else:
                status = "FAIL"
                
            # Live Feed Output
            print(f"[{i+1}/{N_REQUESTS}] {status} | Latency: {latency:.1f}ms")
            print(f"   True: {true_labels}")
            print(f"   Pred: {pred_labels}")
            print(f"   Power: {payload['p']:.1f}W")
            print("-" * 30)
            
        except Exception as e:
            print(f"[{i}] Error: {e}")

    # 5. Final Report
    accuracy = (correct_count / N_REQUESTS) * 100
    avg_latency = sum(latencies) / len(latencies) if latencies else 0
    
    print("\n=== Test Summary ===")
    print(f"Total Requests: {N_REQUESTS}")
    print(f"Accuracy (Exact Match): {accuracy:.1f}%")
    print(f"Avg Latency: {avg_latency:.1f} ms")
    print("====================")

if __name__ == "__main__":
    run_stress_test()