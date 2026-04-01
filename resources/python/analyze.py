from __future__ import annotations

import re
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Optional


ROOT_DIR = Path(__file__).resolve().parents[2]
INPUT_FOLDER = ROOT_DIR / "resources" / "input-recording"
SCORE_DIR = ROOT_DIR / "score"


@dataclass
class ModeStats:
    bpje: float
    frames: int
    joints: int
    mm_time_ms: float
    lmm_time_ms: float
    mm_mem_delta_mb: Optional[float] = None
    lmm_mem_delta_mb: Optional[float] = None
    mm_mem_peak_mb: Optional[float] = None
    lmm_mem_peak_mb: Optional[float] = None
    mm_mem_avg_mb: Optional[float] = None
    lmm_mem_avg_mb: Optional[float] = None


@dataclass
class CsvStats:
    file_name: str
    both: Optional[ModeStats] = None


REPORT_LINE_RE = re.compile(
    r"(?P<filename>[\w-]+\.csv) \| MPJPE=(?P<mpjpe>[-+0-9.eE]+) \| "
    r"frames=(?P<frames>\d+) \| joints=(?P<joints>\d+) \| "
    r"time_ms MM=(?P<mm_time>[-+0-9.eE]+) LMM=(?P<lmm_time>[-+0-9.eE]+)"
    r"(?: \| mem_delta_mb MM=(?P<mm_delta>[-+0-9.eE]+) LMM=(?P<lmm_delta>[-+0-9.eE]+) "
    r"\| mem_peak_mb MM=(?P<mm_peak>[-+0-9.eE]+) LMM=(?P<lmm_peak>[-+0-9.eE]+)"
    r"(?: \| mem_avg_mb MM=(?P<mm_avg>[-+0-9.eE]+) LMM=(?P<lmm_avg>[-+0-9.eE]+))?)?$")


def run_command(args: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, cwd=str(cwd), check=True, capture_output=True, text=True)


def build_project() -> None:
    print("Building project with make...")
    try:
        run_command(["make"], ROOT_DIR)
    except subprocess.CalledProcessError as exc:
        controller_exe = ROOT_DIR / "controller.exe"
        if not controller_exe.exists():
            raise

        print("make failed, but controller.exe already exists. Continuing with the existing executable.", file=sys.stderr)
        if exc.stdout:
            print(exc.stdout, end="")
        if exc.stderr:
            print(exc.stderr, end="", file=sys.stderr)


def list_csv_files(folder: Path) -> list[Path]:
    if not folder.exists():
        raise FileNotFoundError(f"Input folder does not exist: {folder}")

    csv_files = sorted(p for p in folder.iterdir() if p.is_file() and p.suffix.lower() == ".csv")
    if not csv_files:
        raise FileNotFoundError(f"No CSV files found in: {folder}")
    return csv_files


def latest_report(prefix: str) -> Optional[Path]:
    candidates = sorted(
        SCORE_DIR.glob(f"{prefix}*.txt"),
        key=lambda path: path.stat().st_mtime,
        reverse=True,
    )
    return candidates[0] if candidates else None


def parse_mode_report(report_path: Path) -> ModeStats:
    text = report_path.read_text(encoding="utf-8", errors="replace").splitlines()
    for line in text:
        stripped = line.strip()
        match = REPORT_LINE_RE.match(stripped)
        if match:
            return ModeStats(
                bpje=float(match.group("mpjpe")),
                frames=int(match.group("frames")),
                joints=int(match.group("joints")),
                mm_time_ms=float(match.group("mm_time")),
                lmm_time_ms=float(match.group("lmm_time")),
                mm_mem_delta_mb=float(match.group("mm_delta")) if match.group("mm_delta") is not None else None,
                lmm_mem_delta_mb=float(match.group("lmm_delta")) if match.group("lmm_delta") is not None else None,
                mm_mem_peak_mb=float(match.group("mm_peak")) if match.group("mm_peak") is not None else None,
                lmm_mem_peak_mb=float(match.group("lmm_peak")) if match.group("lmm_peak") is not None else None,
                mm_mem_avg_mb=float(match.group("mm_avg")) if match.group("mm_avg") is not None else None,
                lmm_mem_avg_mb=float(match.group("lmm_avg")) if match.group("lmm_avg") is not None else None,
            )

    raise ValueError(f"Could not parse analyze-both data from report: {report_path}")


def run_analyze_mode(mode_flag: str, csv_path: Path) -> Path:
    before = {path.name for path in SCORE_DIR.glob("*.txt")}
    completed = run_command([str(ROOT_DIR / "controller.exe"), mode_flag, f"--input={csv_path}"], ROOT_DIR)

    if completed.stdout:
        print(completed.stdout, end="")
    if completed.stderr:
        print(completed.stderr, end="", file=sys.stderr)

    after = sorted(SCORE_DIR.glob("*.txt"), key=lambda path: path.stat().st_mtime, reverse=True)
    for path in after:
        if path.name not in before:
            return path

    guessed_prefix = "both_" if mode_flag == "--analyze-both" else "mm_"
    report = latest_report(guessed_prefix)
    if report is None:
        raise FileNotFoundError(f"No report file found after running {mode_flag} for {csv_path}")
    return report


def safe_delete(path: Path) -> None:
    try:
        path.unlink()
    except FileNotFoundError:
        pass


def analyze_csv(csv_path: Path) -> CsvStats:
    result = CsvStats(file_name=csv_path.name)

    report = run_analyze_mode("--analyze-both", csv_path)
    try:
        result.both = parse_mode_report(report)
    finally:
        safe_delete(report)

    return result


