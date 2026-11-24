#ifndef FOLDERGROUP_H
#define FOLDERGROUP_H

/*
 * 目录对(= 一个"文件夹组")之间的排序权重。纯函数,有单测。
 *
 * ---------------------------- 要解决的问题 ----------------------------
 *
 * 半选 internal 模式里,图片对按"目录对"分组(2026-10-28 起是 A、B 各自
 * 的**直属父目录**对,以前是顶层子目录对 —— 调用方喂什么目录,这里就按什么分桶)。原来的组间顺序是
 * 目录路径的字典序(dirKey),这在下面这种局面里非常难受:
 *
 *      A: 给 C 加了滤镜/特效的变体(和 C 不算重复,只是相似)
 *      B: C 的高质量原图版本(B 与 C 是有合集关系的)
 *      C: 一堆低质量图
 *
 * A 通常是几百张,于是 A-C 之间的相似对轻轻松松上千条;而 B-C 只有百来条。
 * 字典序一比,A-C 整组排到了 B-C 前面 —— 用户得先翻完上千条"其实不该删"的
 * 滤镜变体,才能开始真正该做的 B-C。
 *
 * ---------------------------- 排序权重怎么算 ----------------------------
 *
 * 以"子集侧"(要被解释的那一侧,也就是 C)为参照,统计两件事:
 *
 *   1) covered  = 这一组里,子集侧**有多少张不同的图片**参与了相似对。
 *                 注意是"图片数"不是"对数":C 里一张图被 A 里 30 张变体配上,
 *                 只算 1 张 —— 这正是"大量 A-C pair"不能靠数量取胜的原因。
 *   2) avgBest  = 这些图片**各自最像的那一对**的相似度均值(0..1)。
 *                 每张图只取最优的一对,避免"一张图配了 30 个烂对"把均值拖坏。
 *
 *   score = covered * avgBest
 *
 * 覆盖得多、而且质量高 → 这一组越该排在前面。因为进入列表的 pair 都已经过
 * 相似度阈值,avgBest 只会在 0.2..1.0 之间波动,所以 score 基本上就是
 * "有效覆盖量"排在第一位、相似质量做微调 —— 与"数量优先,平均最高相似度次之"
 * 的口径一致。
 *
 * ---------------------- 为什么不含 pix diff ----------------------
 *
 * pix diff 是异步算出来的(大图一对要 1 秒级),而组间顺序必须是**稳定**的:
 * 如果它随 pix 结果变化,用户在处理到一半时整个列表会重新分组、当前行被搬走 ——
 * 这在 35000 对的会话里是不可接受的。组内排序里 pix 已经是第二键(见
 * sortPairsFromIndex),足以体现像素层面的差异。
 *
 * 组间只用**扫描阶段就有**的三个指标:pHash 距离、dHash 距离、ORB 匹配率。
 */

#include <QHash>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace foldergroup {

/* 与去重窗口的相似度阈值保持一致(调用方从成员变量填进来) */
struct Thresholds {
    int    pHash = 6;
    int    dHash = 1;
    double orb   = 0.18;
};

/*
 * 一对图片的原始指标。
 * refFile 是"子集侧那张图"的稳定 id(窗口里就是 m_files 的下标)。
 * 同一个 refFile 出现多次没关系 —— summarize() 只留最优的那一对。
 */
struct Sample {
    int    refFile   = -1;
    int    pHashDist = 0;
    int    dHashDist = 0;
    double orb       = 0.0;
};

