#ifndef FOLDERREGIONJUMP_H
#define FOLDERREGIONJUMP_H

/*
 * "目录对比区域"之间跳转的**落点规则**(纯函数,单测覆盖)。
 *
 * 为什么单独拿出来:
 *   这段判定以前直接写在 ImageDeduplicateWindow 的两个函数里,只能靠人肉点界面
 *   验证,而它偏偏有两个容易写反的地方:
 *     · next 要求落到**Pending**(处理过的对不该再停下来);
 *     · prev 只要求**有效**(非 invalid),不要求 Pending —— 回看就是要看
 *       已经处理完的那一批;
 *   两者还都要求"区域键和当前行不一样"(区域 = A、B 的直属父目录对,
 *   见 pairDirSortKey / foldercompare::folderPairSortKey)。
 *   抽成纯函数之后,给的是一张 (区域键, 状态, 失效位) 表,和窗口解耦。
 *
 * 行描述只用 state + invalidMask 两个字段,不碰文件/路径,所以这个头文件
 * 只依赖 pairstate_math.h,能被独立测试程序直接 include。
 *
 * 访问方式用模板回调而不是 QVector:窗口里 m_pairs 可能上万条,而每次跳转
 * 真正要判定的只有"从当前行往后/往前到目标为止"那几行 —— 用 rowAt(i) 按需
 * 读,才不用为了跳一次把整表的区域键都算一遍(pairDirSortKey 要做路径规范化)。
 */

#include "pairstate_math.h"

#include <QString>

namespace folderregion {

/* 一行 = 一对。region 为空表示"这一行没有区域"(空 pair / 越界 / 两边取不到目录) */
struct Row {
    QString  region;
    PairState state       = PairState::Pending;
    quint8    invalidMask = 0;
};

/*
 * 向后跳:下一个区域的**第一条 Pending**。
 *
 *   · 当前区域里剩下的同目录对直接跳过 —— 普通的 next 只挪一对,一个区域里
 *     几十对同目录图要按几十次才出得去;
 *   · 落点必须是 Pending:已经被 remove / jump 掉的对不是"下一个要做的事";
 *     另外失效位非 0 的也不收(那对引用的文件已经被别的对搬走了,停在那里
 *     什么也做不了);
 *   · 区域键和当前行相同的不算"下一个区域"。
 *
 * currentIndex < 0(还没开始/越界)时按"空区域"处理,即第一次 Pending 就落。
 * 找不到返回 -1。
 */
template <typename RowFn>
inline int nextPendingRow(int rowCount, const RowFn &rowAt, int currentIndex)
{
    const Row cur = (currentIndex >= 0 && currentIndex < rowCount) ? rowAt(currentIndex) : Row{};

    for (int i = (currentIndex < 0 ? 0 : currentIndex + 1); i < rowCount; ++i) {
        const Row r = rowAt(i);
        if (r.region.isEmpty()) continue;                       // 空行没有区域,不算落点
        if (r.region == cur.region) continue;                   // 还在当前区域里
        if (r.state != PairState::Pending) continue;             // 只停 Pending
        if (pairLooksInvalid(r.state, r.invalidMask)) continue;  // 失效的 Pending 也不收
        return i;
    }
    return -1;
}

/*
 * 向前跳:上一个区域的**最靠下的一条有效对**。
 *
 *   · "有效"= pairLooksInvalid() 为假,不看是不是 Pending —— 从后往前过的时候
 *     用户要看的就是这个区域尾部那一批(通常刚扫进来、或者刚刚处理完),
 *     要求 Pending 会让几乎所有回看都落空;
 *   · "最靠下"而不是"最靠上":落点必须是区域尾部。扫描是从尾往头走的,
 *     所以**第一次**遇到有效对就要停 —— 继续往前扫只会退化成"区域第一条",
 *     这正是以前写错的地方(赋值没 break,循环结束时留下的是最小下标);
 *   · 整个区域一对有效对都没有时,退到该区域最靠下的那一行:宁可停在一条
 *     失效对上,也不能"按了没反应"(用户会当成快捷键没接上)。
 *
 * currentIndex < 0 时从表尾开始找。找不到(前面没有别的区域)返回 -1。
 */
template <typename RowFn>
inline int lastValidRowOfPreviousRegion(int rowCount, const RowFn &rowAt, int currentIndex)
{
    if (rowCount <= 0)
        return -1;

    const Row cur = (currentIndex >= 0 && currentIndex < rowCount) ? rowAt(currentIndex) : Row{};

    /* 1) 找上一个区域:从当前行往前,第一行"区域键和当前不同"的就是它的最靠下行 */
    QString prevRegion;
    int bottom = -1;
    for (int i = (currentIndex < 0 ? rowCount - 1 : currentIndex - 1); i >= 0; --i) {
        const Row r = rowAt(i);
        if (r.region.isEmpty()) continue;      // 空行既不参与判定,也不当边界
        if (r.region == cur.region) continue;  // 还在当前区域里
        prevRegion = r.region;
        bottom = i;
        break;
    }
    if (bottom < 0)
        return -1;

    /* 2) 在这个区域里从尾往头找第一条有效对;没找到就用区域最靠下的那一行 */
    for (int i = bottom; i >= 0; --i) {
        const Row r = rowAt(i);
        if (r.region.isEmpty()) continue;      // 区域内部的空行不该截断扫描
        if (r.region != prevRegion) break;     // 走出这个区域
        if (!pairLooksInvalid(r.state, r.invalidMask))
            return i;
    }
    return bottom;
}

} // namespace folderregion

#endif // FOLDERREGIONJUMP_H
