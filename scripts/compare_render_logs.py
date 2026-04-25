#!/usr/bin/env python3
import argparse
import math
import re
import statistics
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional


RENDER_RE = re.compile(
    r"【耗时分析】\s*frame_id=(?P<frame_id>-?\d+)\s*\|\s*上传\(CPU发GL\):\s*"
    r"(?P<upload_ms>-?\d+(?:\.\d+)?)\s*ms\s*\|\s*绘制\(draw\):\s*"
    r"(?P<draw_ms>-?\d+(?:\.\d+)?)\s*ms\s*\|\s*上传\+绘制:\s*"
    r"(?P<upload_draw_ms>-?\d+(?:\.\d+)?)\s*ms\s*\|\s*wall\(OnFrame入队→render结束\):\s*"
    r"(?P<wall_ms>-?\d+(?:\.\d+)?)\s*ms\s*\|\s*总\(Decode入口→render结束\):\s*"
    r"(?P<total_ms>-?\d+(?:\.\d+)?|—)"
)

ONFRAME_RE = re.compile(
    r"【耗时分析】\s*OnFrame\s*tracking_id=(?P<frame_id>-?\d+).*?"
    r"OnFrame整体:\s*(?P<onframe_ms>-?\d+(?:\.\d+)?)\s*ms"
)

MCE2E_RE = re.compile(
    r"【耗时分析】硬件解码总耗时\s*McE2E\s*tracking_id=(?P<frame_id>\d+).*?"
    r"e2e=(?P<e2e_us>\d+)us"
)


@dataclass
class MetricStats:
    count: int
    avg: float
    p50: float
    p95: float
    max: float


def percentile(values: List[float], p: float) -> float:
    if not values:
        return math.nan
    if len(values) == 1:
        return values[0]
    ordered = sorted(values)
    rank = (len(ordered) - 1) * p
    low = math.floor(rank)
    high = math.ceil(rank)
    if low == high:
        return ordered[low]
    weight = rank - low
    return ordered[low] * (1.0 - weight) + ordered[high] * weight


def summarize(values: List[float]) -> Optional[MetricStats]:
    if not values:
        return None
    return MetricStats(
        count=len(values),
        avg=statistics.fmean(values),
        p50=percentile(values, 0.50),
        p95=percentile(values, 0.95),
        max=max(values),
    )


def load_metrics(path: Path) -> Dict[str, List[float]]:
    metrics: Dict[str, List[float]] = {
        "render.upload_ms": [],
        "render.draw_ms": [],
        "render.upload_draw_ms": [],
        "render.wall_ms": [],
        "render.total_ms": [],
        "onframe.total_ms": [],
        "decode.mce2e_ms": [],
    }

    with path.open("r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            match = RENDER_RE.search(line)
            if match:
                metrics["render.upload_ms"].append(float(match.group("upload_ms")))
                metrics["render.draw_ms"].append(float(match.group("draw_ms")))
                metrics["render.upload_draw_ms"].append(float(match.group("upload_draw_ms")))
                metrics["render.wall_ms"].append(float(match.group("wall_ms")))
                total_text = match.group("total_ms")
                if total_text != "—":
                    metrics["render.total_ms"].append(float(total_text))
                continue

            match = ONFRAME_RE.search(line)
            if match:
                metrics["onframe.total_ms"].append(float(match.group("onframe_ms")))
                continue

            match = MCE2E_RE.search(line)
            if match:
                metrics["decode.mce2e_ms"].append(float(match.group("e2e_us")) / 1000.0)

    return metrics


def fmt(value: float) -> str:
    if math.isnan(value):
        return "-"
    return f"{value:.3f}"


def fmt_delta(candidate: float, baseline: float) -> str:
    if math.isnan(candidate) or math.isnan(baseline):
        return "-"
    delta = candidate - baseline
    if baseline == 0:
        return f"{delta:+.3f}"
    pct = (delta / baseline) * 100.0
    return f"{delta:+.3f} ({pct:+.1f}%)"


def print_single(label: str, stats_map: Dict[str, Optional[MetricStats]]) -> None:
    print(label)
    print(
        f"{'metric':28} {'count':>7} {'avg':>10} {'p50':>10} {'p95':>10} {'max':>10}"
    )
    for metric, stats in stats_map.items():
        if not stats:
            continue
        print(
            f"{metric:28} {stats.count:7d} {fmt(stats.avg):>10} {fmt(stats.p50):>10} "
            f"{fmt(stats.p95):>10} {fmt(stats.max):>10}"
        )


def print_compare(
    baseline_label: str,
    baseline_stats: Dict[str, Optional[MetricStats]],
    candidate_label: str,
    candidate_stats: Dict[str, Optional[MetricStats]],
) -> None:
    print(f"baseline:  {baseline_label}")
    print(f"candidate: {candidate_label}")
    print(
        f"{'metric':28} {'base avg':>10} {'cand avg':>10} {'delta avg':>18} "
        f"{'base p95':>10} {'cand p95':>10} {'delta p95':>18}"
    )
    for metric in baseline_stats.keys():
        base = baseline_stats.get(metric)
        cand = candidate_stats.get(metric)
        if not base and not cand:
            continue
        base_avg = base.avg if base else math.nan
        cand_avg = cand.avg if cand else math.nan
        base_p95 = base.p95 if base else math.nan
        cand_p95 = cand.p95 if cand else math.nan
        print(
            f"{metric:28} {fmt(base_avg):>10} {fmt(cand_avg):>10} "
            f"{fmt_delta(cand_avg, base_avg):>18} {fmt(base_p95):>10} "
            f"{fmt(cand_p95):>10} {fmt_delta(cand_p95, base_p95):>18}"
        )


def build_stat_map(metrics: Dict[str, List[float]]) -> Dict[str, Optional[MetricStats]]:
    ordered = [
        "decode.mce2e_ms",
        "onframe.total_ms",
        "render.upload_ms",
        "render.draw_ms",
        "render.upload_draw_ms",
        "render.wall_ms",
        "render.total_ms",
    ]
    return {name: summarize(metrics.get(name, [])) for name in ordered}


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Compare renderer timing logs between two runs."
    )
    parser.add_argument("candidate", help="new/current run log file")
    parser.add_argument(
        "--baseline",
        help="old/before run log file; if omitted, only summarize candidate",
    )
    args = parser.parse_args()

    candidate_path = Path(args.candidate)
    candidate_stats = build_stat_map(load_metrics(candidate_path))

    if not args.baseline:
        print_single(f"summary: {candidate_path}", candidate_stats)
        return

    baseline_path = Path(args.baseline)
    baseline_stats = build_stat_map(load_metrics(baseline_path))
    print_compare(str(baseline_path), baseline_stats, str(candidate_path), candidate_stats)


if __name__ == "__main__":
    main()
