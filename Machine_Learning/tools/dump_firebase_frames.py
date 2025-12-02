#!/usr/bin/env python
import json
from pathlib import Path

import firebase_admin
from firebase_admin import credentials, db

DB_URL = "https://home-power-monitor-752b7-default-rtdb.firebaseio.com"

# change this if your device/stream id changes
FRAMES_PATH = "/devices/esp32-devkitc-01/streams/fw-esp-0-1-0/frames"

def init_firebase():
    cred = credentials.Certificate("firebase_service_account.json")
    firebase_admin.initialize_app(cred, {"databaseURL": DB_URL})

def main(out_path: Path):
    init_firebase()

    ref = db.reference(FRAMES_PATH)
    data = ref.order_by_key().limit_to_last(500).get()    # { "0": {...}, "1": {...}, ... }

    # turn the dict of frames into a simple list
    frames = []
    if isinstance(data, dict):
        # sort by key so 0,1,2,... are in order
        for k in sorted(data.keys(), key=lambda x: int(x)):
            frames.append(data[k])

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(frames, indent=2))
    print(f"Dumped {len(frames)} frames to {out_path}")

if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--out",
        type=Path,
        default=Path("data/raw/firebase_frames.json"),
    )
    args = ap.parse_args()
    main(args.out)
