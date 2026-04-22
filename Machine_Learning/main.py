import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional

# -------------------------------------------------------------------
# CONFIGURATION
# -------------------------------------------------------------------
# Defines the contract between the Host (your laptop) and the Containers.
DEFAULT_API_IMAGE = "ohm-api"      # Name of the Inference Container
DEFAULT_TRAIN_IMAGE = "ohm-trainer" # Name of the Training Job Container
DEFAULT_HOST_PORT = 5001           # Port you access on localhost
CONTAINER_API_PORT = 8080          # Port Gunicorn listens on INSIDE the container

# Internal paths where the container expects to find files.
# We must mount our local folders to these exact locations.
CONTAINER_MODELS_DIR = Path("/app/models")
CONTAINER_DATA_DIR = Path("/app/data")

def _check_docker():
    """Safety check to ensure Docker is actually installed and running."""
    if not shutil.which("docker"):
        print("Error: Docker is not installed or not on PATH.", file=sys.stderr)
        sys.exit(1)

def _build_image(dockerfile: str, tag: str, no_cache: bool, pull: bool, platform: Optional[str]):
    """
    Wraps 'docker build' to create our containers.
    
    Why: Ensures we always use the correct Dockerfile (api vs train) and 
    allows forcing a clean rebuild (--no-cache) if libraries change.
    """
    cmd = ["docker", "build", "-t", tag, "-f", dockerfile, "."]
    if no_cache:
        cmd.append("--no-cache")
    if pull:
        cmd.append("--pull")
    if platform:
        cmd.extend(["--platform", platform])
    
    print(f"Building image: {' '.join(cmd)}")
    subprocess.run(cmd, check=True)

def run_inference_api(
    image: str,
    port: int,
    models_dir: Path,
    no_cache: bool,
    pull: bool,
    platform: Optional[str]
):
    """
    ORCHESTRATOR: Launches the API as a long-running service.
    
    Key Feature: Volume Mounting
    - We map local './models' -> container '/app/models'.
    - This allows the API to load the trained model (.joblib) 
      without needing to rebuild the image every time we retrain.
    """
    _check_docker()
    # Ensure models dir exists so Docker doesn't create it as root-owned
    models_dir.mkdir(parents=True, exist_ok=True)

    print(f"--- Building API Image ({image}) ---")
    _build_image(dockerfile="Dockerfile.api", tag=image, no_cache=no_cache, pull=pull, platform=platform)

    print(f"\nStarting API container...")
    print(f"Serving on http://localhost:{port}")
    
    # Docker Run Command
    # --rm: Clean up container after it stops
    # -p: Bridge Local Port (5001) to Container Port (8080)
    run_cmd = [
        "docker", "run", "--rm",
        "-p", f"{port}:{CONTAINER_API_PORT}",
        "-v", f"{models_dir.resolve()}:{CONTAINER_MODELS_DIR}", 
        image,
    ]
    
    try:
        subprocess.run(run_cmd, check=True)
    except KeyboardInterrupt:
        print("\nStopping container...")

def run_training_job(
    image: str,
    models_dir: Path,
    data_path: Optional[Path],
    model_output_path: Optional[Path],
    no_cache: bool,
    pull: bool,
    platform: Optional[str],
):
    """
    ORCHESTRATOR: Launches the Trainer as a one-off job.
    
    Key Feature: Dynamic Data Injection
    - The container is empty by default.
    - We dynamically mount YOUR specific data file into the container
      at runtime.
    - We mount the models directory so the container can "hand back"
      the trained brain (.joblib) to your host machine.
    """
    _check_docker()
    models_dir.mkdir(parents=True, exist_ok=True)

    print(f"--- Building Trainer Image ({image}) ---")
    _build_image(dockerfile="Dockerfile.train", tag=image, no_cache=no_cache, pull=pull, platform=platform)

    # 1. Base Command: Mount the output directory (models)
    run_cmd = [
        "docker", "run", "--rm",
        "-v", f"{models_dir.resolve()}:{CONTAINER_MODELS_DIR}",
    ]

    override_args = []

    # 2. Handle Input Data Mapping
    # Scenario: User passes "--data-path /Users/josh/data.json"
    # Action: We mount "/Users/josh" to "/app/data" inside container
    #         and tell the script to look for "/app/data/data.json"
    if data_path:
        data_path = data_path.resolve()
        data_dir = data_path.parent
        container_data_path = CONTAINER_DATA_DIR / data_path.name
        
        run_cmd.extend(["-v", f"{data_dir}:{CONTAINER_DATA_DIR}"])
        override_args.extend(["--data_path", str(container_data_path)])

    # 3. Handle Output Path Mapping
    # We translate the host path to the internal container path
    if model_output_path:
        filename = model_output_path.name
        container_output_path = CONTAINER_MODELS_DIR / filename
        override_args.extend(["--model_output_path", str(container_output_path)])

    run_cmd.append(image)
    run_cmd.extend(override_args)

    print(f"\n--- Running Training Job ---")
    subprocess.run(run_cmd, check=True)
    print(f"\n[Done] Training complete. Check your local '{models_dir}' folder.")

def main():
    """
    CLI Entrypoint.
    Usage:
      python main.py api   -> Runs the API
      python main.py train -> Runs the training job
    """
    parser = argparse.ArgumentParser(description="Ohm Sweet Home ML Orchestrator")
    sub = parser.add_subparsers(dest="task", required=True)

    # Common flags helper (DRY principle)
    def add_common_flags(p):
        p.add_argument("--platform", default=None, help="Docker platform (e.g. linux/amd64)")
        p.add_argument("--no-cache", action="store_true", help="Force rebuild without cache")
        p.add_argument("--pull", action="store_true", help="Pull latest base image")

    # --- API Command Definition ---
    p_api = sub.add_parser("api", help="Run API")
    p_api.add_argument("--image", default=DEFAULT_API_IMAGE)
    p_api.add_argument("--port", type=int, default=DEFAULT_HOST_PORT)
    p_api.add_argument("--models-dir", default="models")
    add_common_flags(p_api)

    # --- TRAIN Command Definition ---
    p_train = sub.add_parser("train", help="Run Training")
    p_train.add_argument("--image", default=DEFAULT_TRAIN_IMAGE)
    p_train.add_argument("--models-dir", default="models")
    p_train.add_argument("--data-path", required=False)
    p_train.add_argument("--model-output-path", required=False)
    add_common_flags(p_train)

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
            model_output_path=(Path(args.model_output_path) if args.model_output_path else None),
            no_cache=args.no_cache,
            pull=args.pull,
            platform=args.platform,
        )

if __name__ == "__main__":
    main()