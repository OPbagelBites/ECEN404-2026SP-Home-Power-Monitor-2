import joblib
import pandas as pd
from flask import Flask, request, jsonify

# 1. Initialize the Flask App
app = Flask(__name__)

# 2. Load the trained model from the .joblib file
print("Loading model...")
model = joblib.load('models/appliance_model.joblib')
print("Model loaded successfully.")

# THIS MUST EXACTLY MATCH THE ORDER USED IN train.py
MODEL_FEATURES = [
    'p', 's', 'pf_true', 'rms_i', 'thd_i', 'crest_i',
    'h2_i_norm', 'h3_i_norm', 'h4_i_norm', 'h5_i_norm'
]


# 3. Define the API endpoint for predictions
@app.route('/predict', methods=['POST'])
def predict():
    """Receives a JSON feature packet and returns a prediction."""
    
    # Get the JSON data sent from the Ohm Monitor
    json_data = request.get_json()
    if not json_data:
        return jsonify({"error": "No input data provided"}), 400

    # Convert the incoming JSON into a Pandas DataFrame
    # This ensures the data is in the exact format our model expects.
    try:
        df = pd.DataFrame([json_data])
        
        # Ensure all required features are present, fill missing ones with 0
        for col in MODEL_FEATURES:
            if col not in df.columns:
                df[col] = 0
        
        # Keep only the features the model needs, in the correct order
        live_features = df[MODEL_FEATURES]

        # 4. Use the loaded model to make a prediction
        prediction = model.predict(live_features)
        
        # 5. Return the result as a clean JSON response
        return jsonify({
            "appliance_prediction": prediction[0]
        })

    except Exception as e:
        return jsonify({"error": str(e)}), 500


# This allows you to run the app directly from the terminal
if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5001, debug=True)

# 1.
#         # On macOS/Linux
#         source venv/bin/activate

#         # On Windows
#         .\venv\Scripts\activate

# 2.
#         pip install Flask

# 3.
#         python api.py

# 4.
#         Open a NEW Terminal
#         # On macOS/Linux
#         source venv/bin/activate

#         # On Windows
#         .\venv\Scripts\activate

# 5.
#         curl -X POST \
#         http://127.0.0.1:5001/predict \
#         -H 'Content-Type: application/json' \
#         -d '{
#                 "p": 850, "s": 855, "pf_true": 0.99, "rms_i": 7.1,
#                 "thd_i": 0.02, "crest_i": 1.42, "h2_i_norm": 0.01,
#                 "h3_i_norm": 0.01, "h4_i_norm": 0.0, "h5_i_norm": 0.0
#             }'