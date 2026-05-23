import os
import sys
import csv

try:
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    import numpy as np
except Exception as e:
    print("Error: matplotlib and numpy are required. Install with: pip install matplotlib numpy", file=sys.stderr)
    raise

def parse_markdown_table_v2(report_path):
    metrics = {}
    if not os.path.exists(report_path):
        return metrics
    with open(report_path, "r", encoding="utf-8") as f:
        lines = f.readlines()
    
    headers = []
    for line in lines:
        if line.startswith("| Model |") or line.startswith("|Model|"):
            headers = [h.strip() for h in line.split("|")[2:-1]]
        elif line.strip().startswith("|") and not line.strip().startswith("|---"):
            parts = [p.strip() for p in line.split("|")[1:-1]]
            if not parts:
                continue
            model_name = parts[0]
            if model_name in ['Model', 'Ground Truth']:
                continue
            values = parts[1:]
            for h, v in zip(headers, values):
                try:
                    metrics[(model_name, h)] = float(v)
                except ValueError:
                    metrics[(model_name, h)] = float('nan')
    return metrics

def main():
    if len(sys.argv) < 2:
        print("Usage: python plot_history_comparison.py <out_dir>")
        return 2

    out_dir = sys.argv[1]
    comparison_dir = os.path.join(out_dir, 'history_comparison')
    os.makedirs(comparison_dir, exist_ok=True)

    summary_csv = os.path.join(out_dir, 'history_metrics_summary.csv')
    nohistory_report = os.path.join(out_dir, 'walkpath', 'walkpath_report_nohistory.md')
    history_report = os.path.join(out_dir, 'walkpath', 'walkpath_report_history.md')

    # Default structure in case files aren't found
    data = {
        'nohistory': {'MM': {}, 'LMM': {}},
        'history': {'MM': {}, 'LMM': {}}
    }

    # 1. Read summary CSV
    if os.path.exists(summary_csv):
        with open(summary_csv, 'r', encoding='utf-8') as f:
            reader = csv.DictReader(f)
            for row in reader:
                var = row['variant'] # 'nohistory' or 'history'
                model = row['model'] # 'MM' or 'LMM'
                if var in data and model in data[var]:
                    data[var][model]['MPJPE (local)'] = float(row['mpjpe_local'])
                    if 'memory_static_mb' in row:
                        data[var][model]['Memory Static (MB)'] = float(row['memory_static_mb'])
                        data[var][model]['Memory Average (MB)'] = float(row['memory_avg_mb'])
                        data[var][model]['Memory Peak (MB)'] = float(row['memory_peak_mb'])
                    elif 'memory_mb' in row:
                        data[var][model]['Memory Static (MB)'] = float(row['memory_mb'])
                        data[var][model]['Memory Average (MB)'] = float('nan')
                        data[var][model]['Memory Peak (MB)'] = float('nan')
                    else:
                        data[var][model]['Memory Static (MB)'] = float('nan')
                        data[var][model]['Memory Average (MB)'] = float('nan')
                        data[var][model]['Memory Peak (MB)'] = float('nan')
                    data[var][model]['Time (ms)'] = float(row['time_ms'])
                    
                    # Parse detailed memory components if available
                    for col in ['mm_feat_tot', 'mm_feat_non_hist', 'mm_feat_hist', 'mm_anim_tot', 'mm_anim_pos', 'mm_anim_vel', 'mm_anim_rot', 'mm_anim_ang', 'mm_anim_cont', 'mm_anim_toe', 'mm_add_range', 'lmm_dec', 'lmm_step', 'lmm_proj']:
                        if col in row:
                            data[var][model][col] = float(row[col])

    # 2. Parse walkpath reports
    nohistory_walk_metrics = parse_markdown_table_v2(nohistory_report)
    history_walk_metrics = parse_markdown_table_v2(history_report)

    mapping = {
        'ADE Full (m)': 'ADE Full (m)',
        'ADE 1s (m)': 'ADE 1s (m)',
        'ADE 2s (m)': 'ADE 2s (m)',
        'ADE 5s (m)': 'ADE 5s (m)',
        'ARE Full (deg)': 'ARE Full (deg)',
        'ARE 1s (deg)': 'ARE 1s (deg)',
        'ARE 2s (deg)': 'ARE 2s (deg)',
        'ARE 5s (deg)': 'ARE 5s (deg)',
        'RTE': 'RTE (m)',
        'RTE (m)': 'RTE (m)',
        'RRE (deg)': 'RRE (deg)'
    }

    for src_key, dst_key in mapping.items():
        for model in ['MM', 'LMM']:
            if (model, src_key) in nohistory_walk_metrics:
                data['nohistory'][model][dst_key] = nohistory_walk_metrics[(model, src_key)]
            if (model, src_key) in history_walk_metrics:
                data['history'][model][dst_key] = history_walk_metrics[(model, src_key)]

    # 3. Create plots
    metrics_to_plot = [
        ('MPJPE (local)', 'MPJPE Local', 'lower is better'),
        ('Time (ms)', 'Execution Time (ms)', 'lower is better'),
        ('Memory Static (MB)', 'Static Memory Footprint (MB)', 'lower is better'),
        ('Memory Average (MB)', 'Dynamic Average RAM (MB)', 'lower is better'),
        ('Memory Peak (MB)', 'Dynamic Peak RAM (MB)', 'lower is better'),
        ('ADE Full (m)', 'ADE Full (m)', 'lower is better'),
        ('ADE 1s (m)', 'ADE 1s (m)', 'lower is better'),
        ('ADE 2s (m)', 'ADE 2s (m)', 'lower is better'),
        ('ADE 5s (m)', 'ADE 5s (m)', 'lower is better'),
        ('ARE Full (deg)', 'ARE Full (deg)', 'lower is better'),
        ('ARE 1s (deg)', 'ARE 1s (deg)', 'lower is better'),
        ('ARE 2s (deg)', 'ARE 2s (deg)', 'lower is better'),
        ('ARE 5s (deg)', 'ARE 5s (deg)', 'lower is better'),
        ('RTE (m)', 'Relative Translation Error (m)', 'lower is better'),
        ('RRE (deg)', 'Relative Rotational Error (deg)', 'lower is better')
    ]

    # Setup HSL-tailored premium colors
    colors = {
        'MM': {'nohistory': '#3A86C8', 'history': '#1C4A7E'}, # Light blue and dark blue
        'LMM': {'nohistory': '#F77F00', 'history': '#D62828'} # Orange and dark red
    }

    # Generate Combined Plot (5x3 dashboard)
    fig, axes = plt.subplots(5, 3, figsize=(22, 30))
    axes = axes.flatten()

    for idx, (m_key, m_title, m_desc) in enumerate(metrics_to_plot):
        ax = axes[idx]
        
        # Data preparation
        mm_nohist = data['nohistory']['MM'].get(m_key, float('nan'))
        mm_hist = data['history']['MM'].get(m_key, float('nan'))
        lmm_nohist = data['nohistory']['LMM'].get(m_key, float('nan'))
        lmm_hist = data['history']['LMM'].get(m_key, float('nan'))

        x = np.arange(2)  # [MM, LMM]
        width = 0.35      # width of bars

        # Bar values
        nohist_vals = [mm_nohist, lmm_nohist]
        hist_vals = [mm_hist, lmm_hist]

        # Draw bars with two colors (no history vs with history) – MM and LMM share colors
        rects1 = ax.bar(x - width/2, nohist_vals, width, label='Without History', color='#3A86C8', edgecolor='black', linewidth=0.7, alpha=0.9)
        rects2 = ax.bar(x + width/2, hist_vals, width, label='With History', color='#1C4A7E', edgecolor='black', linewidth=0.7, alpha=0.9)

        ax.set_ylabel(m_title, fontsize=12, fontweight='bold')
        ax.set_title(f"{m_title}\n({m_desc})", fontsize=13, fontweight='bold', pad=8)
        ax.set_xticks(x)
        ax.set_xticklabels(['Motion Matching (MM)', 'Learned Motion Matching (LMM)'], fontsize=10, fontweight='bold')
        
        # Simplified legend with only two entries
        from matplotlib.patches import Patch
        legend_elements = [
            Patch(facecolor='#3A86C8', edgecolor='black', linewidth=0.7, alpha=0.9, label='Without History'),
            Patch(facecolor='#1C4A7E', edgecolor='black', linewidth=0.7, alpha=0.9, label='With History')
        ]
        ax.legend(handles=legend_elements, frameon=True, facecolor='white', edgecolor='none')

        # Add values on top of bars
        def autolabel(rects):
            for rect in rects:
                height = rect.get_height()
                if not np.isnan(height):
                    ax.annotate(f'{height:.4f}',
                                xy=(rect.get_x() + rect.get_width() / 2, height),
                                xytext=(0, 3),  # 3 points vertical offset
                                textcoords="offset points",
                                ha='center', va='bottom', fontsize=9, fontweight='bold')

        autolabel(rects1)
        autolabel(rects2)

    plt.suptitle("History Feature Impact Analysis Dashboard (MM vs. LMM)", fontsize=22, fontweight='bold', y=0.98)
    plt.tight_layout(rect=[0, 0, 1, 0.95])
    
    dashboard_path = os.path.join(comparison_dir, 'history_metrics_comparison.png')
    plt.savefig(dashboard_path, dpi=200)
    plt.close()
    print(f"Saved combined comparison dashboard: {dashboard_path}")

    # Generate individual premium plots for each metric
    for m_key, m_title, m_desc in metrics_to_plot:
        fig, ax = plt.subplots(figsize=(10, 6))

        mm_nohist = data['nohistory']['MM'].get(m_key, float('nan'))
        mm_hist = data['history']['MM'].get(m_key, float('nan'))
        lmm_nohist = data['nohistory']['LMM'].get(m_key, float('nan'))
        lmm_hist = data['history']['LMM'].get(m_key, float('nan'))

        x = np.arange(2)
        width = 0.35

        rects1 = ax.bar(x - width/2, [mm_nohist, lmm_nohist], width, label='Without History', color='#3A86C8', edgecolor='black', linewidth=0.7, alpha=0.9)
        rects2 = ax.bar(x + width/2, [mm_hist, lmm_hist], width, label='With History', color='#1C4A7E', edgecolor='black', linewidth=0.7, alpha=0.9)

        ax.set_ylabel(m_title, fontsize=12, fontweight='bold')
        ax.set_title(f"{m_title} - Impact of History Feature", fontsize=15, fontweight='bold', pad=15)
        ax.set_xticks(x)
        ax.set_xticklabels(['Motion Matching (MM)', 'Learned Motion Matching (LMM)'], fontsize=12, fontweight='bold')
        ax.grid(True, linestyle='--', alpha=0.3)
        
        from matplotlib.patches import Patch
        legend_elements_ind = [
            Patch(facecolor='#3A86C8', edgecolor='black', linewidth=0.7, alpha=0.9, label='Without History'),
            Patch(facecolor='#1C4A7E', edgecolor='black', linewidth=0.7, alpha=0.9, label='With History')
        ]
        ax.legend(handles=legend_elements_ind, frameon=True, facecolor='white', edgecolor='none', fontsize=11)

        # Label bars
        for rect in rects1:
            height = rect.get_height()
            if not np.isnan(height):
                ax.annotate(f'{height:.4f}', xy=(rect.get_x() + rect.get_width() / 2, height), xytext=(0, 3), textcoords="offset points", ha='center', va='bottom', fontsize=11, fontweight='bold')
        for rect in rects2:
            height = rect.get_height()
            if not np.isnan(height):
                ax.annotate(f'{height:.4f}', xy=(rect.get_x() + rect.get_width() / 2, height), xytext=(0, 3), textcoords="offset points", ha='center', va='bottom', fontsize=11, fontweight='bold')

        plt.tight_layout()
        fname = m_key.lower().replace(' ', '_').replace('(', '').replace(')', '') + "_comparison.png"
        individual_path = os.path.join(comparison_dir, fname)
        plt.savefig(individual_path, dpi=200)
        plt.close()
        print(f"Saved individual plot: {individual_path}")

    # 4. Generate Comprehensive Report in Markdown
    report_path = os.path.join(comparison_dir, "history_analysis_report.md")
    with open(report_path, "w", encoding="utf-8") as rf:
        rf.write("# History Feature Comparison Analysis Report\n\n")
        rf.write("This report analyzes how the inclusion of the **History Feature** affects trajectory tracking accuracy, system memory, search execution speed, and path divergence (ADE, ARE, RTE, RRE) in both **Motion Matching (MM)** and **Learned Motion Matching (LMM)**.\n\n")

        rf.write("## 1. Metrics Comparison Summary\n\n")
        
        headers = ["Model & Configuration", "MPJPE (local)", "Static Footprint (MB)", "Avg RAM (MB)", "Peak RAM (MB)", "Execution Time (ms)", "ADE Full (m)", "ADE 1s (m)", "ADE 2s (m)", "ADE 5s (m)", "ARE Full (deg)", "ARE 1s (deg)", "ARE 2s (deg)", "ARE 5s (deg)", "RRE (deg)", "RTE (m)"]
        rf.write("| " + " | ".join(headers) + " |\n")
        rf.write("|" + "|".join(["---" for _ in headers]) + "|\n")

        def fmt(val):
            return f"{val:.6f}" if not np.isnan(val) else "N/A"

        configs = [
            ("MM (Without History)", 'nohistory', 'MM'),
            ("MM (With History)", 'history', 'MM'),
            ("LMM (Without History)", 'nohistory', 'LMM'),
            ("LMM (With History)", 'history', 'LMM')
        ]

        for label, var, model in configs:
            m_local = data[var][model].get('MPJPE (local)', float('nan'))
            m_stat = data[var][model].get('Memory Static (MB)', float('nan'))
            m_avg = data[var][model].get('Memory Average (MB)', float('nan'))
            m_peak = data[var][model].get('Memory Peak (MB)', float('nan'))
            m_time = data[var][model].get('Time (ms)', float('nan'))
            m_ade = data[var][model].get('ADE Full (m)', float('nan'))
            m_ade_1s = data[var][model].get('ADE 1s (m)', float('nan'))
            m_ade_2s = data[var][model].get('ADE 2s (m)', float('nan'))
            m_ade_5s = data[var][model].get('ADE 5s (m)', float('nan'))
            m_are = data[var][model].get('ARE Full (deg)', float('nan'))
            m_are_1s = data[var][model].get('ARE 1s (deg)', float('nan'))
            m_are_2s = data[var][model].get('ARE 2s (deg)', float('nan'))
            m_are_5s = data[var][model].get('ARE 5s (deg)', float('nan'))
            m_rre = data[var][model].get('RRE (deg)', float('nan'))
            m_rte = data[var][model].get('RTE (m)', float('nan'))

            rf.write(f"| {label} | {fmt(m_local)} | {fmt(m_stat)} | {fmt(m_avg)} | {fmt(m_peak)} | {fmt(m_time)} | {fmt(m_ade)} | {fmt(m_ade_1s)} | {fmt(m_ade_2s)} | {fmt(m_ade_5s)} | {fmt(m_are)} | {fmt(m_are_1s)} | {fmt(m_are_2s)} | {fmt(m_are_5s)} | {fmt(m_rre)} | {fmt(m_rte)} |\n")

        rf.write("\n## 2. Key Findings & Discussion\n\n")
        
        # Compute exact percentage improvements/impacts
        mm_ade_no = data['nohistory']['MM'].get('ADE Full (m)', float('nan'))
        mm_ade_yes = data['history']['MM'].get('ADE Full (m)', float('nan'))
        lmm_ade_no = data['nohistory']['LMM'].get('ADE Full (m)', float('nan'))
        lmm_ade_yes = data['history']['LMM'].get('ADE Full (m)', float('nan'))

        rf.write("### Trajectory Accuracy & Drift (ADE & RTE)\n")
        if not np.isnan(mm_ade_no) and not np.isnan(mm_ade_yes):
            mm_improve = ((mm_ade_no - mm_ade_yes) / mm_ade_no) * 100.0
            verb = "improves" if mm_improve > 0 else "degrades"
            rf.write(f"- **Motion Matching**: The addition of history search features {verb} the Average Displacement Error (ADE) by **{abs(mm_improve):.2f}%** (from {mm_ade_no:.4f}m to {mm_ade_yes:.4f}m).\n")
        if not np.isnan(lmm_ade_no) and not np.isnan(lmm_ade_yes):
            lmm_improve = ((lmm_ade_no - lmm_ade_yes) / lmm_ade_no) * 100.0
            verb = "improves" if lmm_improve > 0 else "degrades"
            rf.write(f"- **Learned Motion Matching**: Training the neural network with history features {verb} the ADE by **{abs(lmm_improve):.2f}%** (from {lmm_ade_no:.4f}m to {lmm_ade_yes:.4f}m).\n")

        mm_mem_no = data['nohistory']['MM'].get('Memory Static (MB)', float('nan'))
        mm_mem_yes = data['history']['MM'].get('Memory Static (MB)', float('nan'))
        lmm_mem_no = data['nohistory']['LMM'].get('Memory Static (MB)', float('nan'))
        lmm_mem_yes = data['history']['LMM'].get('Memory Static (MB)', float('nan'))

        rf.write("\n### Memory footprint / component sizes:\n")
        if not np.isnan(mm_mem_no) and not np.isnan(mm_mem_yes):
            mm_mem_diff = mm_mem_yes - mm_mem_no
            rf.write(f"- **Motion Matching**: Enabling history columns in the search database increases MM memory usage by **{mm_mem_diff:.4f} MB** (from {mm_mem_no:.2f} MB to {mm_mem_yes:.2f} MB) due to the extra 23 database feature dimensions.\n")
        if not np.isnan(lmm_mem_no) and not np.isnan(lmm_mem_yes):
            lmm_mem_diff = lmm_mem_yes - lmm_mem_no
            rf.write(f"- **Learned Motion Matching**: The neural network trained with history (`_history.bin` models) increases memory usage by **{lmm_mem_diff:.4f} MB** (from {lmm_mem_no:.4f} MB to {lmm_mem_yes:.4f} MB) because of the larger input/output layers matching the 68 feature dimensions.\n")

        mm_peak_no = data['nohistory']['MM'].get('Memory Peak (MB)', float('nan'))
        mm_peak_yes = data['history']['MM'].get('Memory Peak (MB)', float('nan'))
        lmm_peak_no = data['nohistory']['LMM'].get('Memory Peak (MB)', float('nan'))
        lmm_peak_yes = data['history']['LMM'].get('Memory Peak (MB)', float('nan'))

        rf.write("\n### Dynamic RAM consumption:\n")
        if not np.isnan(mm_peak_no) and not np.isnan(mm_peak_yes):
            rf.write(f"- **Motion Matching**: The peak dynamic RAM consumption during execution went from **{mm_peak_no:.2f} MB** (without history) to **{mm_peak_yes:.2f} MB** (with history).\n")
        if not np.isnan(lmm_peak_no) and not np.isnan(lmm_peak_yes):
            rf.write(f"- **Learned Motion Matching**: The peak dynamic RAM consumption went from **{lmm_peak_no:.2f} MB** (without history) to **{lmm_peak_yes:.2f} MB** (with history).\n")

        # Detailed Memory Component Comparison Section
        rf.write("\n## 3. Memory Component Breakdown Comparison\n\n")
        rf.write("Here is the detailed side-by-side breakdown of the memory components (in MB) for both configurations:\n\n")
        
        # Table of MM memory components
        rf.write("### Motion Matching (MM) Memory Components (MB)\n\n")
        headers_mm = ["Component", "Without History", "With History", "Difference"]
        rf.write("| " + " | ".join(headers_mm) + " |\n")
        rf.write("|" + "|".join(["---" for _ in headers_mm]) + "|\n")
        
        def write_mm_comp_row(label, col_key):
            v_no = data['nohistory']['MM'].get(col_key, 0.0)
            v_yes = data['history']['MM'].get(col_key, 0.0)
            diff = v_yes - v_no
            rf.write(f"| {label} | {v_no:.6f} | {v_yes:.6f} | {diff:+.6f} |\n")

        write_mm_comp_row("**Total Static Memory (with additional)**", 'Memory Static (MB)')
        write_mm_comp_row("   - db.features (Total)", 'mm_feat_tot')
        write_mm_comp_row("      - non history columns", 'mm_feat_non_hist')
        write_mm_comp_row("      - history columns", 'mm_feat_hist')
        write_mm_comp_row("   - Animation Database (Total)", 'mm_anim_tot')
        write_mm_comp_row("      - bone_positions", 'mm_anim_pos')
        write_mm_comp_row("      - bone_velocities", 'mm_anim_vel')
        write_mm_comp_row("      - bone_rotations", 'mm_anim_rot')
        write_mm_comp_row("      - bone_angular_velocities", 'mm_anim_ang')
        write_mm_comp_row("      - contact_states", 'mm_anim_cont')
        write_mm_comp_row("      - future_toe_positions", 'mm_anim_toe')
        write_mm_comp_row("   - additional (range starts/stops)", 'mm_add_range')

        rf.write("\n### Learned Motion Matching (LMM) Memory Components (MB)\n\n")
        headers_lmm = ["Component", "Without History", "With History", "Difference"]
        rf.write("| " + " | ".join(headers_lmm) + " |\n")
        rf.write("|" + "|".join(["---" for _ in headers_lmm]) + "|\n")

        def write_lmm_comp_row(label, col_key):
            v_no = data['nohistory']['LMM'].get(col_key, 0.0)
            v_yes = data['history']['LMM'].get(col_key, 0.0)
            diff = v_yes - v_no
            rf.write(f"| {label} | {v_no:.6f} | {v_yes:.6f} | {diff:+.6f} |\n")

        write_lmm_comp_row("**Total Static Network Memory**", 'Memory Static (MB)')
        write_lmm_comp_row("   - D (Decompressor weights)", 'lmm_dec')
        write_lmm_comp_row("   - S (Stepper weights)", 'lmm_step')
        write_lmm_comp_row("   - P (Projector weights)", 'lmm_proj')

        rf.write("\n## 4. Visualization Dashboard\n\n")
        rf.write("![History Impact Summary Dashboard](history_metrics_comparison.png)\n")

    print(f"Comprehensive history comparison report generated: {report_path}")
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
