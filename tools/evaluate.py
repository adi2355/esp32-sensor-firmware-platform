#!/usr/bin/env python3
"""
Evaluate ML model against rule baseline on held-out sessions.

This is the truth test: if the ML model does not beat the baseline,
it does not earn the TinyML label. Period.

Runs both the baseline (train_baseline.py rules) and the trained model
(train_model.py weights) on the same test set and compares:
  - Accuracy
  - Macro F1
  - Per-class F1
  - False positive rate on idle class
  - Robustness across sessions

Usage:
    python evaluate.py dataset.csv model_weights.json

Requirements:
    pip install numpy scikit-learn
"""

import argparse
import csv
import json
import sys
from pathlib import Path
from collections import defaultdict
from typing import List, Dict, Tuple

WINDOW_SIZE = 40
NUM_FEATURES = 10

LABEL_NAMES = {
    0: "idle", 1: "approach", 2: "hover", 3: "quick_pass", 4: "cover_hold",
    5: "confirmed_event", 6: "surface_reflection", 7: "noise", 8: "unknown",
}
NAME_TO_ID = {v: k for k, v in LABEL_NAMES.items()}

# Import feature extraction from train_model (same normalization)
PROX_NORM = 10000.0
DERIV_NORM = 1000.0
ENERGY_NORM = 1e8


def extract_features(prox: List[int]) -> List[float]:
    """Extract normalized features (identical to train_model.py and firmware)."""
    n = len(prox)
    if n == 0:
        return [0.0] * NUM_FEATURES

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
        mean / PROX_NORM,
        stddev / PROX_NORM,
        min_val / PROX_NORM,
        max_val / PROX_NORM,
        (max_val - min_val) / PROX_NORM,
        mean_deriv / DERIV_NORM,
        std_deriv / DERIV_NORM,
        dwell_count / n,
        max_idx / (n - 1) if n > 1 else 0.0,
        sum_sq_dev / ENERGY_NORM,
    ]


def baseline_predict(features_raw: Dict[str, float]) -> str:
    """Rule baseline (same as train_baseline.py)."""
    mean = features_raw["mean"]
    stddev = features_raw["stddev"]
    ptp = features_raw["peak_to_peak"]
    dwell = features_raw["dwell_above"]
    mean_deriv = features_raw["mean_deriv"]

    norm_mean = mean / 10000.0
    norm_stddev = stddev / 10000.0
    norm_ptp = ptp / 10000.0

    if norm_mean > 0.5 and dwell > 0.7 and norm_stddev < 0.15:
        return "cover_hold"
    if norm_ptp > 0.2 and norm_stddev > 0.1 and dwell < 0.4:
        return "quick_pass"
    if norm_mean > 0.2 and norm_stddev < 0.08 and dwell > 0.5:
        return "hover"
    if mean_deriv > 20 and norm_stddev > 0.03:
        return "approach"
    if norm_mean < 0.15 and norm_stddev < 0.05:
        return "idle"
    return "idle"


def model_predict(features_norm: List[float], weights: list, biases: list) -> str:
    """Linear model prediction (same as firmware InferenceService)."""
    import math

    logits = []
    for c in range(len(weights)):
        z = biases[c]
        for f in range(len(features_norm)):
            z += weights[c][f] * features_norm[f]
        logits.append(z)

    # Softmax
    max_logit = max(logits)
    exp_logits = [math.exp(l - max_logit) for l in logits]
    sum_exp = sum(exp_logits)
    probs = [e / sum_exp for e in exp_logits]

    best_idx = probs.index(max(probs))
    # CRITICAL: Use enum-order (by key), NOT alphabetical order (by value).
    # Model weights are indexed by GestureLabel enum value (0=idle, 1=approach, ...).
    # Alphabetical sort would map weight[0] (idle) to "approach" — silently wrong.
    class_names = [LABEL_NAMES[i] for i in sorted(LABEL_NAMES.keys())]
    return class_names[best_idx]


def compute_metrics(true_labels: list, pred_labels: list, name: str) -> float:
    """Compute and print classification metrics. Returns macro F1."""
    classes = sorted(set(true_labels) | set(pred_labels))
    tp = defaultdict(int)
    fp = defaultdict(int)
    fn = defaultdict(int)
    correct = 0

    for t, p in zip(true_labels, pred_labels):
        if t == p:
            correct += 1
            tp[t] += 1
        else:
            fp[p] += 1
            fn[t] += 1

    accuracy = correct / len(true_labels) if true_labels else 0.0

    print(f"\n  {name}")
    print(f"  {'=' * 55}")
    print(f"  Accuracy: {accuracy:.3f} ({correct}/{len(true_labels)})")
    print(f"\n  {'Class':<12} {'Prec':>8} {'Recall':>8} {'F1':>8} {'Support':>8}")
    print(f"  {'-' * 44}")

    f1_scores = []
    for cls in sorted(LABEL_NAMES.values()):
        prec = tp[cls] / (tp[cls] + fp[cls]) if (tp[cls] + fp[cls]) > 0 else 0.0
        rec = tp[cls] / (tp[cls] + fn[cls]) if (tp[cls] + fn[cls]) > 0 else 0.0
        f1 = 2 * prec * rec / (prec + rec) if (prec + rec) > 0 else 0.0
        sup = tp[cls] + fn[cls]
        f1_scores.append(f1)
        print(f"  {cls:<12} {prec:>8.3f} {rec:>8.3f} {f1:>8.3f} {sup:>8}")

    macro_f1 = sum(f1_scores) / len(f1_scores) if f1_scores else 0.0
    print(f"\n  Macro F1: {macro_f1:.3f}")

    # Idle false positive rate
    idle_fp = fp.get("idle", 0)
    non_idle_total = sum(1 for t in true_labels if t != "idle")
    idle_fpr = idle_fp / non_idle_total if non_idle_total > 0 else 0.0
    print(f"  Idle FPR: {idle_fpr:.3f} ({idle_fp}/{non_idle_total})")

    return macro_f1


