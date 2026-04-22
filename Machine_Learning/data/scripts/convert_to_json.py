import pandas as pd
import numpy as np
from pathlib import Path
import json

def main():
    # Define paths
    base_dir = Path("Machine_Learning") if Path("Machine_Learning").exists() else Path(".")
    input_csv = base_dir / "data/processed/appliance_features.csv"
    output_json = base_dir / "data/processed/appliance_features.json"

    # Check if input exists
    if not input_csv.exists():
        print(f"Error: Could not find {input_csv}")
        print("Please save the CSV data into 'data/raw/plaid_features.csv' first.")
        return

    print(f"Reading {input_csv}...")
    df = pd.read_csv(input_csv)

    # 1. Rename columns to match Model Config (src/config.py)
    #    The model expects: p, s, pf_true, rms_i, thd_i, crest_i, hX_i_norm
    column_mapping = {
        "active_power": "p",
        "apparent_power": "s",
        "power_factor": "pf_true",
        "crest_factor_i": "crest_i",
        # 'rms_i', 'thd_i', 'h2_i_norm'... are already named correctly in your CSV
    }
    df = df.rename(columns=column_mapping)

    # 2. Calculate Reactive Power (var_p)
    #    Q = sqrt(S^2 - P^2)
    #    We use np.maximum to ensure we don't sqrt a negative number due to tiny sensor noise
    print("Calculating Reactive Power (var_p)...")
    df["var_p"] = np.sqrt(np.maximum(0, df["s"]**2 - df["p"]**2))

    # 3. Add Event ID (just use the index or file_id)
    df["event_id"] = df["file_id"]

    # 4. Select only the columns we need for the JSON
    #    (Keep appliance_type and all numeric features)
    keep_cols = [
        "appliance_type", "event_id", 
        "p", "s", "var_p", "pf_true", 
        "rms_v", "rms_i", "thd_i", "crest_i",
        "h2_i_norm", "h3_i_norm", "h4_i_norm", "h5_i_norm"
    ]
    
    # Filter to available columns (in case h6 exists but we don't need it)
    final_cols = [c for c in keep_cols if c in df.columns]
    df_clean = df[final_cols]

    # 5. Convert to JSON list of dicts
    print(f"Converting {len(df_clean)} rows to JSON...")
    records = df_clean.to_dict(orient="records")

    # 6. Save
    output_json.parent.mkdir(parents=True, exist_ok=True)
    with open(output_json, "w") as f:
        json.dump(records, f, indent=2)

    print(f"Success! Saved clean dataset to: {output_json}")
    
    # Print sanity check
    print("\nSanity Check (Average Watts per Appliance):")
    print(df.groupby("appliance_type")["p"].mean().astype(int))

if __name__ == "__main__":
    main()