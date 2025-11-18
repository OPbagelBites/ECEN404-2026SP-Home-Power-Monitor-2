import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional

DEFAULT_API_IMAGE = "ohm-api"
DEFAULT_TRAIN_IMAGE = "ohm-trainer"
DEFAULT_PORT = 5001

# Inside the container, our code expects models here (api loads DEFAULT_MODEL_PATH under /app/models)
CONTAINER_MODELS_DIR = "/app/models"

def _check_docker():
    if not shutil.which("docker"):
        print("Error: Docker is not installed or not on PATH.", file=sys.stderr)
        sys.exit(1)

def _build_image(dockerfile: str, tag: str, no_cache: bool, pull: bool, platform: Optional[str]): # <--- UPDATED
    cmd = ["docker", "build", "-t", tag, "-f", dockerfile, "."]
    if no_cache:
        cmd.insert(2, "--no-cache")
    if pull:
        cmd.insert(2, "--pull")
    if platform:
        cmd.insert(2, "--platform")
        cmd.insert(3, platform)
    print(f"Building image: {' '.join(cmd)}")
    subprocess.run(cmd, check=True)

def run_inference_api(
    image: str,
    port: int,
    models_dir: Path,
    no_cache: bool,
    pull: bool,
    platform: Optional[str] # <--- UPDATED
):
    """Build and run the API container."""
    _check_docker()
    models_dir.mkdir(parents=True, exist_ok=True)

    _build_image(dockerfile="Dockerfile.api", tag=image, no_cache=no_cache, pull=pull, platform=platform)

    print(f"\nRunning the inference API on http://localhost:{port}")
    print("Press CTRL+C to stop.")
    run_cmd = [
        "docker", "run", "--rm",
        "-p", f"{port}:5001",
        "-v", f"{models_dir.resolve()}:{CONTAINER_MODELS_DIR}", # Use resolve() for full path
        image,
    ]
    subprocess.run(run_cmd, check=True)

def run_training_job(
    image: str,
    models_dir: Path,
    data_path: Optional[Path],
    model_output_path: Optional[Path],
    no_cache: bool,
    pull: bool,
    platform: Optional[str],
):
    """Build and run a one-off training job container."""
    _check_docker()
    models_dir.mkdir(parents=True, exist_ok=True)

    _build_image(dockerfile="Dockerfile.train", tag=image, no_cache=no_cache, pull=pull, platform=platform)

    # Start assembling docker run (volumes FIRST)
    run_cmd = [
        "docker", "run", "--rm",
        "-v", f"{models_dir.resolve()}:{CONTAINER_MODELS_DIR}",
    ]

    # We’ll append optional mounts BEFORE the image name
    override_args = []

    if data_path:
        data_path = data_path.resolve()
        data_parent = data_path.parent
        # Mount the parent directory of the data file
        run_cmd.extend(["-v", f"{data_parent}:{data_parent}"])
        # Pass the full (container-side) path to the train script
        override_args.extend(["--data_path", str(data_path)])

    if model_output_path:
        model_output_path = model_output_path.resolve()
        model_output_path.parent.mkdir(parents=True, exist_ok=True)
        # Mount the parent directory of the output file
        run_cmd.extend(["-v", f"{model_output_path.parent}:{model_output_path.parent}"])
        override_args.extend(["--model_output_path", str(model_output_path)])

    # Now append the image name
    run_cmd.append(image)

    # Append trainer CLI args (which are the override_args)
    run_cmd.extend(override_args)

    print("\nRunning the training job...")
    subprocess.run(run_cmd, check=True)
    print(f"\nTraining complete. New/updated model should be in {models_dir} "
          f"or at your custom --model-output-path if provided.")

def main():
    parser = argparse.ArgumentParser(description="Ohm Sweet Home ML Orchestrator")
    sub = parser.add_subparsers(dest="task", required=True)

    # Common toggles
    def add_build_flags(p):
        p.add_argument("--platform", default=None, help="e.g. linux/amd64 (useful on Apple Silicon)")
        p.add_argument("--no-cache", action="store_true", help="Build images without cache")
        p.add_argument("--pull", action="store_true", help="Always attempt to pull a newer base image")

    # API
    p_api = sub.add_parser("api", help="Serve predictions from the trained model")
    p_api.add_argument("--image", default=DEFAULT_API_IMAGE, help="Docker image tag for the API")
    p_api.add_argument("--port", type=int, default=DEFAULT_PORT, help="Host port to expose API on")
    p_api.add_argument("--models-dir", default="models", help="Host directory containing the .joblib model")
    add_build_flags(p_api)

    # TRAIN
    p_train = sub.add_parser("train", help="Run a one-off training job")
    p_train.add_argument("--image", default=DEFAULT_TRAIN_IMAGE, help="Docker image tag for the trainer")
    p_train.add_argument("--models-dir", default="models", help="Host directory where trained model(s) will be saved")
    p_train.add_argument("--data-path", default=None, help="Optional host path to features JSON/CSV (passed to train.py)")
    p_train.add_argument("--model-output-path", default=None, help="Optional host path for output .joblib (passed to train.py)")
    add_build_flags(p_train)

    args = parser.parse_args()
    cwd = Path.cwd()

    if args.task == "api":
        run_inference_api(
            image=args.image,
            port=args.port,
            models_dir=(cwd / args.models_dir),
            no_cache=args.no_cache,
            pull=args.pull,
            platform=args.platform,
        )
    elif args.task == "train":
        run_training_job(
            image=args.image,
            models_dir=(cwd / args.models_dir),
            data_path=(Path(args.data_path) if args.data_path else None),
            model_output_path=(Path(args.model_output_path) if args.model_output_path else None), # <--- CORRECTED THIS LINE
            no_cache=args.no_cache,
            pull=args.pull,
            platform=args.platform,
        )
    else:
        parser.error("Unknown task")

if __name__ == "__main__":
    main()



# source venv/bin/activate
# python main.py train --data-path data/processed/appliance_features.json
# python main.py api