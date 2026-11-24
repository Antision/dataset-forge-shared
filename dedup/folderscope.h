#ifndef FOLDERSCOPE_H
#define FOLDERSCOPE_H

/*
 * 目录级操作的作用域判定(纯函数,单测覆盖)。
 *
 * "这两个目录"= 两张图各自的**直接父目录**。Jump These Folders 和
 * Remove A/B/Both(目录级)共用同一套范围定义:
 *   · 方向无关:一对 (dirA, dirB) 与 (dirB, dirA) 是同一个目录对;
 *   · 只认同层关系:嵌套一层的子目录、同层其它子目录都不算。
 *
 * 之所以单独拿出来:按"每对的 A/B 位"删文件是错的 —— 摆位(合集优先)
 * 会让同一张图在不同对里出现在 A 或 B 位,按位删会删错边。
 * 正确的口径是"删掉这些对里落在某个**目录**里的那一张"。
 */

#include <QDir>
#include <QSet>
#include <QString>

#include <utility>

namespace folderscope {

/*
 * 目录对的规范化键:方向无关、大小写无关、分隔符无关。
 *
 * 全工程只有这一份实现(窗口里原来的 dirPairKeyOf 也转调它),因为
 * m_folderKeep 的键、目录级 remove/jump 的键、"被 jump 的目录对"的键
 * 必须是同一个东西 —— 一旦有两套写法,就会出现"明明 jump 了却还在列表里"。
 */
inline QString pairKey(const QString &a, const QString &b)
{
    if (a.isEmpty() || b.isEmpty())
        return QString();
    QString k1 = QDir::cleanPath(QDir::fromNativeSeparators(a)).toLower();
    QString k2 = QDir::cleanPath(QDir::fromNativeSeparators(b)).toLower();
    if (k1 == k2)
        return QString();
    if (k1 > k2)
        std::swap(k1, k2);
    return k1 + QLatin1Char('|') + k2;
}

/* 这一对是否落在"这两个目录"之间(方向无关) */
inline bool inFolderPair(const QString &dirA, const QString &dirB,
                         const QString &pairDirA, const QString &pairDirB)
{
    if (dirA.isEmpty() || dirB.isEmpty() || dirA == dirB)
        return false;
    if (pairDirA.isEmpty() || pairDirB.isEmpty())
        return false;
    return (pairDirA == dirA && pairDirB == dirB)
        || (pairDirA == dirB && pairDirB == dirA);
}

/*
 * 这一对(两个顶层子目录)是不是被 jump 掉的目录对之一。
 *
 * jump = 这两个目录之间**不比**,所以它们之间的相似对要整个剔掉,
 * 而不是留成"没有合集方向的散图对"。键的写法由 pairKey 统一,避免出现
 * "jump 了却还在列表里"这种键对不上的问题。
 */
inline bool isJumpedPair(const QSet<QString> &jumped, const QString &dirA, const QString &dirB)
{
    const QString k = pairKey(dirA, dirB);
    return !k.isEmpty() && jumped.contains(k);
}

/*
 * 该删这一对的哪一侧。返回 0=A 位、1=B 位、2=两侧。
 * side:     0 = 删"目标目录 A 里的图",1 = 删"目标目录 B 里的图",2 = 两侧都删
 * pairDirA: 这一对的 A 位文件所在目录
 */
inline int removeSideFor(const QString &targetDirA, const QString &targetDirB,
                         const QString &pairDirA, int side)
{
    if (side == 2) return 2;
    const QString target = (side == 0) ? targetDirA : targetDirB;
    if (target.isEmpty()) return -1;
    return (pairDirA == target) ? 0 : 1;
}

} // namespace folderscope

#endif // FOLDERSCOPE_H