inline double clamp01(double v)
{
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

/*
 * 单对相似度 0..1(1 = 最像)。
 *
 * 三个指标各自归一化到 0..1 再加权:
 *   · pHash:距离 0 → 1,距离 = 阈值 → 0。它是组内排序的第一键,权重最大。
 *   · dHash:同理,阈值很紧(默认 1),只当补充。
 *   · ORB  :本身就在 0..1,直接当相似度用(0.19 的勉强命中就该低分)。
 *
 * 不使用 pix:见文件头的说明 —— 组间顺序不能随异步结果变化。
 */
inline double similarity(int pHashDist, int dHashDist, double orb, const Thresholds &t)
{
    const int ph = std::max(1, t.pHash);
    const int dh = std::max(1, t.dHash);
    const double sPH  = clamp01(1.0 - double(std::max(0, pHashDist)) / double(ph));
    const double sDH  = clamp01(1.0 - double(std::max(0, dHashDist)) / double(dh));
    const double sORB = clamp01(orb);
    return 0.55 * sPH + 0.15 * sDH + 0.30 * sORB;
}

struct Stat {
    int    covered   = 0;      // 子集侧参与相似对的图片数(去重后)
    int    pairCount = 0;      // 这一组的总对数(只用于日志/调试)
    double avgBest   = 0.0;    // 每张图最优相似度的均值

    /* 展示/日志用:覆盖量 × 质量 */
    double score() const { return double(covered) * avgBest; }

    /*
     * 组间排序键(越大越先做),编码成单个整数:
     *
     *     rank = covered * 1000 + round(avgBest * 10)
     *
     * 为什么把 avgBest 分档到 0.1 而不是直接比 score:
     *   · 主键必须是"覆盖了多少张图"(用户口径:数量优先);
     *   · ORB 匹配率本身有 ±0.05 量级的抖动,换算到相似度约 ±0.015。
     *     不分档的话,两组覆盖率完全一样、只是 ORB 抖了 0.004,整组的先后
     *     就会跟着翻 —— 同一批数据两次跑出两个顺序,验收/复现都没法做。
     *     0.1 一档 = 只要不是真的差一截,就当同一档,用目录名兜底。
     *
     * 单一整数也让比较器天然满足严格弱序(不会有浮点相等判断的坑)。
     */
    qint64 rank() const
    {
        const int grade = int(std::lround(avgBest * 10.0));   // 0..10
        return qint64(covered) * 1000 + qint64(grade);
    }
};

/* 按 refFile 归并(每张图只留最优),再求均值 */
inline Stat summarize(const QVector<Sample> &samples, const Thresholds &t)
{
    Stat s;
    s.pairCount = samples.size();
    if (samples.isEmpty())
        return s;

    QHash<int, double> best;
    best.reserve(samples.size());
    for (const Sample &x : samples) {
        const double sim = similarity(x.pHashDist, x.dHashDist, x.orb, t);
        auto it = best.find(x.refFile);
        if (it == best.end())
            best.insert(x.refFile, sim);
        else if (sim > it.value())
            it.value() = sim;
    }

    s.covered = best.size();
    if (s.covered <= 0)
        return s;

    double sum = 0.0;
    for (double v : best)
        sum += v;
    s.avgBest = sum / double(s.covered);
    return s;
}

/*
 * 没有合集方向时,拿哪一侧当参照?
 *
 * 取**覆盖图片更少**的那一侧:它才是"被另一侧解释"的那一堆。
 * 例:A 有 300 张滤镜变体,全部配上了 C 的 50 张 —— 两侧覆盖数分别是 300 / 50,
 * 拿 50 这一侧当参照才能问出"这 50 张各自最像的是谁",拿 300 侧算出来的
 * 均值会被同一批图的多个变体稀释。
 *
 * 两侧覆盖数相同(常见于一对一的大小相近目录):两边的平均质量取中间值,
 * 谁都不占便宜。
 */
inline Stat pickReference(const Stat &fromA, const Stat &fromB)
{
    if (fromA.covered != fromB.covered)
        return (fromA.covered < fromB.covered) ? fromA : fromB;
    if (fromA.covered <= 0)
        return fromA;

    Stat s = fromA;
    s.avgBest   = (fromA.avgBest + fromB.avgBest) / 2.0;
    s.pairCount = fromA.pairCount + fromB.pairCount;
    return s;
}

/*
 * 组内的一对原始记录:两个文件各自属于哪个目录 + 三个相似度指标。
 *
 * dirA/dirB 是**文件所属的目录**(窗口里传直属父目录),不是 pair 的 A/B 位 ——
 * 同一组里摆位是乱的(合集优先 + 逐对质量择优),这一对的 A 在左目录、
 * 下一对的 A 可能在右目录。这里按目录分桶,所以摆位怎么翻都不影响结果。
 * 注意:keepDir 必须是同一粒度(= 组内某一侧目录本身),调用方负责换算。
 */
struct RawPair {
    QString dirA, dirB;
    int     fileA = -1, fileB = -1;
    int     pHashDist = 0;
    int     dHashDist = 0;
    double  orb = 0.0;
};

/*
 * 一个目录对的统计。
 * keepDir:已知的合集侧(空 = 没有方向)。合集侧的对面就是"子集侧",也就是参照侧。
 */
inline Stat summarizeDirPair(const QVector<RawPair> &pairs, const QString &keepDir,
                             const Thresholds &t)
{
    Stat empty;
    if (pairs.isEmpty())
        return empty;

    QString dir1 = pairs.first().dirA;
    QString dir2 = pairs.first().dirB;
    if (dir1 > dir2)
        std::swap(dir1, dir2);

    QVector<Sample> s1, s2;
    s1.reserve(pairs.size());
    s2.reserve(pairs.size());
    for (const RawPair &p : pairs) {
        const Sample sa{p.fileA, p.pHashDist, p.dHashDist, p.orb};
        const Sample sb{p.fileB, p.pHashDist, p.dHashDist, p.orb};
        (p.dirA == dir1 ? s1 : s2).push_back(sa);
        (p.dirB == dir1 ? s1 : s2).push_back(sb);
    }

    const Stat v1 = summarize(s1, t);
    const Stat v2 = summarize(s2, t);
    if (!keepDir.isEmpty())
        return (keepDir == dir1) ? v2 : v1;   // 参照合集侧的另一边
    return pickReference(v1, v2);
}

} // namespace foldergroup

#endif // FOLDERGROUP_H
