# -*- coding: utf-8 -*-
"""投毒仿真——渐进学习阶段 1 的发布门禁(Biggio 2012 / LOPA 式线性注入)。

复刻 TemplateLearner 的策略数学(门控 0.55 / α=0.10 / 无日内衰减的最坏情况),
在合成嵌入上断言两条安全性质:

  P1 门外攻击者零位移:距离 ∈ [gate, 阈值) 的攻击者(能通过认证!)提交任意多次,
     模板一个字节都不动——更新门远严于认证门的直接后果。
  P2 门内攻击者位移有界:最坏情况(距离恰为 gate 的攻击者)被连续接受 k 次,
     模板位移 ≤ 1-(1-α)^k(解析上界),与 docs/progressive-learning-v2.md §3 一致。

用法: python poison_sim.py   (纯合成数据,零依赖,自包含)
"""
import math
import sys

GATE = 0.60      # learning_distance_gate 上限(2026-09-03 修订;实际门=min(用户纪元 p20, 该值),仿真取上限=最坏)
AUTH = 0.80      # match_threshold
ALPHA = 0.10     # learning_alpha(最坏:每日首笔,无衰减)


def dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def norm(v):
    n = math.sqrt(sum(x * x for x in v))
    return [x / n for x in v]


def blend(t, s, alpha):
    """TemplateLearner::BlendEMA 的 Python 复刻:归一化后的线性混合再归一化。"""
    return norm([alpha * x + (1 - alpha) * y for x, y in zip(s, t)])


def run_attacker(t0, attacker, rounds, gate=GATE, alpha=ALPHA):
    """按生产策略跑 rounds 次注入,返回(最终模板, 被接受的次数)。"""
    t = list(t0)
    accepted = 0
    for _ in range(rounds):
        if dist(attacker, t) <= gate:
            t = blend(t, attacker, alpha)
            accepted += 1
    return t, accepted


def main() -> int:
    rng_seed = 42
    import random
    rng = random.Random(rng_seed)
    dim = 512
    t0 = norm([rng.gauss(0, 1) for _ in range(dim)])
    far = norm([rng.gauss(0, 1) for _ in range(dim)])   # 异人参考点
    print(f"初始模板-异人距离: {dist(t0, far):.3f} (异人带 ≥1.25,合成空间仅作相对参考)")

    failures = 0

    # ---- P1: 门外攻击者(能通过 0.80 认证,但够不到更新门) ----
    for d in (0.61, 0.70, 0.79):   # 0.79 = 贴着认证线通过解锁的最强门外攻击者
        # 构造距 t0 严格小于 d 的攻击者:沿 t0→far 方向的球面插值,二分取内侧解
        lo, hi = 0.0, 1.0
        for _ in range(60):
            mid = (lo + hi) / 2
            if dist(t0, blend(t0, far, mid)) < d:
                lo = mid
            else:
                hi = mid
        attacker = blend(t0, far, lo)
        t, accepted = run_attacker(t0, attacker, rounds=100)
        moved = dist(t0, t)
        ok = accepted == 0 and moved < 1e-9
        print(f"P1 d<{d:.2f}: 注入 100 次, 接受 {accepted}, 位移 {moved:.2e}  "
              + ("PASS" if ok else "FAIL"))
        failures += 0 if ok else 1

    # ---- P2: 门内最坏攻击者(距离恰为 gate,严格内侧)----
    lo, hi = 0.0, 1.0
    for _ in range(60):
        mid = (lo + hi) / 2
        if dist(t0, blend(t0, far, mid)) < GATE:
            lo = mid
        else:
            hi = mid
    worst = blend(t0, far, lo)
    for k in (1, 5, 10, 20, 50):
        t, accepted = run_attacker(t0, worst, rounds=k)
        moved = dist(t0, t)
        # 硬上界:位移永不超过攻击者初始距离(模板不会越过攻击者)。
        # 紧回归门:朴素上界 (1-(1-α)^k)·d₀ 对"每步重归一化"存在 ~1e-5
        # 量级的球面超出(重归一化使有效步长略大于 1-α),留 0.1% 余量
        # 防回归(α/门控被改大时会大幅越界,1e-3 足以抓住)。
        hard = GATE
        tight = (1 - (1 - ALPHA) ** k) * GATE * 1.001
        ok = accepted == k and moved <= hard + 1e-9 and moved <= tight
        print(f"P2 k={k:2d}: 接受 {accepted}, 位移 {moved:.4f} ≤ 紧上界 {tight:.4f}  "
              + ("PASS" if ok else "FAIL"))
        failures += 0 if ok else 1

    if failures:
        print(f"poison_sim: {failures} FAIL")
        return 1
    print("poison_sim: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
