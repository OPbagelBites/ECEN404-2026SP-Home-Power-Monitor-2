import joblib
import pandas as pd
from flask import Flask, request, jsonify

# 1. Initialize the Flask App
app = Flask(__name__)

# 2. Load the trained model from the .joblib file
# This is done only ONCE when the server starts up for efficiency.
print("Loading model...")
model = joblib.load('models/appliance_model.joblib')
print("Model loaded successfully.")

# Define the order of features the model was trained on.
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
    # For development, you can run it on port 5000
    # For production, a proper web server like Gunicorn would be used.
    app.run(host='0.0.0.0', port=5001, debug=True)



### ## Step 2: Install Flask

#Your virtual environment doesn't have Flask yet. You need to install it.

#* In your terminal (with the `(venv)` active), run:
#```bash
#pip install Flask


### ## Step 3: Run the API Server

#Now you can start your server. It will load your model into memory and wait for requests.

#* In your terminal (with the `(venv)` active), run:
#```bash
#python api.py
#```
#* You will see output confirming that the model was loaded and the server is running, something like:
#Loading model...
#Model loaded successfully.
#* Serving Flask app 'api'
#* Running on http://127.0.0.1:5000
#Press CTRL+C to quit

#**Your model is now live and ready to make predictions!**


### ## Step 4: Test Your Live Model

#Finally, let's send a sample request to your running server to see it in action.

#1.  **Open a NEW terminal window** (leave your server running in the first one).
#2.  **Activate your `(venv)`** in this new terminal as well (`source venv/bin/activate`).
#3.  Use the `curl` command to send a sample JSON packet to your `/predict` endpoint.

#* Copy and paste the command below into your new terminal and press **Enter**. This simulates what the Ohm Monitor hardware would do.

#```bash
#curl -X POST \
#http://127.0.0.1:5000/predict \
#-H 'Content-Type: application/json' \
#-d '{
#    "p": 850,
#    "s": 855,
#    "pf_true": 0.99,
#    "rms_i": 7.1,
#    "thd_i": 0.02,
#    "crest_i": 1.42,
#    "h_i": {"120": 0.01, "180": 0.01, "240": 0.0, "300": 0.0}
#   }'
#```

### **Expected Response**

#In your second terminal, you will instantly get a JSON response back from your model. Since the data we sent looks like a resistive heating element, the response should be something like this:

##```json
#{
#"appliance_prediction": "Hairdryer" 
#}
