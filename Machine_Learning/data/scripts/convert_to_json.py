import pandas as pd
import json
import time
from pathlib import Path

def convert_row_to_json(row, frame_id: int) -> dict:
    """
    Converts a single row of the appliance features DataFrame into the target JSON format.
    """
    # --- Create the nested h_i dictionary from the h_i_norm columns ---
    harmonics_dict = {}
    for i in range(2, 6): # Assuming you have h2 to h5
        col_name = f'h{i}_i_norm'
        if col_name in row and pd.notna(row[col_name]):
            freq = i * 60 # Calculate harmonic frequency
            harmonics_dict[str(freq)] = round(row[col_name], 4)

    # --- Build the final JSON package using data from the row ---
    json_package = {
        # Placeholders for time-series data not present in the summary CSV
        "t": int(time.time() * 1000), # Using current time as a placeholder
        "frame_id": frame_id,
        "fs": 30000, # Placeholder based on PLAID dataset
        "N": 1024,   # Common window size, placeholder
        "window": "hann",
        "freq_hz": 60.00,

        # Values from the CSV row
        "rms_v": row.get('rms_v', 120.0),
        "rms_i": row.get('rms_i', 0),
        "p": row.get('active_power', 0),
        "s": row.get('apparent_power', 0),
        "pf_true": row.get('power_factor', 0),
        
        # More placeholders for data not in the summary file
        "v1_rms": row.get('rms_v', 120.0),
        "i1_rms": None, 
        "phi_deg": None,
        "p1": None,
        "q1": None,
        "s1": None,
        "pf_disp": None,

        # Values from the CSV row
        "thd_i": row.get('thd_i', 0),
        "thd_v": None, # Placeholder
        "h_i": harmonics_dict,
        "odd_sum_i": None, # Placeholder
        "even_sum_i": None, # Placeholder
        "crest_i": row.get('crest_i', 0),
        "form_i": None, # Placeholder
        
        # Event data based on the appliance type
        "state": "on",
        "d_irms": row.get('rms_i', 0), # Using rms_i as a proxy
        "d_p": row.get('active_power', 0), # Using active_power as a proxy
        "events": [{
            "type": "on", 
            "t": int(time.time() * 1000), 
            "appliance_identified": row.get('appliance_type', 'Unknown')
        }],

        # Metadata placeholders
        "fw": "generated-from-csv-1.0",
        "cal_id": f"file_id_{row.get('file_id', 'N/A')}",
        "harmonics": {} # Placeholder
    }
    
    # Clean up the JSON by removing keys with None values
    return {k: v for k, v in json_package.items() if v is not None}


def main():
    """
    Main function to read the CSV and write the JSON output.
    """
    project_root = Path(__file__).parent.parent.parent
    input_csv_path = project_root / 'data' / 'processed' / 'appliance_features.csv'
    output_json_path = project_root / 'data' / 'processed' / 'appliance_features.json'

    if not input_csv_path.exists():
        print(f"Error: Input file not found at {input_csv_path}")
        return

    print(f"Reading data from {input_csv_path}...")
    df = pd.read_csv(input_csv_path)

    # Apply the conversion function to each row of the DataFrame
    json_output_list = [convert_row_to_json(row, i) for i, row in df.iterrows()]

    print(f"Writing {len(json_output_list)} JSON objects to {output_json_path}...")
    with open(output_json_path, 'w') as f:
        json.dump(json_output_list, f, indent=4)

    print("Conversion complete!")

if __name__ == '__main__':
    main()