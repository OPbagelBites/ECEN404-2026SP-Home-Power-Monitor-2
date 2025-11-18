import pandas as pd
import numpy as np
import json
from pathlib import Path

# --- Constants ---
FUNDAMENTAL_FREQ = 60  # Hz, for North America
N_HARMONICS = 5        # How many harmonics to calculate (e.g., 2nd to 6th)

def extract_features_from_file(data_df: pd.DataFrame, meta_info: dict) -> dict:
    """
    Calculates a full electrical feature vector from raw current/voltage data.

    Args:
        data_df (pd.DataFrame): DataFrame with 'current' and 'voltage' columns.
        meta_info (dict): The metadata dictionary for this specific appliance.

    Returns:
        dict: A dictionary containing the calculated fingerprint and the appliance label.
    """
    # --- Basic Sanity Checks ---
    if data_df.empty:
        return None

    current = data_df['current'].values
    voltage = data_df['voltage'].values

    # --- Core Power Calculations ---
    rms_v = np.sqrt(np.mean(voltage**2))
    rms_i = np.sqrt(np.mean(current**2))

    if rms_i == 0 or rms_v == 0:
        return None # Skip files with no signal

    active_power = np.mean(voltage * current)
    apparent_power = rms_v * rms_i
    power_factor = active_power / apparent_power if apparent_power > 0 else 0

    # --- Waveform Shape Features ---
    crest_factor_i = np.max(np.abs(current)) / rms_i if rms_i > 0 else 0

    # --- Harmonic Analysis using FFT ---
    sampling_freq_str = meta_info['meta']['header']['sampling_frequency']
    fs = int(sampling_freq_str.replace('Hz', ''))
    
    # Perform FFT on the current waveform
    N = len(current)
    fft_vals = np.fft.rfft(current)
    fft_freqs = np.fft.rfftfreq(N, 1/fs)
    
    # Find magnitude of the fundamental frequency (60 Hz)
    fundamental_idx = np.argmin(np.abs(fft_freqs - FUNDAMENTAL_FREQ))
    fundamental_mag = np.abs(fft_vals[fundamental_idx])

    # Find magnitudes of the harmonics
    harmonic_mags = []
    for i in range(2, N_HARMONICS + 2):
        harmonic_freq = FUNDAMENTAL_FREQ * i
        harmonic_idx = np.argmin(np.abs(fft_freqs - harmonic_freq))
        harmonic_mags.append(np.abs(fft_vals[harmonic_idx]))

    # Calculate Total Harmonic Distortion (THD)
    sum_sq_harmonics = np.sum(np.square(harmonic_mags))
    thd_i = np.sqrt(sum_sq_harmonics) / fundamental_mag if fundamental_mag > 0 else 0

    # --- Assemble the Fingerprint Dictionary ---
    fingerprint = {
        'appliance_type': meta_info['meta']['appliance']['type'],
        'file_id': meta_info['id'],
        'active_power': active_power,
        'apparent_power': apparent_power,
        'power_factor': power_factor,
        'rms_v': rms_v,
        'rms_i': rms_i,
        'thd_i': thd_i,
        'crest_factor_i': crest_factor_i,
    }
    
    # Add individual harmonic magnitudes to the fingerprint
    for i, mag in enumerate(harmonic_mags, start=2):
        # Normalize by the fundamental for a more stable feature
        fingerprint[f'h{i}_i_norm'] = mag / fundamental_mag if fundamental_mag > 0 else 0
        
    return fingerprint


def process_plaid_data(raw_data_dir: Path, output_file_path: Path):
    """
    Main function to process the entire PLAID dataset.

    Args:
        raw_data_dir (Path): The directory containing meta.json and the CSV files.
        output_file_path (Path): Path to save the final processed CSV.
    """
    meta_file = raw_data_dir / 'meta.json'
    if not meta_file.exists():
        raise FileNotFoundError(f"meta.json not found in {raw_data_dir}")

    with open(meta_file, 'r') as f:
        metadata = json.load(f)

    all_fingerprints = []
    total_files = len(metadata)

    print(f"Starting processing for {total_files} files...")

    for i, appliance_meta in enumerate(metadata):
        appliance_id = appliance_meta['id']
        csv_file = raw_data_dir / f"{appliance_id}.csv"

        if not csv_file.exists():
            print(f"Warning: Skipping ID {appliance_id} - CSV file not found.")
            continue
        
        # Load the raw data
        raw_df = pd.read_csv(csv_file, header=None, names=['current', 'voltage'])

        # Extract features
        fingerprint = extract_features_from_file(raw_df, appliance_meta)
        
        if fingerprint:
            all_fingerprints.append(fingerprint)
        
        # Print progress
        if (i + 1) % 100 == 0:
            print(f"Processed {i + 1}/{total_files} files...")

    # Convert list of dicts to a DataFrame and save
    final_df = pd.DataFrame(all_fingerprints)
    output_file_path.parent.mkdir(parents=True, exist_ok=True)
    final_df.to_csv(output_file_path, index=False)
    
    print("\nProcessing complete!")
    print(f"Saved {len(final_df)} fingerprints to {output_file_path}")
    print("\nSample of the processed data:")
    print(final_df.head())


if __name__ == '__main__':
    # This allows you to run the script directly for testing
    # Assuming your data is in 'data/raw/plaid_dataset/'
    
    project_root = Path(__file__).parent.parent
    raw_data_path = project_root / 'data' / 'raw' / 'plaid_dataset'
    processed_data_path = project_root / 'data' / 'processed' / 'appliance_features.csv'

    process_plaid_data(raw_data_path, processed_data_path)
