"""Offline enrollment, small-gallery and quantization research; no deployment.

Reuse the pinned A/B chips, with identity-disjoint calibration/test groups.
Repeated simulated galleries are correlated trials, not independent subjects.
All weights, chips and individual results stay in ignored recognizer_ab/.
"""
from __future__ import annotations

import argparse
import gc
import hashlib
import json
import platform
import time
from pathlib import Path

import cv2
import numpy as np
import onnx
import onnxruntime as ort
from onnxruntime.quantization import CalibrationDataReader, QuantFormat, QuantType, quantize_static

from compare_recognizers import Candidate, cutoff, distances, sha256, summary
from calibrate import DEFAULT_MODELS_DIR, ScrfdDetector, align_face, load_image

HERE = Path(__file__).resolve().parent
DATA = HERE / "data" / "recognizer_ab"
SEED = 20261001


class ResearchCandidate(Candidate):
    def tensor(self, chip):
        if self.key == "seeta_light":
            # SeetaFace6Onnx FaceRecognizer.cs Light + FaceAlignment.cs:
            # arcface reference coordinates, BGR CHW, pixel/255, no sqrt.
            bgr = chip.astype(np.float32) * np.float32(1.0 / 255.0)
            return np.ascontiguousarray(bgr.transpose(2, 0, 1)[None])
        return super().tensor(chip)


