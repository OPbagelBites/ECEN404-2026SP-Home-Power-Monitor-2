import argparse
import subprocess
import os

def run_inference_api():
    """Builds and runs the Docker container for the API."""
    api_image_name = "ohm-api"
    
    print(f"Building the '{api_image_name}' container...")
    subprocess.run(["docker", "build", "-t", api_image_name, "-f", "Dockerfile.api", "."], check=True)
    
    print(f"\nRunning the inference API on http://localhost:5001")
    print("Press CTRL+C to stop.")
    # --rm automatically cleans up the container when it exits
    subprocess.run(["docker", "run", "--rm", "-p", "5001:5001", api_image_name], check=True)

def run_training_job():
    """Builds and runs the Docker container for a one-off training job."""
    trainer_image_name = "ohm-trainer"
    
    print(f"Building the '{trainer_image_name}' container...")
    subprocess.run(["docker", "build", "-t", trainer_image_name, "-f", "Dockerfile.train", "."], check=True)
    
    print("\nRunning the training job...")
    # This is the most important part for training:
    # We "mount" the local 'models' directory into the container.
    # This ensures that the newly trained .joblib file is saved back to your local machine.
    model_dir = os.path.join(os.getcwd(), 'models')
    os.makedirs(model_dir, exist_ok=True) # Ensure the directory exists
    
    subprocess.run([
        "docker", "run", "--rm",
        "-v", f"{model_dir}:/app/models", # Mount local 'models' to container's '/app/models'
        trainer_image_name
    ], check=True)
    print("\nTraining complete. New model is available in your local 'models' directory.")

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description="Ohm Sweet Home ML Orchestrator")
    parser.add_argument('task', choices=['api', 'train'], help="The task to run: 'api' to serve predictions, or 'train' to retrain the model.")
    
    args = parser.parse_args()
    
    if args.task == 'api':
        run_inference_api()
    elif args.task == 'train':
        run_training_job()

# # On macOS/Linux
# source ven/bin/activate

# python main.py api 

# python main.py train


