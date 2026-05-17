"""
Simple plotting utility to read MPJPE CSV and save PNGs per metric.
Usage: python plot_mpjpe.py <csv_path> <out_dir>

Produces one PNG per metric column (excluding frame,time_seconds).
"""
from __future__ import annotations
import csv
import os
import sys
from typing import List

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
        print("Usage: python plot_mpjpe.py <csv_path> <out_dir>")
        return 2

    csv_path = sys.argv[1]
    out_dir = sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)

    pose_dir = os.path.join(out_dir, 'pose')
    path_dir = os.path.join(out_dir, 'path')
    os.makedirs(pose_dir, exist_ok=True)
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
        plt.title(metric)
        plt.grid(True, linestyle='--', alpha=0.4)
        out_path = os.path.join(pose_dir, f"{metric}.png")
        plt.tight_layout()
        plt.savefig(out_path, dpi=200)
        plt.close()
        print(f"Saved plot: {out_path}")

    return 0


if __name__ == '__main__':
    raise SystemExit(main())