def partition(records):
    owners = sorted({r["owner"] for r in records if r["owner"] != "me"})
    np.random.default_rng(20260930).shuffle(owners)
    return owners[:len(owners) // 2], owners[len(owners) // 2:]


def enrollments(records, owners, count):
    """Reserve at least one probe; never use a source chip twice."""
    templates, probes, correct = [], [], []
    for owner in owners:
        ids = [i for i, r in enumerate(records) if r["owner"] == owner]
        take = min(count, len(ids) - 1)
        if take < 1:
            raise ValueError("Owner has no independent probe")
        templates.append(ids[:take])
        probes.extend(ids[take:])
        correct.extend([len(templates) - 1] * (len(ids) - take))
    return templates, probes, np.asarray(correct)


def matrix_for(embeddings, probes, templates):
    return np.stack([distances(embeddings[probes], embeddings[ids]).min(axis=1)
                     for ids in templates], axis=1)


def gallery_scores(matrix, correct, count, repeats=30):
    """Known probes include own account; unknown probes exclude their account."""
    rng = np.random.default_rng(SEED + count)
    known_best, known_ratio, known_correct, unknown_best, unknown_ratio = [], [], [], [], []
    for row, own in zip(matrix, correct):
        others = np.delete(np.arange(len(row)), own)
        for _ in range(repeats):
            kg = np.r_[own, rng.choice(others, count - 1, replace=False)]
            ug = rng.choice(others, count, replace=False)
            order = np.argsort(row[kg])
            known_best.append(row[kg[order[0]]])
            known_correct.append(kg[order[0]] == own)
            known_ratio.append(row[kg[order[0]]] / row[kg[order[1]]] if count > 1 else 0)
            order = np.argsort(row[ug])
            unknown_best.append(row[ug[order[0]]])
            unknown_ratio.append(row[ug[order[0]]] / row[ug[order[1]]] if count > 1 else 0)
    return tuple(np.asarray(x) for x in (known_best, known_ratio, known_correct, unknown_best, unknown_ratio))


def analyze(records, embeddings, variant_embeddings):
    cal_owners, test_owners = partition(records)
    result = {}
    for count in (1, 3):
        ct, cp, cc = enrollments(records, cal_owners, count)
        tt, tp, tc = enrollments(records, test_owners, count)
        cal = matrix_for(embeddings, cp, ct)
        mask = np.ones(cal.shape, bool)
        mask[np.arange(len(cp)), cc] = False
        threshold = cutoff(cal[mask], 0.0001)
        trials = {}
        for variant, values in variant_embeddings.items():
            test = np.stack([distances(values[tp], embeddings[ids]).min(axis=1) for ids in tt], axis=1)
            imask = np.ones(test.shape, bool)
            imask[np.arange(len(tp)), tc] = False
            genuine = test[np.arange(len(tp)), tc]
            entry = {"probe_n": len(tp), "distance_tar": float(np.mean(genuine < threshold)),
                     "wrong_account_fmr": float(np.mean(test[imask] < threshold)), "galleries": {}}
            for accounts in (1, 2, 5):
                best, ratio, correct, ubest, uratio = gallery_scores(test, tc, accounts)
                entry["galleries"][str(accounts)] = {
                    str(gate): {"tar": float(np.mean(correct & (best < threshold) & (ratio < gate))),
                                "unknown_fpir": float(np.mean((ubest < threshold) & (uratio < gate)))}
                    for gate in (0.75, 0.85, 0.95, 1.0)}
            trials[variant] = entry
        result[str(count)] = {"threshold": threshold, "calibration_pair_n": int(mask.sum()), "variants": trials}
    # Original angle-labelled local set, with fixed disjoint enrollment photos.
    templates = [next(i for i, r in enumerate(records) if r["owner"] == "me"
                      and r["angle"] == a and r["scale"] == 1) for a in ("front", "left", "right")]
    files = {records[i]["file"] for i in templates}
    probes = [i for i, r in enumerate(records) if r["owner"] == "me"
              and r["scale"] == 1 and r["file"] not in files]
    threshold = result["3"]["threshold"]
    result["local"] = {v: {"n": len(probes), "front_only_tar": float(np.mean(distances(e[probes], embeddings[templates[:1]]).min(axis=1) < threshold)),
                            "three_template_tar": float(np.mean(distances(e[probes], embeddings[templates]).min(axis=1) < threshold)),
                            "distance": summary(distances(e[probes], embeddings[templates]).min(axis=1))}
                       for v, e in variant_embeddings.items()}
    return result


def degrade(chips, variant):
    if variant == "original":
        return chips
    if variant == "32px":
        return [cv2.resize(cv2.resize(c, (32, 32), interpolation=cv2.INTER_AREA), (112, 112), interpolation=cv2.INTER_LINEAR) for c in chips]
    if variant == "blur1.5":
        return [cv2.GaussianBlur(c, (0, 0), 1.5) for c in chips]
    raise ValueError(variant)


class QuantReader(CalibrationDataReader):
    def __init__(self, candidate, chips):
        self.name = candidate.input_name
        self.values = iter(candidate.tensor(c) for c in chips)

    def get_next(self):
        value = next(self.values, None)
        return None if value is None else {self.name: value}


def quantize(records, chips, threads):
    source = DATA / "models" / "auraface_v1.onnx"
    output = DATA / "models" / "auraface_v1_int8_research.onnx"
    metadata = output.with_suffix(".json")
    owners, _ = partition(records)
    # One source image per calibration identity; never local/test chips.
    indices = [next(i for i, r in enumerate(records) if r["owner"] == owner) for owner in owners[:64]]
    recipe = {"source_sha256": sha256(source), "calibration_chip_indices": indices,
              "quant_format": "QDQ", "per_channel": True, "weight_type": "QInt8", "activation_type": "QInt8",
              "ops": ["Conv", "Gemm"], "ActivationSymmetric": True, "opset": 13}
    if output.exists() and metadata.exists():
        previous = json.loads(metadata.read_text("utf-8"))
        if previous.get("recipe") == recipe and previous.get("sha256") == sha256(output):
            return output
    candidate = Candidate("auraface", source, threads)
    print("Quantize AuraFace: 64 calibration-only chips, QDQ per-channel", flush=True)
    converted_path = output.with_name("auraface_opset13_intermediate.onnx")
    try:
        graph = onnx.version_converter.convert_version(onnx.load(str(source)), 13)
        onnx.checker.check_model(graph)
        onnx.save(graph, str(converted_path))
        del graph
        converted = Candidate("auraface", converted_path, threads)
        for i in indices[:8]:
            original, _ = candidate.embed_tensor(candidate.tensor(chips[i]))
            upgraded, _ = converted.embed_tensor(converted.tensor(chips[i]))
            if np.max(np.abs(original - upgraded)) > 1e-5:
                raise ValueError("Opset upgrade changed the FP32 embeddings")
        del converted
        quantize_static(str(converted_path), str(output), QuantReader(candidate, [chips[i] for i in indices]),
                        quant_format=QuantFormat.QDQ, per_channel=True, weight_type=QuantType.QInt8,
                        activation_type=QuantType.QInt8, op_types_to_quantize=["Conv", "Gemm"],
                        extra_options={"ActivationSymmetric": True})
    finally:
        converted_path.unlink(missing_ok=True)
    metadata.write_text(json.dumps({"recipe": recipe, "sha256": sha256(output)}, indent=2), "utf-8")
    del candidate
    gc.collect()
    return output


def embed(candidate, chips):
    rows, norms = [], []
    for i, chip in enumerate(chips):
        if i % 300 == 0:
            print(f"  embed {i}/{len(chips)}", flush=True)
        row, norm = candidate.embed_tensor(candidate.tensor(chip))
        rows.append(row)
        norms.append(norm)
    return np.stack(rows), np.asarray(norms)


def benchmark(key, path, chips):
    results = {}
    for threads in (1, 2, 4, 8):
        candidate = ResearchCandidate(key, path, threads)
        tensors = [candidate.tensor(c) for c in chips[:12]]
        for tensor in tensors:
            candidate.embed_tensor(tensor)
        samples = []
        for i in range(120):
            begin = time.perf_counter()
            candidate.embed_tensor(candidate.tensor(chips[(i * 13) % len(chips)]))
            samples.append((time.perf_counter() - begin) * 1000)
        results[str(threads)] = summary(samples)
        print(f"  threads={threads}: p50 {results[str(threads)]['p50']:.2f} ms", flush=True)
        del candidate
        gc.collect()
    return results


def camera_evaluation(paths, results):
    """Small local check using only existing upright WIN_* captures, no PAD."""
    root = HERE.parents[1] / "my_faces"
    detector = ScrfdDetector(DEFAULT_MODELS_DIR / "det_10g_gnkps.onnx")
    records, chips, seen = [], [], set()
    attempts, failures = 0, 0
    for owner in ("me", "mother"):
        for path in sorted((root / owner).glob("WIN_*")):
            attempts += 1
            image = load_image(path)
            if image is None:
                failures += 1
                continue
            digest = hashlib.sha256(image.tobytes()).hexdigest()
            if digest in seen:
                continue
            seen.add(digest)
            detection = detector.detect_largest(image)
            chip = None if detection is None else align_face(image, detection.kps)
            if chip is None:
                failures += 1
                continue
            records.append({"owner": owner})
            chips.append(chip)
    if any(sum(r["owner"] == owner for r in records) < 4 for owner in ("me", "mother")):
        return None
    templates, probes, correct = enrollments(records, ["me", "mother"], 3)
    result = {"attempts": attempts, "detected_unique": len(chips), "failures": failures,
              "method": "WIN_* only; decoded-pixel SHA256 dedup; first 3 per owner enroll; remaining probe; no me_photo/occlusion", "results": {}}
    for key, path in paths.items():
        if key not in results:
            continue
        candidate = ResearchCandidate("auraface" if key == "auraface_int8" else key, path, 8)
        values = np.stack([candidate.embed_tensor(candidate.tensor(c))[0] for c in chips])
        matrix = matrix_for(values, probes, templates)
        order = np.argsort(matrix, axis=1)
        best = matrix[np.arange(len(probes)), order[:, 0]]
        second = matrix[np.arange(len(probes)), order[:, 1]]
        threshold = results[key]["accuracy"]["3"]["threshold"]
        accepted = (order[:, 0] == correct) & (best < threshold) & (best < 0.75 * second)
        result["results"][key] = {"n": len(probes), "tar_ratio075": float(np.mean(accepted))}
        del candidate
        gc.collect()
    return result


def report(all_results):
    lines = ["# 识别模型补充评估（2026-10-01）", "",
             "目标：权重允许无需另行申请许可地分发；允许重新录入，比较识别层准确性与 Windows CPU 耗时。生产未切换。", "",
             "复用前轮 200 个 LFW 身份、1177 张独立源图，以及一个本地身份的成功对齐自拍。标定/测试身份各 100；"
             "量化只使用标定身份各一图，共 64 图。注册 1 或最多 3 张图，并且至少保留一张 probe；注册图不会成为 probe。", "",
             "距离阈值在标定身份上按经验错误账户 FMR ≤0.01% 选择；所有变体保持这个阈值。"
             "ratio 0.75/0.85/0.95/1.0 只作敏感性分析，没有用测试结果选择部署参数。", "",
             "小账户库：每张已知 probe 随机加入 k−1 个异人账户；同一 probe 作为未知人时，从库中排除本人并随机抽 k 个异人账户。"
             "每张图重复 30 个库，库中每人有对应数量注册模板；TAR 包含正确识别和距离/ratio 两个门，FPIR 为未知人被任何账户接收。"
             "重复库共享人脸图，试验有相关性，不能把重复次数当作独立人数。", "",
             "32px 与 blur1.5 仅对 probe 的对齐 chip 做合成降质，注册模板保持原图；不代表真实暗光、真实姿态变化或检测能力。"
             "my_faces/occlusion_test 未读取，允许遮挡漏检。", "",
             "## 本机识别层耗时", "", "预热后的预处理＋推理＋归一化，每个线程档 120 次；ORT CPU EP，inter-op=1。不是完整解锁耗时。", "",
             "| 模型 | MB | 1线程 p50 | 2线程 p50 | 4线程 p50 | 8线程 p50/p90 |", "|---|---:|---:|---:|---:|---:|"]
    for k, r in all_results.items():
        b = r["bench"]
        lines.append(f"| {k} | {r['mb']:.1f} | {b['1']['p50']:.2f} ms | {b['2']['p50']:.2f} ms | {b['4']['p50']:.2f} ms | {b['8']['p50']:.2f}/{b['8']['p90']:.2f} ms |")
    for count in ("1", "3"):
        lines += ["", f"## 注册 {count} 张：距离门与合成降质", "", "| 模型 | 阈值 | 原图 TAR/FMR | 32px TAR/FMR | blur1.5 TAR/FMR |", "|---|---:|---:|---:|---:|"]
        for k, r in all_results.items():
            a = r["accuracy"][count]
            cells = [f"{a['variants'][v]['distance_tar']:.2%}/{a['variants'][v]['wrong_account_fmr']:.3%}" for v in ("original", "32px", "blur1.5")]
            lines.append(f"| {k} | {a['threshold']:.4f} | " + " | ".join(cells) + " |")
        lines += ["", "### 原图，小账户库 TAR/未知人 FPIR", "", "| 模型 | ratio | 1账户 | 2账户 | 5账户 |", "|---|---:|---:|---:|---:|"]
        for k, r in all_results.items():
            a = r["accuracy"][count]["variants"]["original"]["galleries"]
            for gate in ("0.75", "0.85", "0.95", "1.0"):
                cells = [f"{a[n][gate]['tar']:.2%}/{a[n][gate]['unknown_fpir']:.3%}" for n in ("1", "2", "5")]
                lines.append(f"| {k} | {gate} | " + " | ".join(cells) + " |")
    lines += ["", "## 本地三角度注册", "", "每种变体仅 11 张 probe、一个身份、同一会话；不是跨会话或多人结论。", "", "| 模型 | 原图：正脸模板/三角度模板 | 32px：三角度模板 | blur1.5：三角度模板 |", "|---|---:|---:|---:|"]
    for k, r in all_results.items():
        a = r["accuracy"]["local"]
        lines.append(f"| {k} | {a['original']['front_only_tar']:.2%}/{a['original']['three_template_tar']:.2%} | {a['32px']['three_template_tar']:.2%} | {a['blur1.5']['three_template_tar']:.2%} |")
    camera_path = DATA / "local_camera_results.json"
    if camera_path.exists():
        camera = json.loads(camera_path.read_text("utf-8"))
        lines += ["", "## 现有摄像头照片补充", "",
                  f"只读取 my_faces/me 和 mother 下的 WIN_* 摄像头照片，排除 me_photo 与 occlusion_test；"
                  f"尝试 {camera['attempts']} 张，像素 SHA-256 去重后成功对齐 {camera['detected_unique']} 张，失败 {camera['failures']} 张。"
                  "每人按文件时间顺序取前三张注册，其余作为 probe；两账户匹配含 ratio 0.75。短时间连拍不是跨日验证。", "",
                  "| 模型 | 正确身份＋距离＋ratio 通过 |", "|---|---:|"]
        for key, item in camera["results"].items():
            lines.append(f"| {key} | {item['tar_ratio075']:.2%}（n={item['n']}） |")
    lines += ["", "## 量化误差", ""]
    q = all_results["auraface_int8"]["fp32_cosine"]
    lines += [f"AuraFace INT8 与原 FP32 的同图 embedding 余弦：min={q['min']:.6f}，p50={q['p50']:.6f}。接近 1 也不能替代识别准确率检查。", "", "## 复现与边界", "",
              "- 识别权重/量化配方/哈希/个体缓存保存在已忽略的 data/recognizer_ab/，未复制到生产或安装器。",
              "- 旧基准与本次时间不可直接做优化归因；本表是在同一程序内重新测量。线程档仅描述本机，不代表慢机。",
              "- 只有 100 个测试身份，极低误接收率不具备足够统计保证；零观测不等于总体为零。",
              "- 检测先失败时，更换识别器不能恢复该帧；本轮只评估已成功对齐的 chip。",
              "- 未执行摄像头、PAD、连续帧、注册 GUI、锁屏端到端验证。",
              "", "```powershell", "& D:/anaconda/python.exe tools/threshold_calibration/refine_recognizer_comparison.py", "```", ""]
    return "\n".join(lines)


def write_report(target, results):
    begin, end = "<!-- BEGIN GENERATED EVALUATION -->", "<!-- END GENERATED EVALUATION -->"
    # Keep the research interpretation, licensing audit and native validation
    # outside the regenerated metric block.
    generated = "\n".join(report(results).splitlines()[4:])
    if target.exists():
        existing = target.read_text("utf-8")
        if begin in existing and end in existing:
            prefix, remainder = existing.split(begin, 1)
            _, suffix = remainder.split(end, 1)
            target.write_text(prefix + begin + "\n" + generated + "\n" + end + suffix, "utf-8")
            return
    target.write_text("# 识别模型补充评估（2026-10-01）\n\n" + begin + "\n" + generated + "\n" + end + "\n", "utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--threads", type=int, default=8)
    parser.add_argument("--only-model", choices=["r50", "auraface", "sface", "auraface_int8", "seeta_light"])
    args = parser.parse_args()
    cv2.setNumThreads(1)
    records = json.loads((DATA / "samples.json").read_text("utf-8"))["records"]
    chips = list(np.load(DATA / "chips.npz")["chips"])
    paths = {"r50": DEFAULT_MODELS_DIR / "w600k_r50.onnx", "auraface": DATA / "models" / "auraface_v1.onnx",
             "sface": DATA / "models" / "sface_2021dec.onnx", "auraface_int8": quantize(records, chips, args.threads)}
    seeta = DATA / "models" / "seetaface6_light.onnx"
    if seeta.exists():
        metadata = json.loads((HERE / "recognizer_research_candidates.json").read_text("utf-8"))["seeta_light"]
        if metadata["sha256"] != sha256(seeta):
            raise ValueError("Unverified SeetaFace6 research weight")
        paths["seeta_light"] = seeta
    results_path = DATA / "refined_results.json"
    if args.only_model and not results_path.exists():
        parser.error("Run the full comparison before --only-model")
    results = json.loads(results_path.read_text("utf-8"))["results"] if args.only_model and results_path.exists() else {}
    if args.only_model and args.only_model not in paths:
        parser.error("Requested research weight is missing")
    for key, path in paths.items():
        if args.only_model and key != args.only_model:
            continue
        kind = "auraface" if key == "auraface_int8" else key
        print(key, flush=True)
        candidate = ResearchCandidate(kind, path, args.threads)
        variants = {}
        for variant in ("original", "32px", "blur1.5"):
            cache = DATA / f"refined_{key}_{variant}.npz"
            model_hash = sha256(path)
            if cache.exists() and str(np.load(cache)["sha256"]) == model_hash:
                variants[variant] = np.load(cache)["embeddings"]
            else:
                values, norms = embed(candidate, degrade(chips, variant))
                variants[variant] = values
                np.savez_compressed(cache, embeddings=values, norms=norms, sha256=model_hash)
        del candidate
        gc.collect()
        result = {"sha256": sha256(path), "mb": path.stat().st_size / 1e6,
                  "accuracy": analyze(records, variants["original"], variants), "bench": benchmark(kind, path, chips)}
        if key == "auraface_int8":
            baseline = np.load(DATA / "refined_auraface_original.npz")["embeddings"]
            result["fp32_cosine"] = summary(np.sum(baseline * variants["original"], axis=1))
        results[key] = result
        (DATA / "refined_results.json").write_text(json.dumps({"platform": platform.platform(), "ort": ort.__version__, "results": results}, indent=2), "utf-8")
    camera = camera_evaluation(paths, results)
    if camera is not None:
        (DATA / "local_camera_results.json").write_text(json.dumps(camera, indent=2), "utf-8")
    else:
        (DATA / "local_camera_results.json").unlink(missing_ok=True)
    target = HERE.parents[1] / "docs" / "work" / "completed" / "recognizer-refined-evaluation.md"
    write_report(target, results)
    print(target, flush=True)


if __name__ == "__main__":
    main()
