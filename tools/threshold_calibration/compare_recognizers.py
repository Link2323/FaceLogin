"""Offline R50/AuraFace/SFace comparison on identical SCRFD-aligned chips.

No production files are changed. Local photos/embeddings remain under ignored
data/. Thresholds are exploratory, calibrated on identities disjoint from the
test identities; they must not be copied to production without further data.
"""
from __future__ import annotations

import argparse
import gc
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import time

import cv2
import numpy as np
import onnx
import onnxruntime as ort

from calibrate import DEFAULT_MODELS_DIR, ScrfdDetector, align_face, load_image
from download_recognizer_candidates import DEST, verified

HERE = Path(__file__).resolve().parent
EXTS = {".jpg", ".jpeg", ".png", ".bmp", ".webp"}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


class Candidate:
    def __init__(self, key: str, path: Path, threads: int):
        self.key = key
        graph = onnx.load(str(path))
        first = graph.graph.node[:8]
        embedded_norm = (any(n.name.startswith(("Sub", "_minus")) for n in first)
                         and any(n.name.startswith(("Mul", "_mul")) for n in first))
        # Exact upstream ArcFace loader convention. SFace's OpenCV wrapper
        # always feeds raw RGB float pixels; its graph does the normalization.
        self.raw_rgb = key == "sface" or (key == "auraface" and embedded_norm)
        self.head = [{"name": n.name, "op": n.op_type} for n in first]
        del graph
        opts = ort.SessionOptions()
        opts.intra_op_num_threads = threads
        opts.inter_op_num_threads = 1
        opts.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
        opts.log_severity_level = 3
        start = time.perf_counter()
        self.session = ort.InferenceSession(str(path), sess_options=opts,
                                           providers=["CPUExecutionProvider"])
        self.load_ms = (time.perf_counter() - start) * 1000
        inputs, outputs = self.session.get_inputs(), self.session.get_outputs()
        if len(inputs) != 1 or len(outputs) != 1 or inputs[0].type != "tensor(float)":
            raise ValueError(f"Unexpected model interface: {key}")
        if inputs[0].shape[1:] != [3, 112, 112]:
            raise ValueError(f"Unexpected input shape: {inputs[0].shape}")
        self.input_name = inputs[0].name
        self.output_shape = outputs[0].shape

    def tensor(self, chip: np.ndarray) -> np.ndarray:
        rgb = chip[:, :, ::-1].astype(np.float32)
        if not self.raw_rgb:
            rgb = rgb * np.float32(1.0 / 127.5) - np.float32(1.0)
        return np.ascontiguousarray(rgb.transpose(2, 0, 1)[None])

    def embed_tensor(self, tensor: np.ndarray) -> tuple[np.ndarray, float]:
        raw = self.session.run(None, {self.input_name: tensor})[0].reshape(-1)
        norm = float(np.linalg.norm(raw))
        if not np.isfinite(raw).all() or not math.isfinite(norm) or norm < 1e-8:
            raise ValueError(f"Invalid embedding from {self.key}")
        return raw / norm, norm


