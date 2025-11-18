import numpy as np
import pandas as pd
import json
from pathlib import Path

# --- Constants based on PLAID dataset description ---
SAMPLING_FREQUENCY = 30000  # 30 kHz
NOMINAL_VOLTAGE = 120.0     # 120V
NOMINAL_FREQUENCY = 60.0      # 60 Hz

# --- Label mapping from the .ts file header ---
APPLIANCE_MAP = {
    0: 'Air Conditioner',
    1: 'Compact Fluorescent Lamp',
    2: 'Fan',
    3: 'Fridge',
    4: 'Hairdryer',
    5: 'Heater',
    6: 'Incandescent Light Bulb',
    7: 'Laptop',
    8: 'Microwave',
    9: 'Vacuum',
    10: 'Washing Machine'
}

def calculate_features(current_waveform: np.ndarray) -> dict:
    """
    Calculates a feature vector (fingerprint) from a raw current waveform.
    Since voltage is not provided, an ideal 120V/60Hz sine wave is synthesized.
    """
    if len(current_waveform) == 0:
        return {}

    # 1. Synthesize an ideal voltage waveform
    num_samples = len(current_waveform)
    time = np.arange(num_samples) / SAMPLING_FREQUENCY
    voltage_waveform = NOMINAL_VOLTAGE * np.sqrt(2) * np.sin(2 * np.pi * NOMINAL_FREQUENCY * time)

    # 2. Calculate basic RMS values
    rms_i = np.sqrt(np.mean(current_waveform**2))
    rms_v = np.sqrt(np.mean(voltage_waveform**2))

    # 3. Calculate Power Metrics
    instantaneous_power = voltage_waveform * current_waveform
    active_power = np.mean(instantaneous_power)
    apparent_power = rms_v * rms_i
    power_factor = active_power / apparent_power if apparent_power > 0 else 0

    # 4. Calculate Waveform Shape Metrics
    crest_factor_i = np.max(np.abs(current_waveform)) / rms_i if rms_i > 0 else 0

    # 5. Calculate Harmonics (using FFT)
    fft_i = np.fft.fft(current_waveform)
    fft_freq = np.fft.fftfreq(num_samples, 1.0/SAMPLING_FREQUENCY)
    
    # Find the magnitude of the fundamental frequency (60 Hz)
    fundamental_mag_i = np.abs(fft_i[np.argmin(np.abs(fft_freq - NOMINAL_FREQUENCY))])

    harmonics_norm = {}
    total_harmonic_power_sq = 0

    # Calculate for 2nd to 5th harmonics
    for n in range(2, 6):
        harmonic_freq = NOMINAL_FREQUENCY * n
        mag = np.abs(fft_i[np.argmin(np.abs(fft_freq - harmonic_freq))])
        harmonics_norm[f'h{n}_i_norm'] = mag / fundamental_mag_i if fundamental_mag_i > 0 else 0
        total_harmonic_power_sq += mag**2

    # 6. Calculate Total Harmonic Distortion (THD)
    thd_i = np.sqrt(total_harmonic_power_sq) / fundamental_mag_i if fundamental_mag_i > 0 else 0

    return {
        "p": active_power,
        "s": apparent_power,
        "pf_true": power_factor,
        "rms_v": rms_v,
        "rms_i": rms_i,
        "thd_i": thd_i,
        "crest_i": crest_factor_i,
        **harmonics_norm
    }

def parse_ts_file(file_path: Path) -> list:
    """
    Parses a .ts file, extracts each time series and its label.
    """
    all_series = []
    with open(file_path, 'r') as f:
        for line in f:
            line = line.strip()
            # Skip header lines
            if not line or line.startswith(("#", "@")):
                continue

            try:
                data_str, label_str = line.rsplit(':', 1)
                series = np.array([float(v) for v in data_str.split(',') if v != 'NaN'])
                label = int(label_str)
                all_series.append({'series': series, 'label': label})
            except ValueError as e:
                print(f"Skipping malformed line: {line[:50]}...")
                
    return all_series

def main():
    """
    Main function to process PLAID .ts files and generate a single JSON feature file.
    """
    project_root = Path(__file__).parent.parent.parent
    data_raw_path = project_root / 'data' / 'raw'
    data_processed_path = project_root / 'data' / 'processed'
    data_processed_path.mkdir(exist_ok=True)

    train_file = data_raw_path / 'PLAID_TRAIN.ts'
    test_file = data_raw_path / 'PLAID_TEST.ts'
    output_file = data_processed_path / 'appliance_features.json'

    if not train_file.exists() or not test_file.exists():
        print(f"Error: Make sure '{train_file.name}' and '{test_file.name}' are in '{data_raw_path}'.")
        return

    print("Parsing train and test files...")
    train_data = parse_ts_file(train_file)
    test_data = parse_ts_file(test_file)
    all_data = train_data + test_data

    print(f"Found {len(all_data)} total appliance measurements. Extracting features...")
    
    all_features = []
    for i, item in enumerate(all_data):
        features = calculate_features(item['series'])
        if features:
            features['appliance_type'] = APPLIANCE_MAP.get(item['label'], 'Unknown')
            features['event_id'] = i
            all_features.append(features)

    print(f"Successfully extracted features for {len(all_features)} measurements.")

    print(f"Saving final features to {output_file}...")
    with open(output_file, 'w') as f_out:
        json.dump(all_features, f_out, indent=4)
        
    print("Processing complete!")


if __name__ == '__main__':
    main()

