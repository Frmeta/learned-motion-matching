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
        print("Usage: python plot_big_small_comparison.py <out_dir>")
        return 2

    out_dir = sys.argv[1]
    comparison_dir = os.path.join(out_dir, 'big_small_comparison')
    os.makedirs(comparison_dir, exist_ok=True)

    summary_csv = os.path.join(out_dir, 'big_small_metrics_summary.csv')
    big_report = os.path.join(out_dir, 'walkpath', 'walkpath_report_big.md')
    small_report = os.path.join(out_dir, 'walkpath', 'walkpath_report_small.md')

    # Default structure in case files aren't found
    data = {
        'big': {'MM': {}, 'LMM': {}},
        'small': {'MM': {}, 'LMM': {}}
    }

    # 1. Read summary CSV
    if os.path.exists(summary_csv):
        with open(summary_csv, 'r', encoding='utf-8') as f:
            reader = csv.DictReader(f)
            for row in reader:
                var = row['variant'] # 'big' or 'small'
                model = row['model'] # 'MM' or 'LMM'
                if var in data and model in data[var]:
                    data[var][model]['MPJPE (local)'] = float(row['mpjpe_local'])
                    data[var][model]['MPJPE (world)'] = float(row['mpjpe_world'])
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
    big_walk_metrics = parse_markdown_table_v2(big_report)
    small_walk_metrics = parse_markdown_table_v2(small_report)

    mapping = {
        'ADE Full (m)': 'ADE Full (m)',
        'ADE 1s (m)': 'ADE 1s (m)',
        'ADE 2s (m)': 'ADE 2s (m)',
        'ADE 5s (m)': 'ADE 5s (m)',
        'ARE Full (deg)': 'ARE Full (deg)',
        'ARE 1s (deg)': 'ARE 1s (deg)',
        'ARE 2s (deg)': 'ARE 2s (deg)',
        'ARE 5s (deg)': 'ARE 5s (deg)',
        'RTE (m)': 'RTE (m)',
        'RTE': 'RTE (m)',
        'RRE (deg)': 'RRE (deg)'
    }

    for src_key, dst_key in mapping.items():
        for model in ['MM', 'LMM']:
            if (model, src_key) in big_walk_metrics:
                data['big'][model][dst_key] = big_walk_metrics[(model, src_key)]
            if (model, src_key) in small_walk_metrics:
                data['small'][model][dst_key] = small_walk_metrics[(model, src_key)]

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

    fig, axes = plt.subplots(5, 3, figsize=(22, 30))
    axes = axes.flatten()

    for idx, (m_key, m_title, m_desc) in enumerate(metrics_to_plot):
        ax = axes[idx]

        mm_big = data['big']['MM'].get(m_key, float('nan'))
        mm_small = data['small']['MM'].get(m_key, float('nan'))
        lmm_big = data['big']['LMM'].get(m_key, float('nan'))
        lmm_small = data['small']['LMM'].get(m_key, float('nan'))

        # Updated bar plotting: groups = Small then Big, colors differentiate MM vs LMM
        x = np.arange(2)  # [Small, Big]
        width = 0.35      # width of bars

        # Draw bars: MM (blue) and LMM (orange) within each group
        rects_mm = ax.bar(x - width/2, [mm_small, mm_big], width, label='Motion Matching (MM)', color='#3A86C8', edgecolor='black', linewidth=0.7, alpha=0.9)
        rects_lmm = ax.bar(x + width/2, [lmm_small, lmm_big], width, label='Learned Motion Matching (LMM)', color='#F77F00', edgecolor='black', linewidth=0.7, alpha=0.9)

        ax.set_ylabel(m_title, fontsize=12, fontweight='bold')
        ax.set_title(f"{m_title}\n({m_desc})", fontsize=13, fontweight='bold', pad=8)
        ax.set_xticks(x)
        ax.set_xticklabels(['Small Database', 'Big Database'], fontsize=10, fontweight='bold')
        ax.grid(True, linestyle='--', alpha=0.3)
        
        # Simplified legend showing algorithm colors only
        from matplotlib.patches import Patch
        legend_elements = [
            Patch(facecolor='#3A86C8', edgecolor='black', linewidth=0.7, alpha=0.9, label='Motion Matching (MM)'),
            Patch(facecolor='#F77F00', edgecolor='black', linewidth=0.7, alpha=0.9, label='Learned Motion Matching (LMM)')
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

        autolabel(rects_mm)
        autolabel(rects_lmm)

    plt.suptitle("Big vs. Small Database Impact Analysis Dashboard (MM vs. LMM)", fontsize=22, fontweight='bold', y=0.98)
    plt.tight_layout(rect=[0, 0, 1, 0.95])
    
    dashboard_path = os.path.join(comparison_dir, 'big_small_metrics_comparison.png')
    plt.savefig(dashboard_path, dpi=200)
    plt.close()
    print(f"Saved combined comparison dashboard: {dashboard_path}")

    # Generate individual premium plots for each metric
    for m_key, m_title, m_desc in metrics_to_plot:
        fig, ax = plt.subplots(figsize=(10, 6))

        mm_big = data['big']['MM'].get(m_key, float('nan'))
        mm_small = data['small']['MM'].get(m_key, float('nan'))
        lmm_big = data['big']['LMM'].get(m_key, float('nan'))
        lmm_small = data['small']['LMM'].get(m_key, float('nan'))

        x = np.arange(2)
        width = 0.35

        rects_mm = ax.bar(x - width/2, [mm_small, mm_big], width, label='Motion Matching (MM)', color='#3A86C8', edgecolor='black', linewidth=0.7, alpha=0.9)
        rects_lmm = ax.bar(x + width/2, [lmm_small, lmm_big], width, label='Learned Motion Matching (LMM)', color='#F77F00', edgecolor='black', linewidth=0.7, alpha=0.9)

        ax.set_ylabel(m_title, fontsize=12, fontweight='bold')
        ax.set_title(f"{m_title} - Impact of Database Size", fontsize=15, fontweight='bold', pad=15)
        ax.set_xticks(x)
        ax.set_xticklabels(['Small Database', 'Big Database'], fontsize=12, fontweight='bold')
        ax.grid(True, linestyle='--', alpha=0.3)
        
        from matplotlib.patches import Patch
        legend_elements_ind = [
            Patch(facecolor='#3A86C8', edgecolor='black', linewidth=0.7, alpha=0.9, label='Motion Matching (MM)'),
            Patch(facecolor='#F77F00', edgecolor='black', linewidth=0.7, alpha=0.9, label='Learned Motion Matching (LMM)')
        ]
        ax.legend(handles=legend_elements_ind, frameon=True, facecolor='white', edgecolor='none', fontsize=11)

        # Label bars
        for rect in rects_mm:
            height = rect.get_height()
            if not np.isnan(height):
                ax.annotate(f'{height:.4f}', xy=(rect.get_x() + rect.get_width() / 2, height), xytext=(0, 3), textcoords="offset points", ha='center', va='bottom', fontsize=11, fontweight='bold')
        for rect in rects_lmm:
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
    report_path = os.path.join(comparison_dir, 'big_small_analysis_report.md')
    with open(report_path, 'w', encoding='utf-8') as f:
        f.write("# Big vs. Small Database Comparison Report\n\n")
        f.write("This report analyzes the impact of motion database size on the performance, quality, and memory characteristics of both standard **Motion Matching (MM)** and **Learned Motion Matching (LMM)**.\n\n")
        
        f.write("## 1. Metrics Dashboard Summary Table\n\n")
        f.write("| Category | Metric | MM (Big) | MM (Small) | LMM (Big) | LMM (Small) |\n")
        f.write("|---|---|---|---|---|---|\n")

        for m_key, m_title, m_desc in metrics_to_plot:
            mm_b = data['big']['MM'].get(m_key, float('nan'))
            mm_s = data['small']['MM'].get(m_key, float('nan'))
            lmm_b = data['big']['LMM'].get(m_key, float('nan'))
            lmm_s = data['small']['LMM'].get(m_key, float('nan'))
            
            f.write(f"| {m_desc.capitalize()} | {m_title} | {mm_b:.6f} | {mm_s:.6f} | {lmm_b:.6f} | {lmm_s:.6f} |\n")

        f.write("\n")
        f.write("## 2. Memory Footprint Breakdown\n\n")
        f.write("### Motion Matching (MM) Component Memory\n\n")
        f.write("| Database Size | Features Total (MB) | Animation DB (MB) | Total Static (MB) |\n")
        f.write("|---|---|---|---|\n")
        for var in ['big', 'small']:
            mm_feat = data[var]['MM'].get('mm_feat_tot', 0.0)
            mm_anim = data[var]['MM'].get('mm_anim_tot', 0.0)
            mm_stat = data[var]['MM'].get('Memory Static (MB)', 0.0)
            f.write(f"| {var.capitalize()} | {mm_feat:.3f} | {mm_anim:.3f} | {mm_stat:.3f} |\n")

        f.write("\n")
        f.write("### Learned Motion Matching (LMM) Network Parameters Memory\n\n")
        f.write("| Database Size | Decompressor (MB) | Stepper (MB) | Projector (MB) | Total Network (MB) |\n")
        f.write("|---|---|---|---|---|\n")
        for var in ['big', 'small']:
            l_dec = data[var]['LMM'].get('lmm_dec', 0.0)
            l_step = data[var]['LMM'].get('lmm_step', 0.0)
            l_proj = data[var]['LMM'].get('lmm_proj', 0.0)
            l_tot = data[var]['LMM'].get('Memory Static (MB)', 0.0)
            f.write(f"| {var.capitalize()} | {l_dec:.3f} | {l_step:.3f} | {l_proj:.3f} | {l_tot:.3f} |\n")

        f.write("\n")
        f.write("## 3. Visualization Dashboard\n\n")
        f.write("A visual dashboard compiling all comparative metrics is available at:\n")
        f.write(f"![Big vs. Small Comparison Dashboard](big_small_metrics_comparison.png)\n")

    print(f"Saved big/small analysis markdown report: {report_path}")
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
