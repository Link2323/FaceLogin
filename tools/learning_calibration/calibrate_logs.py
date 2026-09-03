# -*- coding: utf-8 -*-
"""渐进学习阶段 0:标定日志聚合器。

聚合 FaceLogin 四类埋点,产出渐进学习(progressive-learning-v2.md)阶段 1 所需的
门控数值建议与继续/终止判据:

  service.log(.YYYY-MM-DD)     Identity binding complete ... distance=D   成功轮同人距离
                               Authentication worker failed ... closest identity distance=D
  auth_worker.log(.YYYY-MM-DD) Embedding norms pre-normalize (binding attempts): [..]
  console.log(.YYYY-MM-DD)     Enrollment sample: angle=.. yaw=.. pad=.. emb_norm=N
                               Enrollment consistency [角度]: avg pairwise dist=D
                               Template stats [load|reload]: ... norm=N

日志按天轮转(默认保留 14 天),本脚本自动读取轮转的带日期后缀文件,并把提取出的
事件增量缓存到脚本目录 calibration_cache.jsonl —— 日志过期删除后统计量仍可累积。

用法: python calibrate_logs.py [--tag 机器名] <dir-or-log> [<dir-or-log> ...]
"""
from __future__ import annotations

import json
import math
import re
import sys
from datetime import datetime
from pathlib import Path

CACHE = Path(__file__).resolve().parent / "calibration_cache.jsonl"

TS_RE = re.compile(r"^\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3})\]")
RE_BINDING = re.compile(r"Identity binding complete: user=(\S+), distance=([\d.]+)")
RE_FAIL = re.compile(r"closest identity distance=([\d.]+), threshold=([\d.]+)")
RE_NORMS = re.compile(r"Embedding norms pre-normalize \(binding attempts\): \[([^\]]*)\]")
RE_ENROLL_SAMPLE = re.compile(r"Enrollment sample: angle=(-?\d+) yaw=(-?[\d.]+) pad=([\d.]+) emb_norm=([\d.]+)")
RE_ENROLL_CONS = re.compile(r"Enrollment consistency \[(.+?)\]: avg pairwise dist=([\d.]+)")
RE_TEMPLATE = re.compile(r"Template stats \[(\w+)\]: user=(\S+) face#(\d+)\((.+?)\) dim=(\d+) norm=([\d.]+)")


def read_text(path: Path) -> str:
    raw = path.read_bytes()
    if raw.startswith(b"\xff\xfe") or raw.startswith(b"\xfe\xff"):
        return raw.decode("utf-16")
    return raw.decode("utf-8-sig", errors="replace")


def parse_ts(line: str) -> datetime | None:
    m = TS_RE.match(line)
    return datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f") if m else None


def log_files(base: Path, stem: str) -> list[Path]:
    files = []
    if (base / f"{stem}.log").is_file():
        files.append(base / f"{stem}.log")
    files += sorted(p for p in base.glob(f"{stem}.*.log")
                    if re.fullmatch(rf"{re.escape(stem)}\.\d{{4}}-\d{{2}}-\d{{2}}\.log", p.name))
    return files


def parse_any(path: Path, tag: str, out: list[dict]) -> None:
    try:
        text = read_text(path)
    except OSError as e:
        print(f"  [warn] 不可读: {path} ({e})", file=sys.stderr)
        return
    for line in text.splitlines():
        ts = parse_ts(line)
        if ts is None:
            continue
        t = ts.isoformat(sep=" ", timespec="milliseconds")
        if m := RE_BINDING.search(line):
            out.append({"t": "b", "ts": t, "tag": tag, "u": m.group(1), "d": float(m.group(2))})
        elif m := RE_FAIL.search(line):
            out.append({"t": "f", "ts": t, "tag": tag, "d": float(m.group(1))})
        elif m := RE_NORMS.search(line):
            vals = [float(v) for v in m.group(1).split(",") if v.strip()]
            if vals:
                out.append({"t": "n", "ts": t, "tag": tag, "v": vals})
        elif m := RE_ENROLL_SAMPLE.search(line):
            out.append({"t": "e", "ts": t, "tag": tag, "n": float(m.group(4))})
        elif m := RE_ENROLL_CONS.search(line):
            out.append({"t": "c", "ts": t, "tag": tag, "a": m.group(1), "d": float(m.group(2))})
        elif m := RE_TEMPLATE.search(line):
            out.append({"t": "s", "ts": t, "tag": tag, "u": m.group(2),
                        "f": int(m.group(3)), "l": m.group(4), "n": float(m.group(6))})
    print(f"  {path.name}: {len(text.splitlines())} 行")


