#!/usr/bin/env python3
"""
Train a linear classifier for proximity gesture classification.

Trains a multiclass logistic regression model on features extracted
from proximity sensor windows. Exports trained weights as C arrays
for direct inclusion in InferenceService.cpp.

Supports two normalization modes:
  --normalize-mode global  : Hard-coded constants (PROX_NORM=10000, backward compat)
  --normalize-mode local   : Per-window dynamic range normalization (cross-glass)

Pipeline:
  1. Load CSV dataset from extract_windows.py
  2. Extract 10 statistical features per window (matching firmware)
  3. Split by session ID (no data leakage)
  4. Train logistic regression with L2 regularization
  5. Evaluate on held-out sessions
  6. Compare against baseline (train_baseline.py)
  7. Export weights as C arrays

Usage:
    python train_model.py dataset.csv -o model_weights.h
    python train_model.py dataset.csv -o model_weights.h --normalize-mode local
    python train_model.py dataset.csv --compare-baseline

Requirements:
    pip install numpy scikit-learn
"""

import argparse
import csv
import sys
from pathlib import Path
from collections import defaultdict
from typing import List, Tuple, Optional
import json

WINDOW_SIZE = 40
NUM_FEATURES = 10
NUM_CLASSES = 9

LABEL_NAMES = {
    0: "idle", 1: "approach", 2: "hover", 3: "quick_pass", 4: "cover_hold",
    5: "confirmed_event", 6: "surface_reflection", 7: "noise", 8: "unknown",
}
NAME_TO_ID = {v: k for k, v in LABEL_NAMES.items()}

# Default normalization constants (must match PersonalizationConfig defaults in Config.h)
DEFAULT_PROX_NORM = 10000.0
DEFAULT_DERIV_NORM = 1000.0
DEFAULT_ENERGY_NORM = 1e8

FEATURE_NAMES = [
    "mean", "stddev", "min", "max", "peak_to_peak",
    "mean_deriv", "std_deriv", "dwell_above", "time_to_peak", "energy"
]


def extract_features(prox: List[int], dynamic_range: Optional[float] = None) -> List[float]:
    """
    Extract normalized feature vector matching InferenceService::extractFeaturesNormalized().

    IMPORTANT: This must produce identical values to the firmware implementation.
    Any divergence between offline training and on-device inference will cause
    the model to silently degrade.

    Args:
        prox: List of raw proximity readings (one window)
        dynamic_range: Per-device normalization divisor. If None, uses DEFAULT_PROX_NORM.
                       When provided, derives deriv_norm and energy_norm from it,
                       matching the firmware's extractFeaturesNormalized().
    """
    n = len(prox)
    if n == 0:
        return [0.0] * NUM_FEATURES

    # Determine normalization constants
    if dynamic_range is not None and dynamic_range > 0:
        prox_norm = dynamic_range
        deriv_norm = dynamic_range / 10.0
        energy_norm = dynamic_range * dynamic_range * n
    else:
        prox_norm = DEFAULT_PROX_NORM
        deriv_norm = DEFAULT_DERIV_NORM
        energy_norm = DEFAULT_ENERGY_NORM

    mean = sum(prox) / n
    min_val = min(prox)
    max_val = max(prox)
    max_idx = prox.index(max_val)

    sum_sq_dev = sum((v - mean) ** 2 for v in prox)
    variance = sum_sq_dev / n
    stddev = variance ** 0.5

    derivs = [prox[i] - prox[i - 1] for i in range(1, n)]
    mean_deriv = sum(derivs) / len(derivs) if derivs else 0.0
    deriv_var = (sum((d - mean_deriv) ** 2 for d in derivs) / len(derivs)) if derivs else 0.0
    std_deriv = max(0.0, deriv_var) ** 0.5

    midpoint = (min_val + max_val) / 2.0
    dwell_count = sum(1 for v in prox if v >= midpoint)

    return [
        mean / prox_norm,
        stddev / prox_norm,
        min_val / prox_norm,
        max_val / prox_norm,
        (max_val - min_val) / prox_norm,
        mean_deriv / deriv_norm,
        std_deriv / deriv_norm,
        dwell_count / n,
        max_idx / (n - 1) if n > 1 else 0.0,
        sum_sq_dev / energy_norm,
    ]


def estimate_dynamic_range(prox: List[int]) -> float:
    """
    Estimate per-window dynamic range for local normalization.

    For local mode, we use the window's own max value as the dynamic range,
    with a floor to avoid division by tiny numbers. This simulates what happens
    on-device when PersonalizationService provides the device's dynamic range.
    """
    if not prox:
        return DEFAULT_PROX_NORM
    max_val = max(prox)
    # Floor at 500 to avoid extreme normalization for near-zero windows
    return max(float(max_val), 500.0)


def load_dataset(filepath: Path, normalize_mode: str = "global") -> Tuple[list, list, list]:
    """Load CSV and return features, labels, session_ids."""
    features = []
    labels = []
    session_ids = []

    with open(filepath, "r") as f:
        reader = csv.DictReader(f)
        for row in reader:
            label_name = row["label_name"]
            if label_name not in NAME_TO_ID:
                continue

            prox = [int(row[f"prox_{i}"]) for i in range(WINDOW_SIZE)]

            if normalize_mode == "local":
                dyn_range = estimate_dynamic_range(prox)
                feat = extract_features(prox, dynamic_range=dyn_range)
            else:
                feat = extract_features(prox)

            features.append(feat)
            labels.append(NAME_TO_ID[label_name])
            session_ids.append(int(row["session_id"]))

    return features, labels, session_ids


