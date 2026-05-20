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

    basename = os.path.splitext(os.path.basename(csv_path))[0]
    suffix = "_1000" if "_1000" in basename else ""

    # Parse variant from CSV filename
    variant_title = ""
    if basename.startswith("walkpath_"):
        label = basename[9:]  # strip 'walkpath_'
        if "nohistory" in label:
            variant_title = "Without History Search Feature"
        elif "history" in label:
            variant_title = "With History Search Feature"
        elif "big" in label:
            variant_title = "Big Motion Database"
        elif "small" in label:
            variant_title = "Small Motion Database"
        else:
            variant_title = f"Test Recording: {label}"
    elif basename.startswith("walkpath_1000_"):
        label = basename[14:]  # strip 'walkpath_1000_'
        if "nohistory" in label:
            variant_title = "Without History Search Feature (1000 frames)"
        elif "history" in label:
            variant_title = "With History Search Feature (1000 frames)"
        elif "big" in label:
            variant_title = "Big Motion Database (1000 frames)"
        elif "small" in label:
            variant_title = "Small Motion Database (1000 frames)"
        else:
            variant_title = f"Test Recording: {label} (1000 frames)"
    else:
        # Default fallback
        if "nohistory" in basename:
            variant_title = "Without History Search Feature"
        elif "history" in basename:
            variant_title = "With History Search Feature"
        elif "big" in basename:
            variant_title = "Big Motion Database"
        elif "small" in basename:
            variant_title = "Small Motion Database"
        else:
            variant_title = f"Test Recording: {basename}"

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
        segments = []
        curr_xs = []
        curr_zs = []
        curr_clip = None
        
        for r in rows:
            clip_val = int(r['clip_id']) if ('clip_id' in fieldnames and r['clip_id']) else 0
            x_val = to_float(r[x_col])
            z_val = to_float(r[z_col])
            
            if curr_clip is None:
                curr_clip = clip_val
            
            if clip_val != curr_clip:
                if curr_xs:
                    segments.append((curr_xs, curr_zs))
                curr_xs = [x_val]
                curr_zs = [z_val]
                curr_clip = clip_val
            else:
                curr_xs.append(x_val)
                curr_zs.append(z_val)
                
        if curr_xs:
            segments.append((curr_xs, curr_zs))
            
        first = True
        for seg_xs, seg_zs in segments:
            lbl = label if first else None
            ax.plot(seg_xs, seg_zs, label=lbl, color=color, linewidth=lw, linestyle=ls, alpha=0.8)
            first = False

        if is_frozen and segments and segments[0][0] and segments[0][1]:
            ax.plot([segments[0][0][0]], [segments[0][1][0]], marker='o', color='red', markersize=5)
        
        ax.set_xlabel('X (m)')
        ax.set_ylabel('Z (m)')
        ax.set_title(label)
        ax.grid(True, linestyle='--', alpha=0.4)
        
        ax.set_xlim(lim_min_x, lim_max_x)
        ax.set_ylim(lim_min_z, lim_max_z)
        ax.set_aspect('equal', adjustable='box')

    scribble_dir = os.path.join(walkpath_dir, 'scribble')
    os.makedirs(scribble_dir, exist_ok=True)
    out_path = os.path.join(scribble_dir, f"walkpath{suffix}.png")
    if variant_title:
        fig.suptitle(f"2D Trajectory Comparison\n[{variant_title}]", fontsize=12, fontweight='bold', y=0.98)
        plt.tight_layout(rect=[0, 0, 1, 0.9])
    else:
        plt.tight_layout()
    plt.savefig(out_path, dpi=200)
    plt.close()
    print(f"Saved plot: {out_path}")

    import math

    # --- Advanced Walkpath Metrics ---
    metrics_results = {
        'RTE (m)': {},
        'RRE (deg)': {},
        'ARE Full (deg)': {},
        'ARE 1s (deg)': {},
        'ARE 2s (deg)': {},
        'ARE 5s (deg)': {},
        'ADE Full (m)': {},
        'ADE 1s (m)': {},
        'ADE 2s (m)': {},
        'ADE 5s (m)': {}
    }
    
    # Store curves for plotting
    curves = {
        'RTE (m)': {},
        'RRE (deg)': {},
        'ARE Full (deg)': {},
        'ARE 1s (deg)': {},
        'ARE 2s (deg)': {},
        'ARE 5s (deg)': {},
        'ADE Full (m)': {},
        'ADE 1s (m)': {},
        'ADE 2s (m)': {},
        'ADE 5s (m)': {}
    }
    
    def angle_diff(a, b):
        diff = a - b
        while diff > math.pi:
            diff -= 2 * math.pi
        while diff < -math.pi:
            diff += 2 * math.pi
        return diff
    
    if 'gt_x' in fieldnames and 'gt_z' in fieldnames and 'gt_yaw' in fieldnames:
        gt_xs = [to_float(r['gt_x']) for r in rows]
        gt_zs = [to_float(r['gt_z']) for r in rows]
        gt_yaws = [to_float(r['gt_yaw']) for r in rows]
        
        clip_ids = []
        if 'clip_id' in fieldnames:
            clip_ids = [int(r['clip_id']) if r['clip_id'] else 0 for r in rows]
        
        for label, x_col, z_col, color, lw, ls, is_frozen in plots:
            if label == 'Ground Truth':
                continue
            
            yaw_col = x_col.replace('_x', '_yaw')
            if yaw_col not in fieldnames:
                continue
                
            xs = [to_float(r[x_col]) for r in rows]
            zs = [to_float(r[z_col]) for r in rows]
            yaws = [to_float(r[yaw_col]) for r in rows]
            
            # --- RTE, RRE & ARE/ADE Full ---
            rte_curve = []
            rre_curve = []
            are_full_curve = []
            ade_full_curve = []
            
            rte_sum = 0.0
            rre_sum = 0.0
            are_full_sum = 0.0
            ade_full_sum = 0.0
            
            valid_rte = 0
            valid_rre = 0
            valid_are_full = 0
            valid_ade_full = 0
            
            for t in range(len(gt_xs)):
                gx, gz, gy = gt_xs[t], gt_zs[t], gt_yaws[t]
                px, pz, py = xs[t], zs[t], yaws[t]
                
                # Check NaNs
                if math.isnan(gx) or math.isnan(px):
                    rte_curve.append(float('nan'))
                    rre_curve.append(float('nan'))
                    are_full_curve.append(float('nan'))
                    ade_full_curve.append(float('nan'))
                    continue
                
                # ADE Full (Euclidean distance per frame)
                err_ade = math.sqrt((gx - px)**2 + (gz - pz)**2)
                ade_full_curve.append(err_ade)
                ade_full_sum += err_ade
                valid_ade_full += 1
                
                # ARE Full (Global Angle diff)
                err_are = abs(angle_diff(gy, py))
                err_are_deg = math.degrees(err_are)
                are_full_curve.append(err_are_deg)
                are_full_sum += err_are_deg
                valid_are_full += 1
                
                # RTE & RRE
                if t > 0 and (not clip_ids or clip_ids[t] == clip_ids[t-1]):
                    gx_prev, gz_prev, gy_prev = gt_xs[t-1], gt_zs[t-1], gt_yaws[t-1]
                    px_prev, pz_prev, py_prev = xs[t-1], zs[t-1], yaws[t-1]
                    
                    # RRE
                    gt_rot_disp = angle_diff(gy, gy_prev)
                    p_rot_disp = angle_diff(py, py_prev)
                    err_rre = abs(angle_diff(p_rot_disp, gt_rot_disp))
                    err_rre_deg = math.degrees(err_rre)
                    
                    rre_curve.append(err_rre_deg)
                    rre_sum += err_rre_deg
                    valid_rre += 1
                    
                    # RTE
                    gt_disp_x = gx - gx_prev
                    gt_disp_z = gz - gz_prev
                    
                    p_disp_x = px - px_prev
                    p_disp_z = pz - pz_prev
                    
                    diff_disp_x = p_disp_x - gt_disp_x
                    diff_disp_z = p_disp_z - gt_disp_z
                    err_rte = math.sqrt(diff_disp_x**2 + diff_disp_z**2)
                        
                    rte_curve.append(err_rte)
                    rte_sum += err_rte
                    valid_rte += 1
                else:
                    rte_curve.append(float('nan'))
                    rre_curve.append(float('nan'))
                    
            if valid_rte > 0:
                metrics_results['RTE (m)'][label] = rte_sum / valid_rte
                curves['RTE (m)'][label] = rte_curve
            if valid_rre > 0:
                metrics_results['RRE (deg)'][label] = rre_sum / valid_rre
                curves['RRE (deg)'][label] = rre_curve
            if valid_are_full > 0:
                metrics_results['ARE Full (deg)'][label] = are_full_sum / valid_are_full
                curves['ARE Full (deg)'][label] = are_full_curve
            if valid_ade_full > 0:
                metrics_results['ADE Full (m)'][label] = ade_full_sum / valid_ade_full
                curves['ADE Full (m)'][label] = ade_full_curve

            # --- Sliding Window Metrics ---
            def calc_sliding_window_metrics(W):
                curve_ade = [float('nan')] * len(gt_xs)
                curve_are = [float('nan')] * len(gt_xs)
                sum_ade = 0.0
                sum_are = 0.0
                valid_w = 0
                
                for t in range(len(gt_xs) - W):
                    if math.isnan(gt_xs[t]) or math.isnan(xs[t]):
                        continue
                        
                    if clip_ids and clip_ids[t] != clip_ids[t + W]:
                        continue
                        
                    gt_t_x, gt_t_z, gt_t_yaw = gt_xs[t], gt_zs[t], gt_yaws[t]
                    p_t_x, p_t_z, p_t_yaw = xs[t], zs[t], yaws[t]
                    
                    rot_diff = gt_t_yaw - p_t_yaw
                    cos_r = math.cos(rot_diff)
                    sin_r = math.sin(rot_diff)
                    
                    w_err_ade_sum = 0.0
                    w_err_are_sum = 0.0
                    w_valid = 0
                    
                    for tau in range(1, W + 1):
                        curr_t = t + tau
                        if math.isnan(gt_xs[curr_t]) or math.isnan(xs[curr_t]):
                            continue
                            
                        # ADE
                        p_off_x = xs[curr_t] - p_t_x
                        p_off_z = zs[curr_t] - p_t_z
                        
                        p_rot_x = p_off_x * cos_r + p_off_z * sin_r
                        p_rot_z = -p_off_x * sin_r + p_off_z * cos_r
                        
                        p_aligned_x = gt_t_x + p_rot_x
                        p_aligned_z = gt_t_z + p_rot_z
                        
                        gt_curr_x, gt_curr_z = gt_xs[curr_t], gt_zs[curr_t]
                        err_ade = math.sqrt((gt_curr_x - p_aligned_x)**2 + (gt_curr_z - p_aligned_z)**2)
                        
                        # ARE
                        p_aligned_yaw = yaws[curr_t] + rot_diff
                        gt_curr_yaw = gt_yaws[curr_t]
                        
                        err_are = abs(angle_diff(gt_curr_yaw, p_aligned_yaw))
                        err_are_deg = math.degrees(err_are)
                        
                        w_err_ade_sum += err_ade
                        w_err_are_sum += err_are_deg
                        w_valid += 1
                        
                    if w_valid > 0:
                        window_ade = w_err_ade_sum / w_valid
                        window_are = w_err_are_sum / w_valid
                        curve_ade[t] = window_ade
                        curve_are[t] = window_are
                        sum_ade += window_ade
                        sum_are += window_are
                        valid_w += 1
                        
                if valid_w > 0:
                    return sum_ade / valid_w, curve_ade, sum_are / valid_w, curve_are
                return None, None, None, None

            metrics_1s = calc_sliding_window_metrics(60)
            if metrics_1s[0] is not None:
                metrics_results['ADE 1s (m)'][label] = metrics_1s[0]
                curves['ADE 1s (m)'][label] = metrics_1s[1]
                metrics_results['ARE 1s (deg)'][label] = metrics_1s[2]
                curves['ARE 1s (deg)'][label] = metrics_1s[3]
                
            metrics_2s = calc_sliding_window_metrics(120)
            if metrics_2s[0] is not None:
                metrics_results['ADE 2s (m)'][label] = metrics_2s[0]
                curves['ADE 2s (m)'][label] = metrics_2s[1]
                metrics_results['ARE 2s (deg)'][label] = metrics_2s[2]
                curves['ARE 2s (deg)'][label] = metrics_2s[3]
                
            metrics_5s = calc_sliding_window_metrics(300)
            if metrics_5s[0] is not None:
                metrics_results['ADE 5s (m)'][label] = metrics_5s[0]
                curves['ADE 5s (m)'][label] = metrics_5s[1]
                metrics_results['ARE 5s (deg)'][label] = metrics_5s[2]
                curves['ARE 5s (deg)'][label] = metrics_5s[3]

    # Generate Plots
    for metric_name, c_dict in curves.items():
        if not c_dict:
            continue
        plt.figure(figsize=(10, 4))
        for label, x_col, z_col, color, lw, ls, is_frozen in plots:
            if label in c_dict:
                curve = c_dict[label]
                score = metrics_results[metric_name][label]
                plt.plot(curve, label=f"{label} ({score:.4f})", color=color, alpha=0.8, linewidth=1.5, linestyle=ls)
                plt.axhline(y=score, color=color, linestyle=':', alpha=0.8)
                
        plt.xlabel('Frame')
        plt.ylabel(metric_name)
        if variant_title:
            plt.title(f'Per-Frame {metric_name}\n[{variant_title}]')
        else:
            plt.title(f'Per-Frame {metric_name}')
        plt.grid(True, linestyle='--', alpha=0.4)
        plt.legend()
        plt.tight_layout()
        fname = metric_name.lower().replace(' ', '_').replace('(', '').replace(')', '') + f"_histogram{suffix}.png"
        metric_lower = metric_name.lower()
        if 'ade' in metric_lower:
            subfolder = 'ade'
        elif 'are' in metric_lower:
            subfolder = 'are'
        elif 'rre' in metric_lower:
            subfolder = 'rre'
        elif 'rte' in metric_lower:
            subfolder = 'rte'
        else:
            subfolder = ''
        
        target_dir = os.path.join(walkpath_dir, subfolder) if subfolder else walkpath_dir
        os.makedirs(target_dir, exist_ok=True)
        plot_path = os.path.join(target_dir, fname)
        plt.savefig(plot_path, dpi=200)
        plt.close()
        print(f"Saved {metric_name} plot: {plot_path}")
        
    # Write Report
    report_path = os.path.join(walkpath_dir, f"walkpath_report{suffix}.md")
    with open(report_path, "w", encoding="utf-8") as rf:
        rf.write(f"# Walkpath Analysis Report{suffix.replace('_', ' ')}\n\n")
        
        # Build Table
        all_metrics = list(metrics_results.keys())
        header_row = "| Model | " + " | ".join(all_metrics) + " |\n"
        sep_row = "|---|" + "|".join(["---" for _ in all_metrics]) + "|\n"
        
        rf.write(header_row)
        rf.write(sep_row)
        
        for label, x_col, z_col, color, lw, ls, is_frozen in plots:
            if label == 'Ground Truth':
                continue
            row = f"| {label} |"
            has_data = False
            for m in all_metrics:
                if label in metrics_results[m]:
                    row += f" {metrics_results[m][label]:.6f} |"
                    has_data = True
                else:
                    row += " N/A |"
            row += "\n"
            if has_data:
                rf.write(row)
                
    print(f"Walkpath report created at: {report_path}")

    return 0

if __name__ == '__main__':
    raise SystemExit(main())