def write_combined_report(results: list[CsvStats], output_path: Path, input_folder: Path) -> None:
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    bpje_values: list[float] = []
    mm_time_values: list[float] = []
    lmm_time_values: list[float] = []
    frame_counts: list[int] = []
    mm_mem_delta_values: list[float] = []
    lmm_mem_delta_values: list[float] = []
    mm_mem_peak_values: list[float] = []
    lmm_mem_peak_values: list[float] = []
    mm_mem_avg_values: list[float] = []
    lmm_mem_avg_values: list[float] = []

    lines: list[str] = []
    lines.append("MM vs LMM analysis (BPJE)")
    lines.append(f"input folder: {input_folder.as_posix()}")
    lines.append(f"generated: {timestamp}")
    lines.append("")

    for entry in results:
        if entry.both is None:
            lines.append(f"{entry.file_name} | FAILED | incomplete comparison data")
            continue

        both = entry.both
        line = f"{entry.file_name} | BPJE={both.bpje:.6e} | frames={both.frames} | joints={both.joints}"
        line += f" | time_ms MM={both.mm_time_ms:.3f} LMM={both.lmm_time_ms:.3f}"
        if all(v is not None for v in (both.mm_mem_delta_mb, both.lmm_mem_delta_mb, both.mm_mem_peak_mb, both.lmm_mem_peak_mb)):
            line += (
                f" | mem_delta_mb MM={both.mm_mem_delta_mb:.3f} LMM={both.lmm_mem_delta_mb:.3f}"
                f" | mem_peak_mb MM={both.mm_mem_peak_mb:.3f} LMM={both.lmm_mem_peak_mb:.3f}"
            )
        if both.mm_mem_avg_mb is not None and both.lmm_mem_avg_mb is not None:
            line += f" | mem_avg_mb MM={both.mm_mem_avg_mb:.3f} LMM={both.lmm_mem_avg_mb:.3f}"
        lines.append(line)
        bpje_values.append(both.bpje)
        mm_time_values.append(both.mm_time_ms)
        lmm_time_values.append(both.lmm_time_ms)
        frame_counts.append(both.frames)
        if both.mm_mem_delta_mb is not None and both.lmm_mem_delta_mb is not None:
            mm_mem_delta_values.append(both.mm_mem_delta_mb)
            lmm_mem_delta_values.append(both.lmm_mem_delta_mb)
        if both.mm_mem_peak_mb is not None and both.lmm_mem_peak_mb is not None:
            mm_mem_peak_values.append(both.mm_mem_peak_mb)
            lmm_mem_peak_values.append(both.lmm_mem_peak_mb)
        if both.mm_mem_avg_mb is not None and both.lmm_mem_avg_mb is not None:
            mm_mem_avg_values.append(both.mm_mem_avg_mb)
            lmm_mem_avg_values.append(both.lmm_mem_avg_mb)

    lines.append("")
    if bpje_values:
        lines.append(f"Average BPJE: {sum(bpje_values) / len(bpje_values):.6e} (across {len(bpje_values)} files)")
    else:
        lines.append("Average BPJE: N/A")

    if mm_time_values and lmm_time_values and frame_counts:
        mm_time_total = sum(mm_time_values)
        lmm_time_total = sum(lmm_time_values)
        total_frames = sum(frame_counts)
        if total_frames > 0:
            lines.append(f"Average Time (ms/frame): MM={mm_time_total / total_frames:.6f} LMM={lmm_time_total / total_frames:.6f}")
        else:
            lines.append("Average Time (ms/frame): N/A")
        lines.append(f"Total Time (ms): MM={mm_time_total:.3f} LMM={lmm_time_total:.3f}")
        lines.append(f"Total Frames: {total_frames}")
    else:
        lines.append("Average Time (ms/frame): N/A")

    if mm_mem_delta_values and lmm_mem_delta_values and mm_mem_peak_values and lmm_mem_peak_values:
        lines.append(
            f"Average Memory Delta (MB): MM={sum(mm_mem_delta_values) / len(mm_mem_delta_values):.3f} "
            f"LMM={sum(lmm_mem_delta_values) / len(lmm_mem_delta_values):.3f}"
        )
        lines.append(
            f"Average Memory Peak (MB): MM={sum(mm_mem_peak_values) / len(mm_mem_peak_values):.3f} "
            f"LMM={sum(lmm_mem_peak_values) / len(lmm_mem_peak_values):.3f}"
        )
    else:
        lines.append("Average Memory Delta (MB): N/A")
        lines.append("Average Memory Peak (MB): N/A")

    if mm_mem_avg_values and lmm_mem_avg_values:
        lines.append(
            f"Average Memory Usage (MB): MM={sum(mm_mem_avg_values) / len(mm_mem_avg_values):.3f} "
            f"LMM={sum(lmm_mem_avg_values) / len(lmm_mem_avg_values):.3f}"
        )
    else:
        lines.append("Average Memory Usage (MB): N/A")

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    input_folder = INPUT_FOLDER
    if len(sys.argv) > 1:
        input_folder = Path(sys.argv[1]).expanduser().resolve()

    build_project()
    csv_files = list_csv_files(input_folder)

    results: list[CsvStats] = []
    for csv_path in csv_files:
        print(f"Analyzing {csv_path.name}...")
        results.append(analyze_csv(csv_path))

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    output_path = SCORE_DIR / f"combined_{timestamp}.txt"
    write_combined_report(results, output_path, input_folder)

    print(f"Combined analysis report exported to: {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())