def prepare(args) -> tuple[list[dict], list[np.ndarray], list[dict]]:
    detector = ScrfdDetector(args.detector)
    # Avoid the default runtime's machine-wide pool; all inference uses the
    # same requested thread count. Detection is shared, not benchmarked here.
    selected = sorted(p for p in args.lfw.iterdir() if p.is_dir()
                      and len([f for f in p.iterdir() if f.suffix.lower() in EXTS]) >= 4)
    rng = np.random.default_rng(20260930)
    rng.shuffle(selected)
    selected = selected[:args.identities]
    if len(selected) < 20:
        raise ValueError("Need at least 20 multi-image LFW identities for a held-out comparison")
    items = []
    for subject in selected:
        files = sorted(f for f in subject.iterdir() if f.suffix.lower() in EXTS)
        rng.shuffle(files)
        for path in files[:args.images_per_identity]:
            items.append((subject.name, path, "lfw", 1.0))
    for path in sorted(args.local.iterdir()):
        if path.suffix.lower() in EXTS and path.name.split("_")[0] in {"front", "left", "right"}:
            items.extend(("me", path, path.name.split("_")[0], scale) for scale in (1.0, 0.5))
    records, chips, failures = [], [], []
    for i, (owner, path, angle, scale) in enumerate(items):
        if i % 100 == 0:
            print(f"prepare {i + 1}/{len(items)}", flush=True)
        bgr = load_image(path)
        reason = "unreadable"
        if bgr is not None:
            if scale != 1:
                bgr = cv2.resize(bgr, None, fx=scale, fy=scale, interpolation=cv2.INTER_AREA)
            det = detector.detect_largest(bgr)
            reason = "no_detection" if det is None else "alignment_failed"
            chip = None if det is None else align_face(bgr, det.kps)
            if chip is not None:
                records.append({"owner": owner, "file": path.name, "angle": angle,
                                "scale": scale, "width": bgr.shape[1], "height": bgr.shape[0]})
                chips.append(chip)
                continue
        failures.append({"owner": owner, "file": path.name, "scale": scale, "reason": reason})
    print(f"Shared chips: {len(chips)}; failures: {len(failures)}", flush=True)
    return records, chips, failures


