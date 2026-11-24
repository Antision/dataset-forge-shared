#ifndef PAIRSTATE_MATH_H
#define PAIRSTATE_MATH_H

/*
 * 图片对状态 + "叠加态"的纯运算。
 *
 * 为什么单独抽出来:
 *   "筛选出与这张图相似的所有配对" 会在失效的对上再删掉另一侧,于是出现
 *   "invalid d + removed e" 这种一维枚举表达不了的状态。这里用
 *   state(主状态) + removedMask(本对删过哪一侧) 两个字段表示,
 *   所有翻转/合并规则都集中在本文件,既能被窗口直接调用,也能单独跑单测
 *   (见 build/import_test/foldercompare_test.cpp 的第 16 组)。
 *
 * 约定:
 *   · removedMask 只在 state 是 Invalid* 时可能非 0;
 *   · 有效对(RemovedA/B/Both)用状态本身就够了,不再保留叠加位;
 *   · Invalid* -> Pending 的那一刻必须把叠加位翻译成真状态,否则会出现
 *     "状态说 Pending、但那一侧的文件还在 duplicate 里"。
 */

#include <QtGlobal>

enum PairState {
    Pending,      // 等待处理
    RemovedA,     // A 被移除
    RemovedB,
    RemovedBoth,
    Skipped,      // jump
    InvalidA,     // A 在其它的对中被移除
    InvalidB,
    InvalidBoth
};

/* 是否为 Invalid 状态 */
inline bool isInvalidPairState(PairState s) {
    return s == PairState::InvalidA || s == PairState::InvalidB
        || s == PairState::InvalidBoth;
}

/* removedMask 的位:哪一侧被"本对"移除过 */
enum : quint8 {
    SideBitA = 1,
    SideBitB = 2
};

/*
 * 本对移除了某一侧之后的状态。
 *   · 有效对(Pending/Skipped/Removed*) -> 收敛成 RemovedA/B/Both,叠加位清零;
 *   · 失效对 -> 状态保持 Invalid*(点它还能跳回肇事那一对),只记叠加位。
 */
inline void applyRemovalToState(PairState &state, quint8 &mask, bool sideA)
{
    const quint8 bit = sideA ? SideBitA : SideBitB;

    if (isInvalidPairState(state)) {
        mask = quint8(mask | bit);
        return;
    }

    quint8 m = quint8(mask | bit);
    if (state == PairState::RemovedA)       m = quint8(m | SideBitA);
    else if (state == PairState::RemovedB)  m = quint8(m | SideBitB);
    else if (state == PairState::RemovedBoth) m = quint8(m | SideBitA | SideBitB);

    mask = 0;
    state = ((m & (SideBitA | SideBitB)) == (SideBitA | SideBitB))
                ? PairState::RemovedBoth
                : ((m & SideBitA) ? PairState::RemovedA : PairState::RemovedB);
}

/*
 * 左右对调之后的状态。
 * RemovedA/InvalidA 这些是**相对当前 A/B** 的,只换下标不换状态,
 * "已移除"就会跑到另一侧去。
 */
inline quint8 flipMaskSides(quint8 mask)
{
    const quint8 wasA = (mask & SideBitA) ? SideBitB : 0;
    const quint8 wasB = (mask & SideBitB) ? SideBitA : 0;
    return quint8(wasA | wasB);
}

inline void flipStateSides(PairState &state, quint8 &mask)
{
    switch (state) {
    case PairState::RemovedA: state = PairState::RemovedB; break;
    case PairState::RemovedB: state = PairState::RemovedA; break;
    case PairState::InvalidA: state = PairState::InvalidB; break;
    case PairState::InvalidB: state = PairState::InvalidA; break;
    default: break;   // Pending / Skipped / RemovedBoth / InvalidBoth 左右对称
    }

    mask = flipMaskSides(mask);
}

/* Invalid* -> Pending 时,把"叠加态里已经删掉的那一侧"翻译成真状态 */
inline void promoteInvalidState(PairState &state, quint8 &mask)
{
    if (mask == 0) {
        state = PairState::Pending;
        return;
    }
    const quint8 m = mask;
    mask = 0;
    state = ((m & (SideBitA | SideBitB)) == (SideBitA | SideBitB))
                ? PairState::RemovedBoth
                : ((m & SideBitA) ? PairState::RemovedA : PairState::RemovedB);
}

/*
 * ===== 第二条叠加轴:失效位(invalidMask)=====
 *
 * 为什么 state 不够用:
 *   InvalidA/InvalidB/InvalidBoth 只是 Pending 的一种变体。一对如果已经被
 *   判成 Skipped(jump)或 Removed*,它在 PairState 里就再也表达不出
 *   "我的某一侧被【别的对】移走了"这件事。
 *
 * 典型 bug(2026-11-04 用户报的):
 *   存在 A-B、B-C 两对;先 Jump 掉 A-B,再在 B-C 里把 B 移除。
 *   此时 A-B 仍是 Skipped —— 但它引用的 B 已经进了 duplicate/。
 *   接着把 A-B 改回 Pending,restorePairToPending 会把 B 从 duplicate 搬回来,
 *   而"真正删了 B"的 B-C 还写着 RemovedB:磁盘和列表从此对不上。
 *
 * 所以失效这件事必须独立记录:
 *   · invalidMask 记"哪一侧被别的对移走"(位掩码,和 removedMask 同构);
 *   · 它对 Pending 的处理不变(仍然降级成 Invalid*),这样老的显示/跳转逻辑照旧;
 *   · 对 Skipped / Removed* 则只置位、不改 state,显示成 "Skipped + InvalidB"。
 * 任何"要不要动文件"的判断都必须问 pairLooksInvalid(),不能只看 state。
 */
inline quint8 invalidBitsOfState(PairState s)
{
    switch (s) {
    case PairState::InvalidA:   return SideBitA;
    case PairState::InvalidB:   return SideBitB;
    case PairState::InvalidBoth: return quint8(SideBitA | SideBitB);
    default: return 0;
    }
}

/* 这一对是否"有某一侧已经不在了"(不管是本对删的还是别的对删的) */
inline bool pairLooksInvalid(PairState s, quint8 invalidMask)
{
    return isInvalidPairState(s) || invalidMask != 0;
}

/*
 * 本对【自己】删过哪一侧(位掩码)。
 *
 * 有效对的信息在 state 里(RemovedA/B/Both),失效对的信息在 removedMask 里
 * (状态要留在 Invalid* 才能跳回肇事那一对)。这个函数把两处合成一份,
 * 给"恢复"用:恢复只能撤销【本对】做过的事,不能替别的对做主。
 */
inline quint8 ownRemovalBits(PairState s, quint8 removedMask)
{
    quint8 m = removedMask;
    switch (s) {
    case PairState::RemovedA:    m = quint8(m | SideBitA); break;
    case PairState::RemovedB:    m = quint8(m | SideBitB); break;
    case PairState::RemovedBoth: m = quint8(m | SideBitA | SideBitB); break;
    default: break;   // Pending / Skipped / Invalid* 只看 removedMask
    }
    return m;
}

/* 由 invalidMask 反推"该显示成哪个 Invalid 状态" */
inline PairState invalidStateFromMask(quint8 mask)
{
    if (!(mask & SideBitA) && !(mask & SideBitB)) return PairState::Pending;
    if ((mask & SideBitA) && (mask & SideBitB))   return PairState::InvalidBoth;
    return (mask & SideBitA) ? PairState::InvalidA : PairState::InvalidB;
}


#endif // PAIRSTATE_MATH_H
