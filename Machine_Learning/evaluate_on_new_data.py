import numpy as np
import pandas as pd
import json
import joblib
from pathlib import Path
from sklearn.metrics import accuracy_score, classification_report

# --- Constants and Label Map (MUST MATCH your training script) ---
SAMPLING_FREQUENCY = 30000
NOMINAL_VOLTAGE = 120.0
NOMINAL_FREQUENCY = 60.0

APPLIANCE_MAP = {
    0: 'Air Conditioner', 1: 'Compact Fluorescent Lamp', 2: 'Fan',
    3: 'Fridge', 4: 'Hairdryer', 5: 'Heater', 6: 'Incandescent Light Bulb',
    7: 'Laptop', 8: 'Microwave', 9: 'Vacuum', 10: 'Washing Machine'
}

def calculate_features(current_waveform: np.ndarray) -> dict:
    """
    Calculates features from a raw current waveform.
    This function MUST be IDENTICAL to the one used in your data processing script.
    """
    if len(current_waveform) == 0:
        return {}

    num_samples = len(current_waveform)
    time = np.arange(num_samples) / SAMPLING_FREQUENCY
    voltage_waveform = NOMINAL_VOLTAGE * np.sqrt(2) * np.sin(2 * np.pi * NOMINAL_FREQUENCY * time)

    rms_i = np.sqrt(np.mean(current_waveform**2))
    rms_v = np.sqrt(np.mean(voltage_waveform**2))
    
    active_power = np.mean(voltage_waveform * current_waveform)
    apparent_power = rms_v * rms_i
    power_factor = active_power / apparent_power if apparent_power > 0 else 0
    crest_factor_i = np.max(np.abs(current_waveform)) / rms_i if rms_i > 0 else 0

    fft_i = np.fft.fft(current_waveform)
    fft_freq = np.fft.fftfreq(num_samples, 1.0/SAMPLING_FREQUENCY)
    fundamental_mag_i = np.abs(fft_i[np.argmin(np.abs(fft_freq - NOMINAL_FREQUENCY))])

    harmonics_norm = {}
    total_harmonic_power_sq = 0
    for n in range(2, 6):
        harmonic_freq = NOMINAL_FREQUENCY * n
        mag = np.abs(fft_i[np.argmin(np.abs(fft_freq - harmonic_freq))])
        harmonics_norm[f'h{n}_i_norm'] = mag / fundamental_mag_i if fundamental_mag_i > 0 else 0
        total_harmonic_power_sq += mag**2
    
    thd_i = np.sqrt(total_harmonic_power_sq) / fundamental_mag_i if fundamental_mag_i > 0 else 0

    return {
        "p": active_power, "s": apparent_power, "pf_true": power_factor,
        "rms_v": rms_v, "rms_i": rms_i, "thd_i": thd_i, "crest_i": crest_factor_i,
        **harmonics_norm
    }

def parse_ts_file(file_path: Path) -> list:
    """Parses a .ts file and returns a list of series and labels."""
    all_series = []
    with open(file_path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith(("#", "@")):
                continue
            try:
                data_str, label_str = line.rsplit(':', 1)
                series = np.array([float(v) for v in data_str.split(',') if v != 'NaN'])
                label = int(label_str)
                all_series.append({'series': series, 'label': label})
            except ValueError:
                print(f"Skipping malformed line: {line[:50]}...")
    return all_series

def evaluate_model(model_path: Path, new_data_path: Path):
    """
    Loads a pre-trained model and evaluates it on a new, unseen dataset.
    """
    if not model_path.exists():
        print(f"Error: Model file not found at {model_path}")
        return
    if not new_data_path.exists():
        print(f"Error: New data file not found at {new_data_path}")
        return
        
    # 1. Load the pre-trained model
    print(f"Loading pre-trained model from {model_path}...")
    model = joblib.load(model_path)
    print("Model loaded successfully.")

    # 2. Parse the new test data file
    print(f"Parsing new test data from {new_data_path}...")
    new_data = parse_ts_file(new_data_path)

    # 3. Process the new data to extract features (must be identical to training)
    print(f"Extracting features from {len(new_data)} new samples...")
    all_features = []
    true_labels = []
    for item in new_data:
        features = calculate_features(item['series'])
        if features:
            all_features.append(features)
            true_labels.append(APPLIANCE_MAP.get(item['label'], 'Unknown'))

    df_new_test = pd.DataFrame(all_features)
    
    # Define the feature order the model expects (must match training)
    model_features = [
        'p', 's', 'pf_true', 'rms_i', 'thd_i', 'crest_i',
        'h2_i_norm', 'h3_i_norm', 'h4_i_norm', 'h5_i_norm'
    ]
    df_new_test[model_features] = df_new_test[model_features].fillna(0)
    X_new_test = df_new_test[model_features]

    # 4. Make predictions on the new data
    print("Making predictions on the new test set...")
    predictions = model.predict(X_new_test)

    # 5. Generate and print the performance report
    print("\n--- Model Performance on New PLAID_eq_TEST.ts Data ---")
    accuracy = accuracy_score(true_labels, predictions)
    print(f"Accuracy: {accuracy:.4f}")
    
    print("\nClassification Report:")
    print(classification_report(true_labels, predictions))

if __name__ == '__main__':
    project_root = Path(__file__).parent
    # Path to your already trained model
    trained_model_path = project_root / 'models' / 'appliance_model_rf.joblib'
    # Path to the new test data
    new_test_data_path = project_root / 'data' / 'raw' / 'PLAID_TRAIN.ts'
    
    evaluate_model(trained_model_path, new_test_data_path)