def distances(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    return np.sqrt(np.maximum(0, 2 - 2 * np.clip(a @ b.T, -1, 1)))


def summary(values) -> dict:
    a = np.asarray(values)
    if a.size == 0:
        return {"n": 0}
    return dict(n=int(a.size), min=float(a.min()), p50=float(np.percentile(a, 50)),
                p90=float(np.percentile(a, 90)), p99=float(np.percentile(a, 99)), max=float(a.max()))


def cutoff(impostors: np.ndarray, target: float) -> float:
    if impostors.size == 0 or not np.isfinite(impostors).all():
        raise ValueError("No finite impostor calibration trials")
    # Strict '<' matches production. At most floor(n*target) trials pass,
    # including ties; no interpolation falsely promises a lower observed FMR.
    index = min(int(math.floor(impostors.size * target)), impostors.size - 1)
    return float(np.sort(impostors)[index])


def account_trials(records, embeddings, owners):
    enroll, probes, probe_owners = [], [], []
    for owner in owners:
        ids = [i for i, r in enumerate(records) if r["owner"] == owner]
        if len(ids) < 2:
            continue
        count = min(3, max(1, len(ids) // 2))
        enroll.append((owner, ids[:count]))
        probes.extend(ids[count:])
        probe_owners.extend([owner] * (len(ids) - count))
    if len(enroll) < 2:
        raise ValueError("Not enough evaluable accounts")
    matrix = np.stack([distances(embeddings[probes], embeddings[ids]).min(axis=1)
                       for _, ids in enroll], axis=1)
    correct = np.array([next(j for j, (name, _) in enumerate(enroll) if name == owner)
                        for owner in probe_owners])
    mask = np.ones(matrix.shape, bool)
    mask[np.arange(len(probes)), correct] = False
    genuine = matrix[np.arange(len(probes)), correct]
    return matrix[mask], genuine, matrix, correct


def evaluate(records, embeddings, target_fmr):
    owners = sorted({r["owner"] for r in records if r["owner"] != "me"})
    np.random.default_rng(20260930).shuffle(owners)
    split = len(owners) // 2
    cal_i, _, _, _ = account_trials(records, embeddings, owners[:split])
    threshold = cutoff(cal_i, target_fmr)
    test_i, genuine, matrix, correct = account_trials(records, embeddings, owners[split:])
    order = np.argsort(matrix, axis=1)
    top = order[:, 0]
    best = matrix[np.arange(len(top)), top]
    second = matrix[np.arange(len(top)), order[:, 1]]
    result = {"threshold": threshold, "calibration_impostor_n": len(cal_i),
              "calibration_fmr": float(np.mean(cal_i < threshold)),
              "test_impostor_n": len(test_i), "test_fmr": float(np.mean(test_i < threshold)),
              "test_genuine_n": len(genuine), "test_tar": float(np.mean(genuine < threshold)),
              "test_identification_ratio075": float(np.mean((top == correct) & (best < threshold)
                                                            & (best < 0.75 * second))),
              "test_genuine": summary(genuine), "test_impostor": summary(test_i), "local": {}}
    # Earliest successful image of each angle is one enrollment template.
    # Exclude that SAME source photo at both resolutions from all probes.
    templates = [next((i for i, r in enumerate(records) if r["owner"] == "me"
                      and r["angle"] == angle and r["scale"] == 1), None)
                 for angle in ("front", "left", "right")]
    if any(i is None for i in templates):
        raise ValueError("Missing local enrollment angle")
    source_files = {records[i]["file"] for i in templates}
    stranger_ids = [i for i, r in enumerate(records) if r["owner"] != "me"]
    for scale in (1.0, 0.5):
        ids = [i for i, r in enumerate(records) if r["owner"] == "me" and r["scale"] == scale]
        own = distances(embeddings[ids], embeddings[ids])
        same, cross = [], []
        for a in range(len(ids)):
            for b in range(a + 1, len(ids)):
                group = same if records[ids[a]]["angle"] == records[ids[b]]["angle"] else cross
                group.append(float(own[a, b]))
        probes = [i for i in ids if records[i]["file"] not in source_files]
        d = distances(embeddings[probes], embeddings[templates])
        nearest = d.min(axis=1)
        front_only = d[:, 0]
        strangers = distances(embeddings[probes], embeddings[stranger_ids]).min(axis=1)
        stranger_to_me = distances(embeddings[stranger_ids], embeddings[templates]).min(axis=1)
        local = {"detected_n": len(ids), "probe_n": len(probes), "same_angle": summary(same),
                 "cross_angle": summary(cross), "multi_template_distance": summary(nearest),
                 "stranger_nearest": summary(strangers), "multi_tar": float(np.mean(nearest < threshold)),
                 "front_only_tar": float(np.mean(front_only < threshold)),
                 "ratio075_tar": float(np.mean((nearest < threshold) & (nearest < 0.75 * strangers))),
                 "stranger_to_me_fmr": float(np.mean(stranger_to_me < threshold)),
                 "stranger_to_me_n": len(stranger_to_me),
                 "at_r50_080_diagnostic": float(np.mean(nearest < 0.8)), "by_angle": {}}
        for angle in ("front", "left", "right"):
            mask = np.array([records[i]["angle"] == angle for i in probes])
            local["by_angle"][angle] = {"n": int(mask.sum()), "tar": float(np.mean(nearest[mask] < threshold))
                                         if mask.any() else None}
        result["local"][str(scale)] = local
    return result


def report(results, context) -> str:
    rows = ["# 识别模型离线 A/B（2026-09-30）", "",
            "仅评估识别层；生产仍使用 R50 INT8。阈值是探索性结果，不是部署建议。", "",
            f"环境：{context['platform']}；Python {context['python']}；ORT {ort.__version__}；"
            f"CPU EP，intra-op {context['threads']} / inter-op 1。", "",
            "同一份 SCRFD INT8（512×512）检测和生产几何对齐 chip，三个识别器共享。"
            "候选保持发布方 FP32；R50 是现有 INT8，耗时不能解释为同精度量化比较。", "",
            f"LFW 随机种子 20260930，选择 {context['identities']} 个至少 4 图身份，每人最多 "
            f"{context['images_per_identity']} 图；检测/对齐成功 {context['lfw_chips']} 图。"
            "身份随机等分为阈值标定组与独立测试组；每账户前最多 3 图注册，其余作 probe。", "",
            f"目标经验 FMR ≤ {context['target_fmr']:.2%}：标定组对每个 probe 与每个错误账户取最近模板距离，"
            "选择严格小于阈值的通过数不超过目标的最大观测边界。测试组与标定组身份不重叠；"
            "未对误差率作总体置信保证。", "",
            "| 指标 | R50 INT8 | AuraFace FP32 | SFace FP32 |", "|---|---:|---:|---:|"]
    keys = list(results)
    def add(label, values):
        rows.append("| " + label + " | " + " | ".join(values) + " |")
    for label, field, fmt in [("输出维度", "dimension", "d"), ("文件 MB（十进制）", "mb", ".1f"),
                              ("探索性阈值", "threshold", ".4f"), ("标定异人试验数", "calibration_impostor_n", "d"),
                              ("标定 FMR", "calibration_fmr", ".3%"), ("测试异人试验数", "test_impostor_n", "d"),
                              ("测试 FMR", "test_fmr", ".3%"), ("测试同人 probe 数", "test_genuine_n", "d"),
                              ("测试 TAR（距离门）", "test_tar", ".2%"),
                              ("测试正确身份 + 距离门 + ratio 0.75", "test_identification_ratio075", ".2%"),
                              ("预热推理 p50 ms", "infer_p50_ms", ".2f"),
                              ("预热推理 p90 ms", "infer_p90_ms", ".2f"),
                              ("预处理 + 推理 + 归一化 p50 ms", "embed_p50_ms", ".2f")]:
        add(label, [format(results[k][field], fmt) for k in keys])
    add("原始 embedding norm p50", [f"{results[k]['raw_norm']['p50']:.2f}" for k in keys])
    rows.extend(["", "## 更严格的探索性阈值", "",
                 "同一身份划分、注册模板与 probe；仅改变标定组的目标 FMR。0 表示标定样本中零通过，"
                 "不表示总体误接受率为零。TAR 是距离门单独通过率。", "",
                 "| 标定目标 / 指标 | R50 INT8 | AuraFace FP32 | SFace FP32 |", "|---|---:|---:|---:|"])
    for target in ("0.0001", "0.0"):
        for label, field, fmt in [("阈值", "threshold", ".4f"), ("测试 FMR", "test_fmr", ".3%"),
                                  ("LFW 测试 TAR", "test_tar", ".2%")]:
            add(f"{float(target):.2%} / {label}", [format(results[k]["sweep"][target][field], fmt) for k in keys])
        add(f"{float(target):.2%} / 本人三模板 TAR", [f"{results[k]['sweep'][target]['local']['1.0']['multi_tar']:.2%}"
                                                  for k in keys])
    for scale, title in [("1.0", "原始自拍"), ("0.5", "自拍宽高减半（INTER_AREA）")]:
        rows.extend(["", f"## {title}", "",
                     "正面/左转/右转各取最早成功图作模板；所有 probe 排除这三张原图（含缩小版）。"
                     "只有一个本地身份，来自同一会话。识别层单图通过率不等于多帧锁屏通过率。", "",
                     "| 指标 | R50 INT8 | AuraFace FP32 | SFace FP32 |", "|---|---:|---:|---:|"])
        for label, field, fmt in [("检测成功图数", "detected_n", "d"), ("独立 probe 数", "probe_n", "d"),
                                  ("仅正脸模板 TAR", "front_only_tar", ".2%"),
                                  ("三个角度模板 TAR", "multi_tar", ".2%"),
                                  ("三个模板 + 采样陌生人 ratio 0.75 TAR", "ratio075_tar", ".2%"),
                                  ("LFW probe 冒充本人：距离门 FMR", "stranger_to_me_fmr", ".3%"),
                                  ("套用 0.80 的 TAR（仅诊断，不用于候选结论）", "at_r50_080_diagnostic", ".2%")]:
            add(label, [format(results[k]["local"][scale][field], fmt) for k in keys])
        for angle in ("front", "left", "right"):
            add(f"{angle} probe TAR", [f"{results[k]['local'][scale]['by_angle'][angle]['tar']:.2%} "
                                        f"(n={results[k]['local'][scale]['by_angle'][angle]['n']})" for k in keys])
        for label, field in [("同角度距离", "same_angle"), ("跨角度距离", "cross_angle"),
                             ("三模板最近距离", "multi_template_distance"), ("最近陌生人距离", "stranger_nearest")]:
            add(label + " p50 / p90", [f"{results[k]['local'][scale][field]['p50']:.3f} / "
                                       f"{results[k]['local'][scale][field]['p90']:.3f}" for k in keys])
    original = results["r50"]["local"]["1.0"]
    smaller = results["r50"]["local"]["0.5"]
    rows.extend(["", "## 检测覆盖与迁移影响", "",
                 f"原自拍每个分辨率均尝试 {context['local_attempts']} 图，原分辨率仅 {original['detected_n']} 图、"
                 f"半分辨率仅 {smaller['detected_n']} 图可检测/对齐。"
                 "这些失败在三模型中相同，不归因于某个识别器；上面的 TAR 只统计成功取得 chip 的 probe。"
                 f"本轮原分辨率右侧脸只有 {original['by_angle']['right']['n']} 张独立 probe，"
                 "侧脸小样本不能支持总体通过率结论。", "",
                 "- AuraFace 输出 512 维，归一化 RGB，与当前识别接口接近；仍需重新录入、标定阈值/ratio/学习门，"
                 "并评估量化和慢机性能。",
                 "- SFace 输出 128 维，输入为原始 RGB 0..255；当前 512 维存储/协议校验及匹配/学习门需联查。"
                 f"其 norm 全样本最大 {results['sface']['raw_norm']['max']:.2f}，当前学习 norm 门限 19.1 不能沿用。",
                 "- SFace 的归一化 embedding 与 OpenCV 官方 FaceRecognizerSF 接口核对，"
                 f"最大绝对差 {results['sface']['reference_max_abs_error']:.8f}。", ""])
    rows.extend(["", "## 来源、复现与边界", "",
                 "- 候选版本、下载 URL、SHA-256、许可来源见 `tools/threshold_calibration/recognizer_candidates.json`。",
                 "- 个体记录、失败项、嵌入、chip 与 JSON 结果仅存在 gitignored `data/recognizer_ab/`。公开报告只含汇总。",
                 "- 正脸与侧脸自拍是本地小样本；LFW 是网络照片，未执行 CFP-FP/CPLFW 标准协议。",
                 "- my_faces/occlusion_test 是允许检测失败的遮挡测试；默认输入不读取此目录，不计入正常人脸结论。",
                 "- 没有 PAD、相机曝光、连续计帧、学习状态和锁屏端到端测量；分辨率缩小不等于真实暗光相机。",
                 "- 距离和未经归一化的 norm 都是模型特定量，不能直接沿用 R50 匹配/学习阈值。",
                 "- 本人 ratio 的次佳距离取所有采样 LFW chip 的最近值；它是探索性拒识门，不代表实际已注册账户数量。",
                 "- 线程设置与生产上限一致，但 Python ORT 版本、独立 session 池与 C++ 全局池不同；"
                 "这些是本机离线耗时，不是慢机或生产 E2E。", "",
                 "```powershell",
                 "& D:/anaconda/python.exe tools/threshold_calibration/download_recognizer_candidates.py",
                 "& D:/anaconda/python.exe tools/threshold_calibration/compare_recognizers.py",
        "```", ""])
    rows.extend(["## 文件标识", "", "| 模型 | SHA-256 |", "|---|---|"])
    rows.extend(f"| {k} | `{results[k]['sha256']}` |" for k in keys)
    rows.append("")
    return "\n".join(rows)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lfw", type=Path, default=HERE / "data" / "lfw_full")
    parser.add_argument("--local", type=Path, default=HERE / "data" / "lfw_subset" / "me")
    parser.add_argument("--detector", type=Path, default=DEFAULT_MODELS_DIR / "det_10g_gnkps.onnx")
    parser.add_argument("--identities", type=int, default=200)
    parser.add_argument("--images-per-identity", type=int, default=8)
    parser.add_argument("--threads", type=int, default=min(8, max(2, (os.cpu_count() or 4) // 2)))
    parser.add_argument("--target-fmr", type=float, default=0.001)
    parser.add_argument("--bench-iters", type=int, default=100)
    parser.add_argument("--report", type=Path, default=HERE.parents[1] / "docs" / "recognizer-comparison.md")
    args = parser.parse_args()
    if not 0 <= args.target_fmr < 1 or args.threads < 1 or args.bench_iters < 1:
        parser.error("Invalid FMR/thread/benchmark option")
    cv2.setNumThreads(args.threads)
    specs = json.loads((HERE / "recognizer_candidates.json").read_text("utf-8"))
    for key, spec in specs.items():
        if not verified(DEST / spec["filename"], spec):
            raise ValueError(f"Missing/unverified {key}; run download_recognizer_candidates.py")
    records, chips, failures = prepare(args)
    outdir = HERE / "data" / "recognizer_ab"
    outdir.mkdir(parents=True, exist_ok=True)
    (outdir / "samples.json").write_text(json.dumps({"records": records, "failures": failures}, indent=2), "utf-8")
    np.savez_compressed(outdir / "chips.npz", chips=np.stack(chips))
    paths = {"r50": DEFAULT_MODELS_DIR / "w600k_r50.onnx",
             **{key: DEST / spec["filename"] for key, spec in specs.items()}}
    results = {}
    for key, path in paths.items():
        print(f"Embedding {key}: {len(chips)} chips", flush=True)
        candidate = Candidate(key, path, args.threads)
        tensors = [candidate.tensor(c) for c in chips]
        for tensor in tensors[:10]:
            candidate.embed_tensor(tensor)
        embeddings, norms = zip(*(candidate.embed_tensor(t) for t in tensors))
        embeddings = np.stack(embeddings)
        reference_error = None
        if key == "sface":
            reference = cv2.FaceRecognizerSF.create(str(path), "").feature(chips[0]).reshape(-1)
            reference /= np.linalg.norm(reference)
            error = float(np.max(np.abs(reference - embeddings[0])))
            reference_error = error
            if error > 1e-3:
                raise ValueError(f"SFace OpenCV/ORT reference mismatch: {error}")
            print(f"SFace OpenCV/ORT max absolute embedding difference: {error:.8f}", flush=True)
        infer_ms, embed_ms = [], []
        for i in range(args.bench_iters):
            index = (i * 13) % len(chips)
            start = time.perf_counter()
            candidate.session.run(None, {candidate.input_name: tensors[index]})
            infer_ms.append((time.perf_counter() - start) * 1000)
            start = time.perf_counter()
            candidate.embed_tensor(candidate.tensor(chips[index]))
            embed_ms.append((time.perf_counter() - start) * 1000)
        result = evaluate(records, embeddings, args.target_fmr)
        result["sweep"] = {str(target): evaluate(records, embeddings, target) for target in (0.0001, 0.0)}
        result.update(dimension=embeddings.shape[1], mb=path.stat().st_size / 1e6,
                      sha256=sha256(path), preprocess="raw RGB 0..255" if candidate.raw_rgb else "RGB pixel/127.5-1",
                      graph_head=candidate.head, raw_norm=summary(norms), load_ms=candidate.load_ms,
                      infer_p50_ms=float(np.median(infer_ms)), infer_p90_ms=float(np.percentile(infer_ms, 90)),
                      embed_p50_ms=float(np.median(embed_ms)))
        result["reference_max_abs_error"] = reference_error
        results[key] = result
        np.savez_compressed(outdir / f"{key}_embeddings.npz", embeddings=embeddings, norms=np.asarray(norms))
        print(f"{key}: threshold {result['threshold']:.4f}; test FMR {result['test_fmr']:.4%}; "
              f"TAR {result['test_tar']:.2%}; infer p50 {result['infer_p50_ms']:.2f}ms", flush=True)
        del candidate, tensors, embeddings
        gc.collect()
    context = dict(platform=platform.platform(), python=platform.python_version(), threads=args.threads,
                   identities=len({r['owner'] for r in records if r['owner'] != 'me'}),
                   images_per_identity=args.images_per_identity, target_fmr=args.target_fmr,
                   lfw_chips=sum(r['owner'] != 'me' for r in records), detector_sha256=sha256(args.detector),
                   local_attempts=sum(r['owner'] == 'me' and r['scale'] == 1 for r in records + failures))
    (outdir / "results.json").write_text(json.dumps({"context": context, "results": results}, indent=2), "utf-8")
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(report(results, context), "utf-8")
    print(f"Report: {args.report}", flush=True)


if __name__ == "__main__":
    main()