def collect(paths: list[str], tag: str) -> list[dict]:
    fresh: list[dict] = []
    for arg in paths:
        p = Path(arg)
        if p.is_dir():
            cands = []
            for stem in ("service", "auth_worker", "console"):
                for base in (p, p / "log"):
                    cands += log_files(base, stem)
            if not cands:
                print(f"  [warn] 目录下无已知日志: {p}", file=sys.stderr)
            for f in cands:
                parse_any(f, tag, fresh)
        elif p.is_file():
            parse_any(p, tag, fresh)
        else:
            print(f"  [warn] 不存在: {p}", file=sys.stderr)
    return fresh


def load_cache() -> list[dict]:
    if not CACHE.exists():
        return []
    events = []
    for line in CACHE.read_text(encoding="utf-8").splitlines():
        try:
            events.append(json.loads(line))
        except json.JSONDecodeError:
            pass
    return events


def save_merged(fresh: list[dict]) -> list[dict]:
    def key(e: dict):
        return (e["t"], e["ts"], e["d"] if "d" in e else json.dumps(e.get("v", e.get("n")), sort_keys=True))
    seen: dict = {}
    for e in load_cache() + fresh:      # fresh wins on duplicate
        seen[key(e)] = e
    events = sorted(seen.values(), key=lambda e: e["ts"])
    CACHE.write_text("".join(json.dumps(e, ensure_ascii=False) + "\n" for e in events), encoding="utf-8")
    return events

# ---------------------------------------------------------------------------
# 统计
# ---------------------------------------------------------------------------

def pct(sorted_vals: list[float], p: float) -> float:
    if not sorted_vals:
        return float("nan")
    i = min(len(sorted_vals) - 1, max(0, math.ceil(p / 100 * len(sorted_vals)) - 1))
    return sorted_vals[i]


def show_dist(name: str, vals: list[float], pcts=(10, 20, 50, 85, 90, 99)) -> None:
    if not vals:
        print(f"{name}: 无数据")
        return
    s = sorted(vals)
    parts = " ".join(f"p{p}={pct(s, p):.3f}" for p in pcts)
    print(f"{name}: n={len(vals)} min={s[0]:.3f} {parts} max={s[-1]:.3f}")


def pearson(xs: list[float], ys: list[float]) -> float:
    n = len(xs)
    if n < 3:
        return float("nan")
    mx, my = sum(xs) / n, sum(ys) / n
    sx = math.sqrt(sum((x - mx) ** 2 for x in xs))
    sy = math.sqrt(sum((y - my) ** 2 for y in ys))
    if sx < 1e-12 or sy < 1e-12:
        return float("nan")
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / (sx * sy)

# ---------------------------------------------------------------------------
# 主报告
# ---------------------------------------------------------------------------

