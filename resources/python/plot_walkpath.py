"""
Simple plotting utility to read walkpath CSV and save a 2D trajectory PNG.
Usage: python plot_walkpath.py <csv_path> <out_dir>
"""
import csv
import os
import sys

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
        print("Usage: python plot_walkpath.py <csv_path> <out_dir>")
        return 2

    csv_path = sys.argv[1]
    out_dir = sys.argv[2]
    
    walkpath_dir = os.path.join(out_dir, 'walkpath')
    os.makedirs(walkpath_dir, exist_ok=True)

    fieldnames, rows = read_csv(csv_path)
    if not fieldnames or not rows:
        print("No data to plot.")
        return 1

    plots = []
    if 'gt_x' in fieldnames and 'gt_z' in fieldnames:
        plots.append(('Ground Truth', 'gt_x', 'gt_z', 'black', 1.5, '-', False))
    if 'mm_x' in fieldnames and 'mm_z' in fieldnames:
        plots.append(('MM', 'mm_x', 'mm_z', 'blue', 1.0, '-', False))
    if 'lmm_x' in fieldnames and 'lmm_z' in fieldnames:
        plots.append(('LMM', 'lmm_x', 'lmm_z', 'green', 1.0, '-', False))
    if 'frozen_x' in fieldnames and 'frozen_z' in fieldnames:
        plots.append(('Frozen', 'frozen_x', 'frozen_z', 'red', 2.0, '--', True))

    n_plots = len(plots)
    if n_plots == 0:
        print("No valid coordinates found in CSV.")
        return 1

    min_x = float('inf')
    max_x = float('-inf')
    min_z = float('inf')
    max_z = float('-inf')

    for label, x_col, z_col, color, lw, ls, is_frozen in plots:
        valid_xs = [to_float(r[x_col]) for r in rows if r[x_col]]
        valid_zs = [to_float(r[z_col]) for r in rows if r[z_col]]
        valid_xs = [x for x in valid_xs if x == x]
        valid_zs = [z for z in valid_zs if z == z]
        if valid_xs:
            min_x = min(min_x, min(valid_xs))
            max_x = max(max_x, max(valid_xs))
        if valid_zs:
            min_z = min(min_z, min(valid_zs))
            max_z = max(max_z, max(valid_zs))
            
    range_x = max_x - min_x
    range_z = max_z - min_z
    max_range = max(range_x, range_z)
    if max_range == 0: 
        max_range = 1.0
    
    mid_x = (max_x + min_x) / 2.0
    mid_z = (max_z + min_z) / 2.0
    
    lim_min_x = mid_x - (max_range / 2.0) * 1.05
    lim_max_x = mid_x + (max_range / 2.0) * 1.05
    lim_min_z = mid_z - (max_range / 2.0) * 1.05
    lim_max_z = mid_z + (max_range / 2.0) * 1.05

    fig, axes = plt.subplots(1, n_plots, figsize=(4 * n_plots, 4))
    if n_plots == 1:
        axes = [axes]
    
    for ax, (label, x_col, z_col, color, lw, ls, is_frozen) in zip(axes, plots):
        xs = [to_float(r[x_col]) for r in rows]
        zs = [to_float(r[z_col]) for r in rows]
        ax.plot(xs, zs, label=label, color=color, linewidth=lw, linestyle=ls, alpha=0.8)
        if is_frozen and xs and zs:
            ax.plot([xs[0]], [zs[0]], marker='o', color='red', markersize=5)
        
        ax.set_xlabel('X (m)')
        ax.set_ylabel('Z (m)')
        ax.set_title(label)
        ax.grid(True, linestyle='--', alpha=0.4)
        
        ax.set_xlim(lim_min_x, lim_max_x)
        ax.set_ylim(lim_min_z, lim_max_z)
        ax.set_aspect('equal', adjustable='box')

    out_path = os.path.join(walkpath_dir, "walkpath.png")
    plt.tight_layout()
    plt.savefig(out_path, dpi=200)
    plt.close()
    print(f"Saved plot: {out_path}")

    import math

    # --- ATE Calculation ---
    ate_results = {}
    ate_curves = {}
    
    if 'gt_x' in fieldnames and 'gt_z' in fieldnames:
        gt_xs = [to_float(r['gt_x']) for r in rows]
        gt_zs = [to_float(r['gt_z']) for r in rows]
        
        for label, x_col, z_col, color, lw, ls, is_frozen in plots:
            if label == 'Ground Truth':
                continue
            
            xs = [to_float(r[x_col]) for r in rows]
            zs = [to_float(r[z_col]) for r in rows]
            
            curve = []
            sum_sq_err = 0.0
            valid_count = 0
            
            for g_x, g_z, p_x, p_z in zip(gt_xs, gt_zs, xs, zs):
                if g_x == g_x and g_z == g_z and p_x == p_x and p_z == p_z: # Check not NaN
                    err_sq = (g_x - p_x)**2 + (g_z - p_z)**2
                    err = math.sqrt(err_sq)
                    curve.append(err)
                    sum_sq_err += err_sq
                    valid_count += 1
                else:
                    curve.append(float('nan'))
            
            if valid_count > 0:
                ate = math.sqrt(sum_sq_err / valid_count)
                ate_results[label] = ate
                ate_curves[label] = curve

    # Plot ATE Histogram
    if ate_curves:
        plt.figure(figsize=(10, 4))
        for label, x_col, z_col, color, lw, ls, is_frozen in plots:
            if label in ate_curves:
                curve = ate_curves[label]
                ate = ate_results[label]
                plt.plot(curve, label=f"{label} (ATE: {ate:.4f})", color=color, alpha=0.8, linewidth=1.5, linestyle=ls)
                plt.axhline(y=ate, color=color, linestyle=':', alpha=0.8)
                
        plt.xlabel('Frame')
        plt.ylabel('Absolute Trajectory Error (m)')
        plt.title('Per-Frame Absolute Trajectory Error (ATE)')
        plt.grid(True, linestyle='--', alpha=0.4)
        plt.legend()
        plt.tight_layout()
        ate_path = os.path.join(walkpath_dir, "ate_histogram.png")
        plt.savefig(ate_path, dpi=200)
        plt.close()
        print(f"Saved ATE plot: {ate_path}")
        
        # Write walkpath_report.md
        report_path = os.path.join(walkpath_dir, "walkpath_report.md")
        with open(report_path, "w", encoding="utf-8") as rf:
            rf.write("# Walkpath Analysis Report\n\n")
            rf.write("## Absolute Trajectory Error (ATE)\n\n")
            rf.write("The Absolute Trajectory Error (ATE) measures the Root Mean Square Error (RMSE) between the predicted and ground truth root trajectories.\n\n")
            rf.write("| Model | ATE (m) |\n")
            rf.write("|---|---|\n")
            for label, x_col, z_col, color, lw, ls, is_frozen in plots:
                if label in ate_results:
                    rf.write(f"| {label} | {ate_results[label]:.6f} |\n")
        print(f"Walkpath report created at: {report_path}")

    return 0

if __name__ == '__main__':
    raise SystemExit(main())
