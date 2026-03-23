#!/usr/bin/env python3
"""
Extract and validate binary SampleWindow records from SPIFFS session files.

Reads .bin files produced by DataLoggerService and outputs a structured
dataset as CSV or NumPy arrays for model training.

Binary format per window (192 bytes):
  WindowHeader (32 bytes):
    - magic (uint16): 0x4D4C ("ML")
    - version (uint8): 1
    - sampleCount (uint8): 40
    - sessionId (uint16)
    - windowId (uint16)
    - label (uint8): GestureLabel enum
    - reserved0 (uint8)
    - startUptimeMs (uint32)
    - startEpoch (uint32)
    - wakeThreshold (uint16)
    - hitThreshold (uint16)
    - criticalHitThreshold (uint16)
    - sensitivity (uint8)
    - flags (uint8)
    - bootCount (uint8)
    - reserved1 (uint8)
    - crc32 (uint32)
  SampleRecord[40] (160 bytes):
    - proximity (uint16)
    - dt_ms (uint16)

Usage:
    python extract_windows.py /path/to/session_files/ -o dataset.csv
    python extract_windows.py /path/to/session_files/ -o dataset.npz --format npz
"""

import argparse
import struct
import os
import sys
from pathlib import Path
from typing import List, Tuple, Optional
import csv

# Constants matching firmware Config.h / SampleWindow.h
WINDOW_MAGIC = 0x4D4C
WINDOW_VERSION = 1
WINDOW_SIZE = 40
SAMPLE_RECORD_SIZE = 4  # uint16 + uint16
WINDOW_HEADER_SIZE = 32
SAMPLE_WINDOW_SIZE = WINDOW_HEADER_SIZE + (SAMPLE_RECORD_SIZE * WINDOW_SIZE)

# Labels matching GestureLabel enum
LABEL_NAMES = {
    0: "idle",
    1: "approach",
    2: "hover",
    3: "quick_pass",
    4: "cover_hold",
    5: "confirmed_event",
    6: "surface_reflection",
    7: "noise",
    8: "unknown",
    254: "unlabeled",
    255: "invalid",
}

# Struct formats
HEADER_FMT = "<HBBHHBBIIHHHBBBBI"  # Little-endian, packed
SAMPLE_FMT = "<HH"  # proximity (uint16) + dt_ms (uint16)


def crc32(data: bytes) -> int:
    """CRC32 matching firmware implementation (same polynomial as ProtocolDefs.h)."""
    import binascii
    return binascii.crc32(data) & 0xFFFFFFFF


def parse_header(data: bytes) -> dict:
    """Parse a 32-byte WindowHeader from binary data."""
    if len(data) < WINDOW_HEADER_SIZE:
        return None

    fields = struct.unpack(HEADER_FMT, data[:WINDOW_HEADER_SIZE])
    return {
        "magic": fields[0],
        "version": fields[1],
        "sampleCount": fields[2],
        "sessionId": fields[3],
        "windowId": fields[4],
        "label": fields[5],
        "reserved0": fields[6],
        "startUptimeMs": fields[7],
        "startEpoch": fields[8],
        "wakeThreshold": fields[9],
        "hitThreshold": fields[10],
        "criticalHitThreshold": fields[11],
        "sensitivity": fields[12],
        "flags": fields[13],
        "bootCount": fields[14],
        "reserved1": fields[15],
        "crc32": fields[16],
    }


def parse_samples(data: bytes, count: int) -> List[Tuple[int, int]]:
    """Parse sample records from binary data."""
    samples = []
    for i in range(count):
        offset = i * SAMPLE_RECORD_SIZE
        if offset + SAMPLE_RECORD_SIZE > len(data):
            break
        prox, dt = struct.unpack(SAMPLE_FMT, data[offset:offset + SAMPLE_RECORD_SIZE])
        samples.append((prox, dt))
    return samples


def validate_window(raw: bytes) -> Optional[dict]:
    """Parse and validate a single SampleWindow from raw bytes."""
    if len(raw) < SAMPLE_WINDOW_SIZE:
        return None

    header = parse_header(raw[:WINDOW_HEADER_SIZE])
    if header is None:
        return None

    # Validate magic
    if header["magic"] != WINDOW_MAGIC:
        return None

    # Validate version
    if header["version"] != WINDOW_VERSION:
        return None

    # Validate sample count
    if header["sampleCount"] > WINDOW_SIZE:
        return None

    # Validate CRC
    # CRC is over header (excluding crc32 field) + samples
    crc_data = raw[:WINDOW_HEADER_SIZE - 4] + raw[WINDOW_HEADER_SIZE:SAMPLE_WINDOW_SIZE]
    expected_crc = crc32(crc_data)
    if header["crc32"] != expected_crc:
        print(f"  CRC mismatch: expected {expected_crc:#010x}, got {header['crc32']:#010x}",
              file=sys.stderr)
        return None

    # Parse samples
    sample_data = raw[WINDOW_HEADER_SIZE:SAMPLE_WINDOW_SIZE]
    samples = parse_samples(sample_data, header["sampleCount"])

    return {
        "header": header,
        "samples": samples,
        "label_name": LABEL_NAMES.get(header["label"], f"unknown_{header['label']}"),
    }


