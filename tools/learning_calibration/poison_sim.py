# -*- coding: utf-8 -*-
"""投毒仿真——渐进学习自动通道(成功路径 EMA)的发布门禁。

复刻 TemplateLearner 的策略数学(2026-09-04 定稿版),在合成嵌入上断言:

  P0 跨锥零位移:距离过门但姿态在模板 ±25° 锥外的帧(跨角度/换姿势),
     一个字节都不动——锥门挡住"错槽更新"(V4 存量模板无标称角,
     锥门退化为开启,由 P1/P2 的距离与位移界兜底)。
  P1 门外攻击者零位移:距离 ∈ [gate, 阈值) 的攻击者(能通过认证!)提交
     任意多次,模板不动——更新门远严于认证门的直接后果。
  P2 门内攻击者位移有界:最坏情况(距离恰为 gate 的攻击者)被连续接受
     k 次,位移 ≤ 1-(1-α)^k(解析上界,0.1% 球面余量),且永不越过
     攻击者初始位置(硬上界)。

用法: python poison_sim.py   (纯合成数据,零依赖,自包含)
"""
import math
import sys

GATE = 0.65      # learning_distance_gate 上限(2026-09-04 定稿;实际门=min(用户纪元 p20, 该值),仿真取上限=最坏)
AUTH = 0.80      # match_threshold
ALPHA = 0.10     # learning_alpha(最坏:每日首笔,无衰减)
CONE = 25.0      # 姿态锥半角(度);None = 模板无标称角(V4 存量)→ 锥门开启
CLARITY = 0.10   # 落点明确性门(次近脸 − 命中脸 ≥ 0.10)


def dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def norm(v):
    n = math.sqrt(sum(x * x for x in v))
    return [x / n for x in v]


def blend(t, s, alpha):
    """TemplateLearner::BlendEMA 的 Python 复刻:归一化后的线性混合再归一化。"""
    return norm([alpha * x + (1 - alpha) * y for x, y in zip(s, t)])


def accepted(attacker, template, nominal_yaw, attacker_yaw):
    """生产门控链的距离+锥门复刻(范数/间隔/明确性与 P0-P2 无关,恒过)。"""
    d = dist(attacker, template)
    if d > GATE:
        return False
    if nominal_yaw is not None and abs(attacker_yaw - nominal_yaw) >= CONE:
        return False
    return True


def run_attacker(t0, attacker, rounds, nominal_yaw=None, attacker_yaw=0.0):
    """按生产策略跑 rounds 次注入,返回(最终模板, 被接受的次数)。"""
    t = list(t0)
    accepted_count = 0
    for _ in range(rounds):
        if accepted(attacker, t, nominal_yaw, attacker_yaw):
            t = blend(t, attacker, ALPHA)
            accepted_count += 1
    return t, accepted_count


def interpolate_toward(t0, far, target_d):
    """球面插值出距 t0 严格小于 target_d 的点(二分取内侧解)。"""
    lo, hi = 0.0, 1.0
    for _ in range(60):
        mid = (lo + hi) / 2
        if dist(t0, blend(t0, far, mid)) < target_d:
            lo = mid
        else:
            hi = mid
    return blend(t0, far, lo)


def main() -> int:
    import random
    rng = random.Random(42)
    dim = 512
    t0 = norm([rng.gauss(0, 1) for _ in range(dim)])
    far = norm([rng.gauss(0, 1) for _ in range(dim)])   # 异人参考点
    print(f"初始模板-异人距离: {dist(t0, far):.3f} (异人带 ≥1.25,合成空间仅作相对参考)")

    failures = 0

    # ---- P0: 跨锥帧(距离过门,姿态在锥外)零位移 ----
    # V5 模板(标称角 0°)遭遇同侧锥内攻击者已由 P2 覆盖;这里构造
    # "距离 ≤ gate 但 yaw 在 ±25° 之外"的帧——多角度账户里另一个角度的
    # 真人帧即此形态,攻击者同样可以利用。锥门必须全部拒绝。
    in_gate = interpolate_toward(t0, far, GATE)
    for yaw in (25.0, 40.0, 90.0):
        t, acc = run_attacker(t0, in_gate, rounds=100, nominal_yaw=0.0,
                              attacker_yaw=yaw)
        moved = dist(t0, t)
        ok = acc == 0 and moved < 1e-9
        print(f"P0 yaw={yaw:4.0f}: 注入 100 次, 接受 {acc}, 位移 {moved:.2e}  "
              + ("PASS" if ok else "FAIL"))
        failures += 0 if ok else 1
    # 对照:锥内(yaw=10°)同一向量可被接受——证明 P0 拒绝来自锥门而非构造错误。
    t, acc = run_attacker(t0, in_gate, rounds=1, nominal_yaw=0.0, attacker_yaw=10.0)
    if acc != 1:
        print("P0 对照(锥内)失败: 距离过门的锥内帧应被接受")
        failures += 1

    # ---- P1: 门外攻击者(能通过 0.80 认证,但够不到更新门) ----
    for d in (0.66, 0.70, 0.79):   # 0.79 = 贴着认证线通过解锁的最强门外攻击者
        attacker = interpolate_toward(t0, far, d)
        t, acc = run_attacker(t0, attacker, rounds=100)
        moved = dist(t0, t)
        ok = acc == 0 and moved < 1e-9
        print(f"P1 d<{d:.2f}: 注入 100 次, 接受 {acc}, 位移 {moved:.2e}  "
              + ("PASS" if ok else "FAIL"))
        failures += 0 if ok else 1

    # ---- P2: 门内最坏攻击者(距离恰为 gate,严格内侧)----
    worst = interpolate_toward(t0, far, GATE)
    for k in (1, 5, 10, 20, 50):
        t, acc = run_attacker(t0, worst, rounds=k)
        moved = dist(t0, t)
        # 硬上界:位移永不超过攻击者初始距离(模板不会越过攻击者)。
        # 紧回归门:朴素上界 (1-(1-α)^k)·d₀ 对"每步重归一化"存在 ~1e-5
        # 量级的球面超出(重归一化使有效步长略大于 1-α),留 0.1% 余量
        # 防回归(α/门控被改大时会大幅越界,1e-3 足以抓住)。
        hard = GATE
        tight = (1 - (1 - ALPHA) ** k) * GATE * 1.001
        ok = acc == k and moved <= hard + 1e-9 and moved <= tight
        print(f"P2 k={k:2d}: 接受 {acc}, 位移 {moved:.4f} ≤ 紧上界 {tight:.4f}  "
              + ("PASS" if ok else "FAIL"))
        failures += 0 if ok else 1

    if failures:
        print(f"poison_sim: {failures} FAIL")
        return 1
    print("poison_sim: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
