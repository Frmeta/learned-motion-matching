"""
Simple plotting utility to read MPJPE CSV and save PNGs per metric.
Usage: python plot_mpjpe.py <csv_path> <out_dir> [--ymax=<value>]

Produces one PNG per metric column (excluding frame,time_seconds).
Optional --ymax=<value> enforces a shared y-axis upper limit across all plots
(useful for big-vs-small database comparisons).
"""
from __future__ import annotations
import csv
import os
import sys
from typing import List, Optional

try:
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
except Exception as e:
    print("Error: matplotlib is required to generate plots. Install with: pip install matplotlib", file=sys.stderr)
    raise


def read_csv(path: str):
    with open(path, 'r', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        rows = list(reader)
        fieldnames = reader.fieldnames if reader.fieldnames else []
    return fieldnames, rows


def to_float(v: str):
    try:
        return float(v)
    except Exception:
        return float('nan')


def main():
    if len(sys.argv) < 3:
        print("Usage: python plot_mpjpe.py <csv_path> <out_dir> [--ymax=<value>]")
        return 2

    csv_path = sys.argv[1]
    out_dir = sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)

    # Parse optional --ymax argument
    forced_ymax: Optional[float] = None
    for arg in sys.argv[3:]:
        if arg.startswith('--ymax='):
            try:
                forced_ymax = float(arg[len('--ymax='):])
            except ValueError:
                print(f"Warning: could not parse --ymax value '{arg}', ignoring.", file=sys.stderr)

    mpjpe_dir = os.path.join(out_dir, 'mpjpe')
    path_dir = os.path.join(out_dir, 'walkpath')
    os.makedirs(mpjpe_dir, exist_ok=True)
    os.makedirs(path_dir, exist_ok=True)

    fieldnames, rows = read_csv(csv_path)
    if not fieldnames or not rows:
        print("No data to plot.")
        return 1

    # Expect 'frame' and 'time_seconds' as first two columns
    time_key = None
    if 'time_seconds' in fieldnames:
        time_key = 'time_seconds'
    elif 'frame' in fieldnames:
        time_key = 'frame'

    metrics = [k for k in fieldnames if k not in ('frame', 'time_seconds')]

    times = [to_float(r[time_key]) if time_key and r.get(time_key) is not None else i for i, r in enumerate(rows)]

    for metric in metrics:
        values = [to_float(r.get(metric, 'nan')) for r in rows]
        # Mask invalid values
        xs = []
        ys = []
        for t, v in zip(times, values):
            if v != v or v < 0.0:  # NaN or placeholder (-1.0)
                continue
            xs.append(t)
            ys.append(v)

        plt.figure(figsize=(10, 3))
        plt.plot(xs, ys, linewidth=1.0)
        
        if ys:
            mean_val = sum(ys) / len(ys)
            plt.axhline(y=mean_val, color='r', linestyle=':', label=f'Mean: {mean_val:.4f}')
            plt.legend()

        plt.xlabel('Time (s)')
        plt.ylabel('MPJPE (m)')
        
        # Map metric keys to highly descriptive names
        metric_descriptions = {
            "mm_local": "Motion Matching (MM) Root-Relative Pose Error (Local MPJPE)",
            "mm_world": "Motion Matching (MM) World Space Pose Error (World MPJPE)",
            "lmm_local": "Learned Motion Matching (LMM) Root-Relative Pose Error (Local MPJPE)",
            "lmm_world": "Learned Motion Matching (LMM) World Space Pose Error (World MPJPE)",
            "frozen_local": "Frozen Frame Baseline Root-Relative Pose Error (Local MPJPE)",
            "frozen_world": "Frozen Frame Baseline World Space Pose Error (World MPJPE)",
            "mm_lmm_local_diff": "Absolute Difference between MM and LMM (Local MPJPE)",
            "mm_lmm_world_diff": "Absolute Difference between MM and LMM (World MPJPE)",
        }
        
        desc_metric = metric_descriptions.get(metric, metric)
        
        # Parse variant from CSV filename
        csv_filename = os.path.basename(csv_path)
        variant_title = ""
        if csv_filename.endswith("_mpjpe.csv"):
            label = csv_filename[:-10]  # strip '_mpjpe.csv'
            if label == "nohistory":
                variant_title = "Without History Search Feature"
            elif label == "history":
                variant_title = "With History Search Feature"
            elif label == "big":
                variant_title = "Big Motion Database"
            elif label == "small":
                variant_title = "Small Motion Database"
            else:
                variant_title = f"Test Recording: {label}"
        
        if variant_title:
            full_title = f"{desc_metric}\n[{variant_title}]"
        else:
            full_title = desc_metric

        plt.title(full_title, fontsize=11, fontweight='bold', pad=10)
        plt.grid(True, linestyle='--', alpha=0.4)

        # Apply shared y-axis limit when provided
        if forced_ymax is not None:
            plt.ylim(0.0, forced_ymax)

        out_path = os.path.join(mpjpe_dir, f"{metric}.png")
        plt.tight_layout()
        plt.savefig(out_path, dpi=200)
        plt.close()
        print(f"Saved plot: {out_path}")

    return 0


if __name__ == '__main__':
    raise SystemExit(main())
