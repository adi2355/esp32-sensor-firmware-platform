#!/usr/bin/env python3
"""
Train a hand-tuned rule baseline for proximity gesture classification.

This is the threshold the ML model must beat to earn the TinyML label.
If the ML model does not beat this baseline on held-out sessions,
the honest thing is to publish the baseline and call it signal
classification, not AI.

Features used by the baseline:
  - mean proximity
  - stddev proximity
  - peak-to-peak range
  - dwell fraction above midpoint
  - mean first derivative (trend direction)

The rules are designed around physical intuition:
  idle:       low mean, low stddev, low peak-to-peak
  approach:   positive mean derivative, moderate stddev
  hover:      high mean, low stddev (plateau)
  quick_pass: high stddev, high peak-to-peak, low dwell
  cover_hold: very high mean, very high dwell

Usage:
    python train_baseline.py dataset.csv --split-by session
"""

import argparse
import csv
import sys
from pathlib import Path
from collections import defaultdict
from typing import List, Dict, Tuple

WINDOW_SIZE = 40

LABEL_NAMES = {
    0: "idle", 1: "approach", 2: "hover", 3: "quick_pass", 4: "cover_hold",
    5: "confirmed_event", 6: "surface_reflection", 7: "noise", 8: "unknown",
}
NAME_TO_LABEL = {v: k for k, v in LABEL_NAMES.items()}


def load_csv(filepath: Path) -> List[dict]:
    """Load dataset from CSV produced by extract_windows.py."""
    rows = []
    with open(filepath, "r") as f:
        reader = csv.DictReader(f)
        for row in reader:
            # Skip unlabeled
            if row["label_name"] in ("unlabeled", "invalid"):
                continue
            # Parse proximity values
            prox = []
            for i in range(WINDOW_SIZE):
                prox.append(int(row[f"prox_{i}"]))
            row["proximity_values"] = prox
            row["session_id"] = int(row["session_id"])
            row["label_int"] = int(row["label"])
            rows.append(row)
    return rows


def extract_features(prox: List[int]) -> Dict[str, float]:
    """Extract the same features used by InferenceService.extractFeatures()."""
    n = len(prox)
    if n == 0:
        return {k: 0.0 for k in ["mean", "stddev", "min", "max", "peak_to_peak",
                                   "mean_deriv", "std_deriv", "dwell_above",
                                   "time_to_peak", "energy"]}

    mean = sum(prox) / n
    min_val = min(prox)
    max_val = max(prox)
    max_idx = prox.index(max_val)

    variance = sum((v - mean) ** 2 for v in prox) / n
    stddev = variance ** 0.5

    # First derivative
    derivs = [prox[i] - prox[i - 1] for i in range(1, n)]
    mean_deriv = sum(derivs) / len(derivs) if derivs else 0.0
    deriv_var = (sum((d - mean_deriv) ** 2 for d in derivs) / len(derivs)) if derivs else 0.0
    std_deriv = max(0, deriv_var) ** 0.5

    midpoint = (min_val + max_val) / 2.0
    dwell_count = sum(1 for v in prox if v >= midpoint)

    return {
        "mean": mean,
        "stddev": stddev,
        "min": min_val,
        "max": max_val,
        "peak_to_peak": max_val - min_val,
        "mean_deriv": mean_deriv,
        "std_deriv": std_deriv,
        "dwell_above": dwell_count / n,
        "time_to_peak": max_idx / (n - 1) if n > 1 else 0.0,
        "energy": sum((v - mean) ** 2 for v in prox),
    }


def baseline_predict(features: Dict[str, float]) -> Tuple[str, float]:
    """
    Rule-based classifier using physical proximity signal intuition.

    Returns (label_name, confidence).
    """
    mean = features["mean"]
    stddev = features["stddev"]
    ptp = features["peak_to_peak"]
    dwell = features["dwell_above"]
    mean_deriv = features["mean_deriv"]
    std_deriv = features["std_deriv"]

    # Normalize to typical sensor range (0-10000 active range)
    norm_mean = mean / 10000.0
    norm_stddev = stddev / 10000.0
    norm_ptp = ptp / 10000.0

    # Rule cascade (order matters: most specific first)

    # Cover hold: very high sustained signal
    if norm_mean > 0.5 and dwell > 0.7 and norm_stddev < 0.15:
        return "cover_hold", 0.8

    # Quick pass: high variability, brief spike
    if norm_ptp > 0.2 and norm_stddev > 0.1 and dwell < 0.4:
        return "quick_pass", 0.7

    # Hover: moderate-to-high mean, low variability (plateau)
    if norm_mean > 0.2 and norm_stddev < 0.08 and dwell > 0.5:
        return "hover", 0.7

    # Approach: positive trend (rising signal)
    if mean_deriv > 20 and norm_stddev > 0.03:
        return "approach", 0.6

    # Idle: nothing interesting
    if norm_mean < 0.15 and norm_stddev < 0.05:
        return "idle", 0.85

    # Default: idle with low confidence
    return "idle", 0.3


