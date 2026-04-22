import argparse, json
from pathlib import Path

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True, help="existing appliance_features.json")
    ap.add_argument("--add", required=True, help="new rows json")
    ap.add_argument("--out", required=True, help="merged output json")
    args = ap.parse_args()

    base = json.loads(Path(args.base).read_text())
    add = json.loads(Path(args.add).read_text())

    if not isinstance(base, list) or not isinstance(add, list):
        raise SystemExit("Both files must be JSON arrays (lists) of rows.")

    merged = base + add
    Path(args.out).write_text(json.dumps(merged, indent=2))
    print(f"Merged: {len(base)} + {len(add)} = {len(merged)} -> {args.out}")

if __name__ == "__main__":
    main()