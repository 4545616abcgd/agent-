#!/usr/bin/env python3
import argparse
import json
from pathlib import Path

def main():
    parser = argparse.ArgumentParser(description="Generate ESP-Claw OTA manifest JSON")
    parser.add_argument("--version", required=True, help="Firmware semantic version, e.g. 0.1.1")
    parser.add_argument("--url", required=True, help="HTTPS URL to edge_agent.bin")
    parser.add_argument("--hardware", default="puzhong_esp32s3")
    parser.add_argument("--channel", default="stable")
    parser.add_argument("--notes", default="")
    parser.add_argument("--mandatory", action="store_true")
    parser.add_argument("--output", default="manifest.json")
    args = parser.parse_args()

    if not args.url.startswith("https://"):
        raise SystemExit("Production manifest firmware URL must use https://")

    data = {
        "schema": 1,
        "hardware": args.hardware,
        "channel": args.channel,
        "version": args.version,
        "url": args.url,
        "mandatory": args.mandatory,
        "notes": args.notes,
    }
    Path(args.output).write_text(
        json.dumps(data, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(args.output)

if __name__ == "__main__":
    main()