def extract_raw_features(prox: List[int]) -> Dict[str, float]:
    """Extract unnormalized features for baseline."""
    n = len(prox)
    mean = sum(prox) / n
    min_val = min(prox)
    max_val = max(prox)
    max_idx = prox.index(max_val)
    sum_sq_dev = sum((v - mean) ** 2 for v in prox)
    stddev = (sum_sq_dev / n) ** 0.5
    derivs = [prox[i] - prox[i - 1] for i in range(1, n)]
    mean_deriv = sum(derivs) / len(derivs) if derivs else 0.0
    deriv_var = (sum((d - mean_deriv) ** 2 for d in derivs) / len(derivs)) if derivs else 0.0
    std_deriv = max(0.0, deriv_var) ** 0.5
    midpoint = (min_val + max_val) / 2.0
    dwell_count = sum(1 for v in prox if v >= midpoint)

    return {
        "mean": mean, "stddev": stddev, "min": min_val, "max": max_val,
        "peak_to_peak": max_val - min_val, "mean_deriv": mean_deriv,
        "std_deriv": std_deriv, "dwell_above": dwell_count / n,
        "time_to_peak": max_idx / (n - 1) if n > 1 else 0.0,
        "energy": sum_sq_dev,
    }


def main():
    parser = argparse.ArgumentParser(
        description="Compare ML model vs baseline on held-out sessions")
    parser.add_argument("dataset", type=Path, help="CSV dataset")
    parser.add_argument("model_weights", type=Path, help="JSON model weights")
    parser.add_argument("--test-fraction", type=float, default=0.3)
    args = parser.parse_args()

    # Load model weights
    with open(args.model_weights) as f:
        model = json.load(f)
    weights = model["weights"]
    biases = model["biases"]

    # Load dataset
    rows = []
    with open(args.dataset) as f:
        reader = csv.DictReader(f)
        for row in reader:
            if row["label_name"] not in NAME_TO_ID:
                continue
            prox = [int(row[f"prox_{i}"]) for i in range(WINDOW_SIZE)]
            row["proximity_values"] = prox
            row["session_id"] = int(row["session_id"])
            rows.append(row)

    # Session split
    sessions = sorted(set(r["session_id"] for r in rows))
    n_test = max(1, int(len(sessions) * args.test_fraction))
    test_sessions = set(sessions[-n_test:])
    test_data = [r for r in rows if r["session_id"] in test_sessions]

    print(f"Test set: {len(test_data)} windows from {n_test} sessions")

    # Run predictions
    true_labels = []
    baseline_preds = []
    model_preds = []

    for row in test_data:
        true_labels.append(row["label_name"])
        prox = row["proximity_values"]

        # Baseline
        raw_feat = extract_raw_features(prox)
        baseline_preds.append(baseline_predict(raw_feat))

        # Model
        norm_feat = extract_features(prox)
        model_preds.append(model_predict(norm_feat, weights, biases))

    # Evaluate both
    print("\n" + "=" * 60)
    print("  HEAD-TO-HEAD: ML Model vs Rule Baseline")
    print("=" * 60)

    baseline_f1 = compute_metrics(true_labels, baseline_preds, "Rule Baseline")
    model_f1 = compute_metrics(true_labels, model_preds, "ML Model")

    # Verdict
    print("\n" + "=" * 60)
    delta = model_f1 - baseline_f1
    if delta > 0:
        print(f"  VERDICT: ML wins by {delta:.3f} macro-F1")
        print(f"  The model earns the TinyML label.")
    elif delta == 0:
        print(f"  VERDICT: TIE (delta = 0.000)")
        print(f"  ML does not beat baseline. Use the baseline.")
    else:
        print(f"  VERDICT: Baseline wins by {-delta:.3f} macro-F1")
        print(f"  ML does NOT beat baseline. Do NOT claim TinyML.")
        print(f"  Publish the baseline and call it signal classification.")
    print("=" * 60)


if __name__ == "__main__":
    main()