def session_split(data: List[dict], test_fraction: float = 0.3):
    """Split data by session ID (no data leakage from overlapping windows)."""
    sessions = sorted(set(row["session_id"] for row in data))
    n_test = max(1, int(len(sessions) * test_fraction))
    test_sessions = set(sessions[-n_test:])
    train_sessions = set(sessions[:-n_test])

    train = [r for r in data if r["session_id"] in train_sessions]
    test = [r for r in data if r["session_id"] in test_sessions]
    return train, test


def evaluate(data: List[dict], name: str = ""):
    """Evaluate baseline on data, print confusion matrix and metrics."""
    # Per-class counts
    classes = sorted(NAME_TO_LABEL.keys())
    tp = defaultdict(int)
    fp = defaultdict(int)
    fn = defaultdict(int)
    total = 0
    correct = 0

    confusion = defaultdict(lambda: defaultdict(int))

    for row in data:
        true_label = row["label_name"]
        features = extract_features(row["proximity_values"])
        pred_label, _ = baseline_predict(features)

        confusion[true_label][pred_label] += 1
        total += 1

        if pred_label == true_label:
            correct += 1
            tp[true_label] += 1
        else:
            fp[pred_label] += 1
            fn[true_label] += 1

    accuracy = correct / total if total > 0 else 0.0

    print(f"\n{'=' * 60}")
    print(f"  Baseline Evaluation: {name}")
    print(f"{'=' * 60}")
    print(f"  Total windows:  {total}")
    print(f"  Accuracy:       {accuracy:.3f} ({correct}/{total})")

    # Per-class metrics
    print(f"\n  {'Class':<12} {'Precision':>10} {'Recall':>10} {'F1':>10} {'Support':>10}")
    print(f"  {'-' * 52}")

    f1_scores = []
    for cls in classes:
        precision = tp[cls] / (tp[cls] + fp[cls]) if (tp[cls] + fp[cls]) > 0 else 0.0
        recall = tp[cls] / (tp[cls] + fn[cls]) if (tp[cls] + fn[cls]) > 0 else 0.0
        f1 = 2 * precision * recall / (precision + recall) if (precision + recall) > 0 else 0.0
        support = tp[cls] + fn[cls]
        f1_scores.append(f1)
        print(f"  {cls:<12} {precision:>10.3f} {recall:>10.3f} {f1:>10.3f} {support:>10}")

    macro_f1 = sum(f1_scores) / len(f1_scores) if f1_scores else 0.0
    print(f"\n  Macro F1:       {macro_f1:.3f}")

    # Confusion matrix
    print(f"\n  Confusion Matrix (rows=true, cols=predicted):")
    header = "  {:>12}".format("") + "".join(f"{c:>12}" for c in classes)
    print(header)
    for true_cls in classes:
        row_str = f"  {true_cls:>12}"
        for pred_cls in classes:
            row_str += f"{confusion[true_cls][pred_cls]:>12}"
        print(row_str)

    print(f"{'=' * 60}\n")
    return macro_f1


def main():
    parser = argparse.ArgumentParser(
        description="Train and evaluate rule baseline for gesture classification")
    parser.add_argument("dataset", type=Path, help="CSV dataset from extract_windows.py")
    parser.add_argument("--split-by", choices=["session", "random"], default="session",
                        help="Split strategy (session = no data leakage)")
    parser.add_argument("--test-fraction", type=float, default=0.3,
                        help="Fraction of sessions for test set")
    args = parser.parse_args()

    if not args.dataset.exists():
        print(f"Dataset not found: {args.dataset}", file=sys.stderr)
        sys.exit(1)

    data = load_csv(args.dataset)
    print(f"Loaded {len(data)} labeled windows")

    # Label distribution
    label_counts = defaultdict(int)
    for row in data:
        label_counts[row["label_name"]] += 1
    print("Label distribution:")
    for label, count in sorted(label_counts.items()):
        print(f"  {label}: {count}")

    # Split
    train, test = session_split(data, args.test_fraction)
    print(f"\nSplit: {len(train)} train, {len(test)} test")

    # Evaluate
    print("\n--- Training Set ---")
    evaluate(train, "Train")

    print("\n--- Test Set (held-out sessions) ---")
    test_f1 = evaluate(test, "Test")

    print(f"\nBaseline test macro-F1: {test_f1:.3f}")
    print("ML model must beat this score to earn the TinyML label.")


if __name__ == "__main__":
    main()