def extract_file(filepath: Path) -> List[dict]:
    """Extract all valid windows from a single .bin session file."""
    windows = []
    with open(filepath, "rb") as f:
        data = f.read()

    num_windows = len(data) // SAMPLE_WINDOW_SIZE
    for i in range(num_windows):
        offset = i * SAMPLE_WINDOW_SIZE
        raw = data[offset:offset + SAMPLE_WINDOW_SIZE]
        window = validate_window(raw)
        if window:
            window["source_file"] = filepath.name
            window["window_index"] = i
            windows.append(window)
        else:
            print(f"  Window {i} in {filepath.name}: INVALID (skipped)", file=sys.stderr)

    return windows


def extract_all(input_dir: Path) -> List[dict]:
    """Extract windows from all .bin files in directory."""
    all_windows = []
    bin_files = sorted(input_dir.glob("*.bin"))

    if not bin_files:
        print(f"No .bin files found in {input_dir}", file=sys.stderr)
        return all_windows

    for bf in bin_files:
        print(f"Processing {bf.name}...")
        windows = extract_file(bf)
        print(f"  {len(windows)} valid windows extracted")
        all_windows.extend(windows)

    return all_windows


def write_csv(windows: List[dict], output_path: Path):
    """Write extracted windows to CSV format."""
    with open(output_path, "w", newline="") as f:
        writer = csv.writer(f)

        # Header row
        header = ["source_file", "session_id", "window_id", "label", "label_name",
                  "start_uptime_ms", "sensitivity", "wake_threshold",
                  "hit_threshold", "critical_hit_threshold", "flags"]
        # Add sample columns
        for i in range(WINDOW_SIZE):
            header.extend([f"prox_{i}", f"dt_{i}"])
        writer.writerow(header)

        # Data rows
        for w in windows:
            h = w["header"]
            row = [
                w["source_file"],
                h["sessionId"],
                h["windowId"],
                h["label"],
                w["label_name"],
                h["startUptimeMs"],
                h["sensitivity"],
                h["wakeThreshold"],
                h["hitThreshold"],
                h["criticalHitThreshold"],
                h["flags"],
            ]
            for prox, dt in w["samples"]:
                row.extend([prox, dt])
            # Pad if fewer samples than WINDOW_SIZE
            for _ in range(WINDOW_SIZE - len(w["samples"])):
                row.extend([0, 0])
            writer.writerow(row)


def write_npz(windows: List[dict], output_path: Path):
    """Write extracted windows to NumPy .npz format."""
    try:
        import numpy as np
    except ImportError:
        print("NumPy not installed. Install with: pip install numpy", file=sys.stderr)
        sys.exit(1)

    n = len(windows)
    if n == 0:
        print("No windows to write.", file=sys.stderr)
        return

    # Arrays
    proximity = np.zeros((n, WINDOW_SIZE), dtype=np.uint16)
    dt_ms = np.zeros((n, WINDOW_SIZE), dtype=np.uint16)
    labels = np.zeros(n, dtype=np.uint8)
    session_ids = np.zeros(n, dtype=np.uint16)
    window_ids = np.zeros(n, dtype=np.uint16)
    sensitivity = np.zeros(n, dtype=np.uint8)

    for i, w in enumerate(windows):
        h = w["header"]
        labels[i] = h["label"]
        session_ids[i] = h["sessionId"]
        window_ids[i] = h["windowId"]
        sensitivity[i] = h["sensitivity"]
        for j, (prox, dt) in enumerate(w["samples"]):
            proximity[i, j] = prox
            dt_ms[i, j] = dt

    np.savez(output_path,
             proximity=proximity,
             dt_ms=dt_ms,
             labels=labels,
             session_ids=session_ids,
             window_ids=window_ids,
             sensitivity=sensitivity)


def main():
    parser = argparse.ArgumentParser(
        description="Extract ML windows from SPIFFS binary session files")
    parser.add_argument("input_dir", type=Path,
                        help="Directory containing .bin session files")
    parser.add_argument("-o", "--output", type=Path, required=True,
                        help="Output file path (.csv or .npz)")
    parser.add_argument("--format", choices=["csv", "npz"], default=None,
                        help="Output format (auto-detected from extension)")
    args = parser.parse_args()

    if not args.input_dir.is_dir():
        print(f"Input directory not found: {args.input_dir}", file=sys.stderr)
        sys.exit(1)

    # Auto-detect format from extension
    fmt = args.format
    if fmt is None:
        if args.output.suffix == ".npz":
            fmt = "npz"
        else:
            fmt = "csv"

    # Extract
    windows = extract_all(args.input_dir)
    print(f"\nTotal valid windows: {len(windows)}")

    if not windows:
        print("No windows extracted. Check input files.", file=sys.stderr)
        sys.exit(1)

    # Summary
    label_counts = {}
    session_counts = {}
    for w in windows:
        ln = w["label_name"]
        sid = w["header"]["sessionId"]
        label_counts[ln] = label_counts.get(ln, 0) + 1
        session_counts[sid] = session_counts.get(sid, 0) + 1

    print("\nLabel distribution:")
    for label, count in sorted(label_counts.items()):
        print(f"  {label}: {count}")

    print(f"\nSessions: {len(session_counts)}")

    # Write output
    if fmt == "npz":
        write_npz(windows, args.output)
    else:
        write_csv(windows, args.output)

    print(f"\nDataset written to {args.output}")


if __name__ == "__main__":
    main()