def main() -> int:
    args = sys.argv[1:]
    tag = "local"
    if "--tag" in args:
        i = args.index("--tag")
        tag = args[i + 1]
        del args[i:i + 2]
    if not args:
        print(__doc__)
        return 2
    events = save_merged(collect(args, tag))
    by = lambda t: [e for e in events if e["t"] == t]
    binding = [(datetime.fromisoformat(e["ts"]), e["d"]) for e in by("b")]
    norms = [(datetime.fromisoformat(e["ts"]), e["v"]) for e in by("n")]
    fail_d = [e["d"] for e in by("f")]
    enroll_norm = [e["n"] for e in by("e")]
    tags = sorted({e["tag"] for e in events})
    print(f"\n事件源: {', '.join(tags)} (缓存累计 {len(events)} 条)")

    print("\n== 1. 成功轮同人距离(渐进学习的主分布) ==")
    dists = [d for _, d in binding]
    show_dist("全部", dists)
    cons_ev = by("c")
    if cons_ev:
        last_enroll = max(datetime.fromisoformat(e["ts"]) for e in cons_ev)
        current = [d for ts, d in binding if ts >= last_enroll]
        if current:
            show_dist(f"当前模板纪元(自 {last_enroll:%m-%d %H:%M} 录入后)", current)
    weeks: dict[str, list[float]] = {}
    for ts, d in binding:
        weeks.setdefault(f"{ts.isocalendar()[0]}-W{ts.isocalendar()[1]:02d}", []).append(d)
    for wk in sorted(weeks):
        show_dist(f"  {wk}", weeks[wk], pcts=(20, 50, 90))
    if fail_d:
        show_dist("失败轮 closest distance", fail_d)
        marginal = [d for d in fail_d if 0.70 <= d <= 0.80]
        print(f"  边缘失败(closest∈[0.70,0.80]): {len(marginal)} 次")

    print("\n== 2. 探针嵌入范数(worker, 归一化前) ==")
    all_norms = [v for _, vs in norms for v in vs]
    best_norms = [min(vs) for _, vs in norms]
    show_dist("全部绑定尝试", all_norms, pcts=(5, 25, 50, 75, 95))
    show_dist("每轮最优帧", best_norms, pcts=(5, 25, 50, 75, 95))

    print("\n== 3. 录入样本范数 + 帧间一致性 ==")
    show_dist("录入样本范数", enroll_norm, pcts=(5, 25, 50, 75, 95))
    for e in cons_ev[-9:]:
        print(f"  {e['ts'][:16]} [{e['a']}] avg pairwise={e['d']:.4f}")

    print("\n== 4. 距离-范数相关性(按轮配对, ±10s) ==")
    pairs_x, pairs_y = [], []
    bi = 0
    for ts, vs in norms:
        while bi < len(binding) and (binding[bi][0] - ts).total_seconds() < -10:
            bi += 1
        if bi < len(binding) and 0 <= (binding[bi][0] - ts).total_seconds() <= 10:
            pairs_x.append(min(vs))
            pairs_y.append(binding[bi][1])
            bi += 1
    if len(pairs_x) >= 3:
        r = pearson(pairs_x, pairs_y)
        print(f"配对 {len(pairs_x)} 轮, 范数-距离 Pearson r = {r:+.3f}"
              + ("  (负相关:范数高≈距离近, 质量信号有效)" if r < -0.2 else "  (相关性弱)"))
    else:
        print(f"配对不足({len(pairs_x)} 轮)")

    print("\n== 5. 存量模板静态范数(时间线) ==")
    for e in by("s")[-12:]:
        print(f"  {e['ts'][:16]} {e['u']} face#{e['f']}({e['l']}) norm={e['n']:.4f}")

    print("\n== 6. 门控数值建议(阶段 1 输入) ==")
    if dists:
        s = sorted(dists)
        p20, p50, p85, p90 = pct(s, 20), pct(s, 50), pct(s, 85), pct(s, 90)
        print(f"更新距离门 = min(同人 p20={p20:.3f}, 阈值-0.25=0.55) → {min(p20, 0.55):.3f}")
        print(f"自动新增带 = [{p50:.3f}, {p85:.3f}]")
        verdict = "系统已稳(p90≤0.70),渐进学习优先级可降" if p90 <= 0.70 else \
                  f"存在漂移(p90={p90:.3f}>0.70),渐进学习收益可期"
        print(f"继续/终止判据: {verdict}")
    if enroll_norm:
        s = sorted(enroll_norm)
        print(f"范数地板(录入 p25) = {pct(s, 25):.3f}")
    elif all_norms:
        s = sorted(all_norms)
        print(f"范数地板(回退:探针 p25) = {pct(s, 25):.3f}")
    print(f"\n数据量: 成功 {len(dists)} 轮 / 失败 {len(fail_d)} 轮 / 录入样本 {len(enroll_norm)} 个"
          + ("  (判据: 两机≥2周且≥300次成功认证)" if len(dists) < 300 else "  (已达标)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
