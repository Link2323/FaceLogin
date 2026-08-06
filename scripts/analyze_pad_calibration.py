#!/usr/bin/env python3
"""Analyze FaceLogin PAD calibration CSV without third-party dependencies."""

from __future__ import annotations

import argparse
import csv
import math
import statistics
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


@dataclass(frozen=True)
class Row:
    split: str
    label: str
    subject: str
    session: str
    condition: str
    attempt: int
    score: float | None

    @property
    def live(self) -> bool:
        return self.label == "live"


def percentile(values: list[float], q: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return math.nan
    position = (len(ordered) - 1) * q
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def wilson(successes: int, total: int, z: float = 1.959963984540054) -> tuple[float, float]:
    if total == 0:
        return math.nan, math.nan
    p = successes / total
    denominator = 1.0 + z * z / total
    centre = (p + z * z / (2.0 * total)) / denominator
    margin = z * math.sqrt(p * (1.0 - p) / total + z * z / (4.0 * total * total)) / denominator
    return max(0.0, centre - margin), min(1.0, centre + margin)


def load_rows(
    paths: Iterable[Path], score_column: str = "score"
) -> tuple[list[Row], int]:
    rows: list[Row] = []
    missing_scores = 0
    seen_sessions: dict[str, tuple[str, str, str, str]] = {}
    seen_samples: set[tuple[str, int]] = set()
    for path in paths:
        with path.open("r", encoding="utf-8-sig", newline="") as handle:
            reader = csv.DictReader(handle)
            required = {
                "split", "label", "subject", "session", "condition", "attempt",
                "accepted_index", "status", score_column,
            }
            missing = required.difference(reader.fieldnames or [])
            if missing:
                raise ValueError(f"{path}: missing columns: {', '.join(sorted(missing))}")
            for line, record in enumerate(reader, 2):
                if record["status"] in {"duplicate", "no_face"}:
                    continue
                try:
                    raw_score = record[score_column].strip()
                    score = float(raw_score) if raw_score else None
                    accepted_index = int(record["accepted_index"])
                    row = Row(
                        split=record["split"].strip(),
                        label=record["label"].strip(),
                        subject=record["subject"].strip(),
                        session=record["session"].strip(),
                        condition=record["condition"].strip(),
                        attempt=int(record["attempt"]),
                        score=score,
                    )
                except (TypeError, ValueError) as exc:
                    raise ValueError(f"{path}:{line}: invalid row: {exc}") from exc
                if row.score is not None and (
                    not math.isfinite(row.score) or not 0.0 <= row.score <= 1.0
                ):
                    raise ValueError(f"{path}:{line}: score outside [0,1]")
                session_metadata = (row.split, row.label, row.subject, row.condition)
                previous = seen_sessions.setdefault(row.session, session_metadata)
                if previous != session_metadata:
                    raise ValueError(
                        f"session {row.session!r} is reused with different metadata"
                    )
                sample_key = (row.session, accepted_index)
                if sample_key in seen_samples:
                    raise ValueError(
                        f"{path}:{line}: duplicate sample index {accepted_index} "
                        f"in session {row.session!r}"
                    )
                seen_samples.add(sample_key)
                if row.score is None:
                    missing_scores += 1
                rows.append(row)
    return rows, missing_scores


def group_attempts(
    rows: list[Row], frames: int
) -> dict[tuple[str, str, str, str, int], list[float | None]]:
    grouped: dict[tuple[str, str, str, str, int], list[float | None]] = defaultdict(list)
    for row in rows:
        grouped[(row.split, row.label, row.subject, row.session, row.attempt)].append(row.score)
    return {key: scores for key, scores in grouped.items() if len(scores) == frames}


def decision_metrics(
    attempts: dict[tuple[str, str, str, str, int], list[float | None]],
    split: str,
    threshold: float,
    passes_required: int,
) -> tuple[int, int, int, int]:
    live_total = live_rejected = attack_total = attack_accepted = 0
    for key, scores in attempts.items():
        row_split, label = key[0], key[1]
        if row_split != split:
            continue
        accepted = sum(
            score is not None and score >= threshold for score in scores
        ) >= passes_required
        if label == "live":
            live_total += 1
            live_rejected += not accepted
        else:
            attack_total += 1
            attack_accepted += accepted
    return live_rejected, live_total, attack_accepted, attack_total


def frame_metrics(rows: list[Row], split: str, threshold: float) -> tuple[int, int, int, int]:
    live = [row for row in rows if row.split == split and row.live]
    attacks = [row for row in rows if row.split == split and not row.live]
    live_rejected = sum(row.score is None or row.score < threshold for row in live)
    attack_accepted = sum(
        row.score is not None and row.score >= threshold for row in attacks
    )
    return live_rejected, len(live), attack_accepted, len(attacks)


def show_distribution(rows: list[Row], split: str) -> None:
    print(f"\n[{split}] frame-score distributions")
    labels = sorted({row.label for row in rows if row.split == split}, key=lambda x: (x != "live", x))
    for label in labels:
        selected = [row for row in rows if row.split == split and row.label == label]
        values = [row.score for row in selected if row.score is not None]
        sessions = {row.session for row in rows if row.split == split and row.label == label}
        if not values:
            print(
                f"  {label:8s} n={len(selected):4d}, sessions={len(sessions):2d}, "
                f"scored=0, inference_errors={len(selected)}"
            )
            continue
        print(
            f"  {label:8s} n={len(selected):4d}, sessions={len(sessions):2d}, "
            f"scored={len(values):4d}, inference_errors={len(selected) - len(values):3d}, "
            f"min={min(values):.4f}, p05={percentile(values, .05):.4f}, "
            f"median={statistics.median(values):.4f}, p95={percentile(values, .95):.4f}, "
            f"max={max(values):.4f}"
        )


def select_threshold(
    rows: list[Row],
    attempts: dict[tuple[str, str, str, str, int], list[float | None]],
    split: str,
    target_apcer: float,
    max_bpcer: float,
    passes_required: int,
    threshold_step: float,
    threshold_min: float,
    threshold_max: float,
) -> float | None:
    steps = math.floor((threshold_max - threshold_min) / threshold_step)
    candidates = [threshold_min + i * threshold_step for i in range(steps + 1)]
    if candidates[-1] < threshold_max - 1e-12:
        candidates.append(threshold_max)
    valid: list[float] = []
    for threshold in candidates:
        live_rejected, live_total, attack_accepted, attack_total = frame_metrics(
            rows, split, threshold
        )
        if live_total == 0 or attack_total == 0:
            continue
        apcer = attack_accepted / attack_total
        bpcer = live_rejected / live_total
        window_live_rejected, window_live_total, window_attack_accepted, window_attack_total = (
            decision_metrics(attempts, split, threshold, passes_required)
        )
        if window_live_total == 0 or window_attack_total == 0:
            continue
        window_apcer = window_attack_accepted / window_attack_total
        window_bpcer = window_live_rejected / window_live_total
        if (
            apcer <= target_apcer
            and bpcer <= max_bpcer
            and window_apcer <= target_apcer
            and window_bpcer <= max_bpcer
        ):
            valid.append(threshold)
    if not valid:
        return None
    # Within the caller's error limits, prefer the strictest operating point.
    # This leaves the largest observed margin above attacks while bounding live
    # rejection at both frame and complete-window levels.
    return max(valid)


def report_frame_metrics(rows: list[Row], split: str, threshold: float) -> None:
    live_rejected, live_total, attack_accepted, attack_total = frame_metrics(
        rows, split, threshold
    )
    if live_total == 0 and attack_total == 0:
        return
    print(f"\n[{split}] frame decision at threshold {threshold:.6f}")
    if attack_total:
        lo, hi = wilson(attack_accepted, attack_total)
        print(
            f"  frame APCER: {attack_accepted}/{attack_total} = "
            f"{attack_accepted / attack_total:.2%} (95% Wilson {lo:.2%}..{hi:.2%})"
        )
    if live_total:
        lo, hi = wilson(live_rejected, live_total)
        print(
            f"  frame BPCER: {live_rejected}/{live_total} = "
            f"{live_rejected / live_total:.2%} (95% Wilson {lo:.2%}..{hi:.2%})"
        )


def report_metrics(
    attempts: dict[tuple[str, str, str, str, int], list[float | None]],
    split: str,
    threshold: float,
    passes_required: int,
) -> None:
    live_rejected, live_total, attack_accepted, attack_total = decision_metrics(
        attempts, split, threshold, passes_required
    )
    if live_total == 0 and attack_total == 0:
        return
    print(f"\n[{split}] attempt decision at threshold {threshold:.6f}")
    if attack_total:
        lo, hi = wilson(attack_accepted, attack_total)
        print(
            f"  APCER: {attack_accepted}/{attack_total} = {attack_accepted / attack_total:.2%} "
            f"(95% Wilson {lo:.2%}..{hi:.2%})"
        )
    if live_total:
        lo, hi = wilson(live_rejected, live_total)
        print(
            f"  BPCER: {live_rejected}/{live_total} = {live_rejected / live_total:.2%} "
            f"(95% Wilson {lo:.2%}..{hi:.2%})"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", nargs="+", type=Path, help="calibration CSV file(s)")
    parser.add_argument("--split", default="tune", help="split used to choose an exploratory threshold")
    parser.add_argument("--validation-split", default="validation")
    parser.add_argument("--frames-per-attempt", type=int, default=5)
    parser.add_argument("--passes-required", type=int, default=5)
    parser.add_argument(
        "--score-column", default="production_score",
        help=("CSV score column to analyze (default: production_score); "
              "diagnostics use minifas_v2_score, minifas_v1se_score, or "
              "minifas_ensemble_score"),
    )
    parser.add_argument(
        "--target-apcer", type=float, default=0.0,
        help="maximum frame-level attack acceptance on the tuning split (default: 0)",
    )
    parser.add_argument(
        "--max-bpcer", type=float, default=0.05,
        help="maximum frame-level live rejection on the tuning split (default: 0.05)",
    )
    parser.add_argument(
        "--threshold-min", type=float,
        help="lowest threshold to evaluate (default: 0.0)",
    )
    parser.add_argument(
        "--threshold-max", type=float,
        help="highest threshold to evaluate (default: 1.0)",
    )
    parser.add_argument(
        "--fixed-threshold", type=float,
        help="evaluate a locked threshold without selecting it from the input rows",
    )
    parser.add_argument(
        "--threshold-step", type=float, default=0.001,
        help="threshold search grid (default: 0.001)",
    )
    args = parser.parse_args()
    if not 1 <= args.passes_required <= args.frames_per_attempt:
        parser.error("--passes-required must be in [1, --frames-per-attempt]")
    if not 0.0 <= args.target_apcer <= 1.0:
        parser.error("--target-apcer must be in [0,1]")
    if not 0.0 <= args.max_bpcer <= 1.0:
        parser.error("--max-bpcer must be in [0,1]")
    if not 0.000001 <= args.threshold_step <= 0.1:
        parser.error("--threshold-step must be in [0.000001,0.1]")
    threshold_min = args.threshold_min
    threshold_max = args.threshold_max
    if threshold_min is None:
        threshold_min = 0.0
    if threshold_max is None:
        threshold_max = 1.0
    if not 0.0 <= threshold_min <= threshold_max <= 1.0:
        parser.error("threshold range must satisfy 0 <= min <= max <= 1")
    if args.fixed_threshold is not None and not 0.0 <= args.fixed_threshold <= 1.0:
        parser.error("--fixed-threshold must be in [0,1]")

    try:
        rows, missing_scores = load_rows(args.csv, args.score_column)
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    if not rows:
        print("error: no valid score rows", file=sys.stderr)
        return 2

    attempts = group_attempts(rows, args.frames_per_attempt)
    incomplete = len({(r.split, r.label, r.subject, r.session, r.attempt) for r in rows}) - len(attempts)
    print(
        f"Score column: {args.score_column}\n"
        + (
            f"Locked threshold evaluation: {args.fixed_threshold:.6f}\n"
            if args.fixed_threshold is not None
            else (
                f"Threshold search: [{threshold_min:.3f}, {threshold_max:.3f}], "
                f"target APCER <= {args.target_apcer:.2%}, "
                f"BPCER <= {args.max_bpcer:.2%}\n"
            )
        )
        + f"Loaded {len(rows)} detected face frames ({missing_scores} inference errors) "
        f"and {len(attempts)} complete "
        f"{args.frames_per_attempt}-frame attempts; ignored {incomplete} incomplete attempts."
    )
    for split in sorted({row.split for row in rows}):
        show_distribution(rows, split)

    if args.fixed_threshold is not None:
        threshold = args.fixed_threshold
        print(
            f"\nEvaluating locked threshold {threshold:.6f}; input rows do not "
            "change this operating point."
        )
    else:
        tune_live = [
            row.score for row in rows
            if row.split == args.split and row.live and row.score is not None
        ]
        tune_attack = [
            row.score for row in rows
            if row.split == args.split and not row.live and row.score is not None
        ]
        if not tune_live or not tune_attack:
            print(f"\nNo recommendation: split {args.split!r} needs both live and attack sessions.")
            return 1

        max_attack = max(tune_attack)
        min_live = min(tune_live)
        print(
            f"\nTuning overlap check: min live={min_live:.4f}, max attack={max_attack:.4f} "
            f"=> {'OVERLAP' if max_attack >= min_live else 'separated in this sample'}"
        )
        threshold = select_threshold(
            rows, attempts, args.split, args.target_apcer, args.max_bpcer,
            args.passes_required, args.threshold_step, threshold_min, threshold_max,
        )
        if threshold is None:
            print(
                "No usable threshold in the allowed range meets both the requested "
                "frame and complete-window APCER/BPCER limits."
            )
            return 1

        print(
            f"\nStrictest exploratory threshold on {args.split!r} satisfying both "
            f"frame and complete-window APCER/BPCER limits: {threshold:.6f}"
        )
    print(
        f"Decision rule: at least {args.passes_required}/{args.frames_per_attempt} "
        f"unique frames must have score >= threshold."
    )
    report_frame_metrics(rows, args.split, threshold)
    report_metrics(attempts, args.split, threshold, args.passes_required)
    report_frame_metrics(rows, args.validation_split, threshold)
    report_metrics(attempts, args.validation_split, threshold, args.passes_required)

    validation_attempts = [key for key in attempts if key[0] == args.validation_split]
    if not validation_attempts:
        print(
            "\nWARNING: this is not a deployable threshold yet. Capture independent "
            "validation sessions (new session/day/person/attack presentation)."
        )
    print(
        "\nTreat attempts from the same session as correlated. Zero observed attack "
        "accepts on a small sample does not prove zero real-world APCER; broaden "
        "devices, lighting, subjects, prints, and displays before deployment."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