def session_split(features, labels, session_ids, test_fraction=0.3):
    """Split by session ID for no-leakage evaluation."""
    import numpy as np

    sessions = sorted(set(session_ids))
    n_test = max(1, int(len(sessions) * test_fraction))
    test_sess = set(sessions[-n_test:])

    X = np.array(features)
    y = np.array(labels)

    train_mask = np.array([sid not in test_sess for sid in session_ids])
    test_mask = ~train_mask

    return X[train_mask], y[train_mask], X[test_mask], y[test_mask]


def export_c_weights(weights, biases, output_path: Path, normalize_mode: str):
    """Export model weights as C arrays for InferenceService.cpp."""
    with open(output_path, "w") as f:
        f.write("// Auto-generated by tools/train_model.py\n")
        f.write(f"// Normalization mode: {normalize_mode}\n")
        f.write("// Copy these arrays into InferenceService.cpp\n")
        f.write("// Then set WEIGHTS_TRAINED = true in InferenceService.h\n\n")

        class_names = [LABEL_NAMES[i] for i in sorted(LABEL_NAMES.keys())]

        f.write(f"const float InferenceService::_weights[NUM_CLASSES][NUM_FEATURES] = {{\n")
        for c in range(NUM_CLASSES):
            f.write(f"    // {class_names[c]}\n")
            f.write("    { ")
            f.write(", ".join(f"{w:.6f}f" for w in weights[c]))
            f.write(" },\n")
        f.write("};\n\n")

        f.write(f"const float InferenceService::_biases[NUM_CLASSES] = {{\n")
        f.write("    ")
        f.write(", ".join(f"{b:.6f}f" for b in biases))
        f.write("\n};\n")

    print(f"C weights exported to {output_path}")


def main():
    parser = argparse.ArgumentParser(
        description="Train linear classifier for gesture classification")
    parser.add_argument("dataset", type=Path, help="CSV dataset from extract_windows.py")
    parser.add_argument("-o", "--output", type=Path, default=None,
                        help="Output path for C weight arrays")
    parser.add_argument("--test-fraction", type=float, default=0.3,
                        help="Fraction of sessions for test set")
    parser.add_argument("--C", type=float, default=1.0,
                        help="Regularization strength (inverse)")
    parser.add_argument("--normalize-mode", choices=["global", "local"], default="global",
                        help="Feature normalization: 'global' (hard-coded PROX_NORM) "
                             "or 'local' (per-window dynamic range, cross-glass)")
    args = parser.parse_args()

    try:
        import numpy as np
        from sklearn.linear_model import LogisticRegression
        from sklearn.metrics import classification_report, confusion_matrix
    except ImportError:
        print("Required: pip install numpy scikit-learn", file=sys.stderr)
        sys.exit(1)

    if not args.dataset.exists():
        print(f"Dataset not found: {args.dataset}", file=sys.stderr)
        sys.exit(1)

    print(f"Normalization mode: {args.normalize_mode}")

    # Load data
    features, labels, session_ids = load_dataset(args.dataset, args.normalize_mode)
    print(f"Loaded {len(features)} samples")

    label_counts = defaultdict(int)
    for l in labels:
        label_counts[LABEL_NAMES[l]] += 1
    print("Label distribution:")
    for name, count in sorted(label_counts.items()):
        print(f"  {name}: {count}")

    # Split
    X_train, y_train, X_test, y_test = session_split(
        features, labels, session_ids, args.test_fraction)
    print(f"\nSplit: {len(X_train)} train, {len(X_test)} test")

    if len(X_train) == 0 or len(X_test) == 0:
        print("Insufficient data for train/test split", file=sys.stderr)
        sys.exit(1)

    # Train
    model = LogisticRegression(
        C=args.C,
        max_iter=1000,
        multi_class="multinomial",
        solver="lbfgs",
        class_weight="balanced",
    )
    model.fit(X_train, y_train)

    # Evaluate
    target_names = [LABEL_NAMES[i] for i in sorted(LABEL_NAMES.keys())]

    print("\n=== Training Set ===")
    y_train_pred = model.predict(X_train)
    print(classification_report(y_train, y_train_pred, target_names=target_names))

    print("\n=== Test Set (held-out sessions) ===")
    y_test_pred = model.predict(X_test)
    print(classification_report(y_test, y_test_pred, target_names=target_names))

    print("Confusion Matrix (rows=true, cols=predicted):")
    cm = confusion_matrix(y_test, y_test_pred)
    header = "            " + "".join(f"{n:>12}" for n in target_names)
    print(header)
    for i, row in enumerate(cm):
        print(f"{target_names[i]:>12}" + "".join(f"{v:>12}" for v in row))

    # Export weights
    weights = model.coef_
    biases = model.intercept_

    print(f"\nModel weights shape: {weights.shape}")
    print(f"Model biases shape: {biases.shape}")

    if args.output:
        export_c_weights(weights, biases, args.output, args.normalize_mode)

    # Also export as JSON for other tools
    json_path = args.output.with_suffix(".json") if args.output else Path("model_weights.json")
    with open(json_path, "w") as f:
        json.dump({
            "weights": weights.tolist(),
            "biases": biases.tolist(),
            "feature_names": FEATURE_NAMES,
            "class_names": target_names,
            "normalize_mode": args.normalize_mode,
            "normalization": {
                "prox_norm": DEFAULT_PROX_NORM if args.normalize_mode == "global" else "per_window",
                "deriv_norm": DEFAULT_DERIV_NORM if args.normalize_mode == "global" else "derived",
                "energy_norm": DEFAULT_ENERGY_NORM if args.normalize_mode == "global" else "derived",
            },
            "train_samples": len(X_train),
            "test_samples": len(X_test),
        }, f, indent=2)
    print(f"JSON weights exported to {json_path}")


if __name__ == "__main__":
    main()
