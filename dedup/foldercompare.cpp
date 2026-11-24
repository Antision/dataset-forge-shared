#include "foldercompare.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <bit>
#include <cmath>

namespace foldercompare {
namespace {

int hamming64(quint64 x, quint64 y)
{
    return int(std::popcount(x ^ y));
}

/*
 * 两张图片的相似度打分。
 *   >=3   : 二进制完全相同(最高优先,一个 A 只配一个 B)
 *   2..3  : ORB 命中
 *   1..2  : pHash/dHash 命中
 *   0     : 不相似
 *
 * 不用"0..1 的连续相似度"是因为贪心匹配要优先用强证据占位:
 * 一张 A 已经被 identical 配走,就不该再被 ORB 抢去。
 */
double similarityScore(const QVector<ImageItem> &images, int ia, int ib,
                       const Options &opt, bool *outIdentical)
{
    const ImageItem &x = images[ia];
    const ImageItem &y = images[ib];

    if (outIdentical) *outIdentical = false;

    if (!x.sha.isEmpty() && x.sha == y.sha) {
        if (outIdentical) *outIdentical = true;
        return 3.0;
    }

    const int pd = hamming64(x.pHash, y.pHash);
    const int dd = hamming64(x.dHash, y.dHash);
    const bool pHit = pd <= opt.pHashThreshold;
    const bool dHit = dd <= opt.dHashThreshold;

    /*
     * 便宜预筛:哈希至少得"接近"才值得算 ORB。
     * 这里的 2 倍阈值只是省时间,不是判据 —— 真正的结论由后面的几何校验给。
     */
    if (opt.orbMatch && (pHit || dHit
                         || (pd <= opt.pHashThreshold * 2 && dd <= opt.dHashThreshold * 2))) {
        const OrbMatch om = opt.orbMatch(ia, ib);
        if (om.inliers >= opt.orbMinInliers && om.inlierRatio >= opt.orbMinRatio)
            return 2.0 + std::min(1.0, std::max(0.0, om.inlierRatio));
    }

    if (!pHit && !dHit)
        return 0.0;

    const double score = std::max(1.0 - double(pd) / 64.0, 1.0 - double(dd) / 64.0);
    return 1.0 + std::min(1.0, std::max(0.0, score));
}

/*
 * 贪心一对一匹配:对 from 里的每张图,在 to 里找一张还没被占用的最相似的。
 * 顺序固定(from 的给定顺序),保证结果可复现。
 */
MatchStats matchDirs(const DirEntry &to, const DirEntry &from,
                     const QVector<ImageItem> &images, const Options &opt)
{
    MatchStats s;
    s.nA = to.count();
    s.nB = from.count();
    if (to.images.isEmpty() || from.images.isEmpty())
        return s;

    QVector<bool> used(to.images.size(), false);

    double qa = 0, qb = 0, sa = 0, sb = 0, pa = 0, pb = 0;
    double wa = 0, wb = 0, ha = 0, hb = 0;

    for (int bi : from.images) {
        int bestSlot = -1;
        double bestScore = 0.0;
        bool bestIdentical = false;

        for (int slot = 0; slot < to.images.size(); ++slot) {
            if (used[slot]) continue;
            bool identical = false;
            const double score = similarityScore(images, to.images[slot], bi, opt, &identical);
            if (score > bestScore) {
                bestScore = score;
                bestSlot = slot;
                bestIdentical = identical;
            }
        }

        if (bestSlot < 0)
            continue;

        used[bestSlot] = true;
        ++s.matched;
        if (bestIdentical) ++s.identical;

        const ImageItem &A = images[to.images[bestSlot]];
        const ImageItem &B = images[bi];
        qa += A.quality;   qb += B.quality;
        sa += A.filesize;  sb += B.filesize;
        pa += double(A.pixels()); pb += double(B.pixels());
        wa += A.width;  wb += B.width;
        ha += A.height; hb += B.height;
    }

    if (s.matched > 0) {
        const double n = double(s.matched);
        s.avgQualityA = qa / n;
        s.avgQualityB = qb / n;
        s.avgSizeA = sa / n / (1024.0 * 1024.0);
        s.avgSizeB = sb / n / (1024.0 * 1024.0);
        s.avgPixelsA = pa / n;
        s.avgPixelsB = pb / n;
        s.avgWidthA = wa / n;  s.avgWidthB = wb / n;
        s.avgHeightA = ha / n; s.avgHeightB = hb / n;
    }
    return s;
}

/* "小目录(子集)是否被大目录包含" */
bool contained(const MatchStats &s, const Options &opt, QString *why)
{
    const int nSmall = s.nB;
    const int nBig   = s.nA;
    if (nSmall <= 0 || nBig <= 0)
        return false;

    if (nSmall <= opt.smallDirMax) {
        // 少于 4 张:最多允许 1 张不相似;只有 1 张时必须全中
        const int unmatched = nSmall - s.matched;
        const bool ok = (nSmall == 1) ? (unmatched == 0 && nBig - nSmall > 1)
                                      : (unmatched <= 1 && nBig - nSmall > 1);
        if (why && ok) {
            *why = QStringLiteral("小目录 %1 张 / 大目录 %2 张,命中 %3 张")
                       .arg(nSmall).arg(nBig).arg(s.matched);
        }
        return ok;
    }

    if (nBig < nSmall)
        return false;

    const int need = int(std::ceil(double(nSmall) * opt.containRatio - 1e-9));
    const bool ok = s.matched >= need;
    if (why && ok) {
        *why = QStringLiteral("相似率 %1% (%2/%3,需要 %4 张)")
                   .arg(QString::number(100.0 * s.ratio(), 'f', 1))
                   .arg(s.matched).arg(nSmall).arg(need);
    }
    return ok;
}

bool qualityOk(const MatchStats &s, double ratio)
{
    auto notWorse = [ratio](double a, double b) {
        if (b <= 0.0) return true;         // 对方没有该项数据,不算劣
        return a + 1e-9 >= b * ratio;
    };
    return notWorse(s.avgQualityA, s.avgQualityB)
        && notWorse(s.avgSizeA, s.avgSizeB)
        && notWorse(s.avgPixelsA, s.avgPixelsB);
}

QString qualityNote(const MatchStats &s)
{
    return QStringLiteral("质量 %1/%2,大小 %3/%4 MB,像素 %5/%6")
        .arg(formatDouble(s.avgQualityA, 1), formatDouble(s.avgQualityB, 1),
             formatDouble(s.avgSizeA, 6), formatDouble(s.avgSizeB, 6),
             formatDouble(s.avgPixelsA, 0), formatDouble(s.avgPixelsB, 0));
}

/* 一个目录"整包"的画像（不依赖匹配结果，纯按目录内全部图片统计） */
struct DirProfile {
    double quality = 0.0;
    double pixels = 0.0;
    double sizeMB = 0.0;
};

DirProfile profileOf(const DirEntry &d, const QVector<ImageItem> &images)
{
    DirProfile p;
    if (d.images.isEmpty())
        return p;
    double q = 0, px = 0, sz = 0;
    for (int idx : d.images) {
        const ImageItem &it = images[idx];
        q += it.quality;
        px += double(it.pixels());
        sz += double(it.filesize);
    }
    const double n = double(d.images.size());
    p.quality = q / n;
    p.pixels = px / n;
    p.sizeMB = sz / n / (1024.0 * 1024.0);
    return p;
}

/*
 * 谁"更像合集"：用于给一对目录决定展示顺序（左/右）。
 *
 * 这一项**不参与**包含关系的判定（判定在 decide() 里按内容做），只决定
 * FolderCompareWidget 里谁是 A 列、以及日志里谁写在前面 ——
 * 否则用户会看到"更有可能是合集、画质也更好"的那个被排到右边，
 * 光看图完全理解不了。
 *
 * 顺序：递归图片数 > 目录深度 > 平均质量 > 平均像素 > 平均体积 > 目录名(稳定兜底)
 */
bool looksMoreLikeCollection(const DirEntry &x, const DirProfile &px,
                             const DirEntry &y, const DirProfile &py)
{
    if (x.count() != y.count()) return x.count() > y.count();
    if (x.depth != y.depth) return x.depth > y.depth;
    if (!qFuzzyCompare(px.quality + 1.0, py.quality + 1.0)) return px.quality > py.quality;
    if (!qFuzzyCompare(px.pixels + 1.0, py.pixels + 1.0)) return px.pixels > py.pixels;
    if (!qFuzzyCompare(px.sizeMB + 1.0, py.sizeMB + 1.0)) return px.sizeMB > py.sizeMB;
    return x.name.compare(y.name, Qt::CaseInsensitive) < 0;
}

} // namespace

ExactDupAction decideExactDupRestore(int deletedFromA, int deletedFromB, bool folderKeepIsA)
{
    if (deletedFromA > deletedFromB)
        return ExactDupAction::RestoreToSubset;   // A 被删得多 -> 保留 B
    if (deletedFromA < deletedFromB)
        return ExactDupAction::KeepCurrent;       // B 被删得多 -> 保留 A
    return folderKeepIsA ? ExactDupAction::KeepCurrent : ExactDupAction::RestoreToSubset;
}

QString formatDouble(double v, int decimals)
{
    return QString::number(v, 'f', decimals);
}

QString folderPairSortKey(const QString &dirA, const QString &dirB)
{
    auto norm = [](const QString &p) {
        return QDir::cleanPath(QDir::fromNativeSeparators(p)).toLower();
    };
    QString a = norm(dirA);
    QString b = norm(dirB);
    if (a > b)
        std::swap(a, b);
    if (a.isEmpty() && b.isEmpty())
        return QString();
    return a + QLatin1Char('\n') + b;
}

MatchStats matchForTest(const DirEntry &collection, const DirEntry &subset,
                        const QVector<ImageItem> &images, const Options &opt)
{
    return matchDirs(collection, subset, images, opt);
}

Comparison decide(const DirEntry &dirA, const DirEntry &dirB,
                  const QVector<ImageItem> &images, const Options &opt)
{
    Comparison c;
    c.bToA = matchDirs(dirA, dirB, images, opt);   // B 中能在 A 找到几张
    c.aToB = matchDirs(dirB, dirA, images, opt);   // A 中能在 B 找到几张

    const int nA = c.bToA.nA;
    const int nB = c.bToA.nB;
    if (nA <= 0 || nB <= 0) {
        c.decision = Decision::None;
        c.note = QStringLiteral("有一侧没有图片");
        return c;
    }

    QString whyBA, whyAB;
    const bool bInA = contained(c.bToA, opt, &whyBA);
    const bool aInB = contained(c.aToB, opt, &whyAB);

    /* ---- 自动方向 ---- */
    if (bInA || aInB) {
        const bool nearEqual = std::abs(nA - nB) <= 1;

        /*
         * 两边内容完全一致(数量相同且匹配到的全部二进制相同):这是"同一套作品的
         * 两份拷贝",按默认稳定顺序保留 A 即可,不必每次都要人工。
         * 提示词 §2.1 允许这种"先处理 B/C"的默认顺序。
         */
        if (nearEqual && nA == nB && bInA && aInB
            && c.aToB.identical >= nA && c.bToA.identical >= nB) {
            c.decision = Decision::KeepA;
            c.note = QStringLiteral("完全相同的两份拷贝(%1 张),默认保留 A").arg(nA);
            return c;
        }

        if (nearEqual) {
            /*
             * 数量相同/只差 1:按"候选合集嵌套层数更大"区分方向。
             * 两边深度也一样就交给人工 —— 这种情况通常就是两份一样的目录。
             */
            if (bInA && dirA.depth > dirB.depth) {
                c.decision = Decision::KeepA;
                c.note = QStringLiteral("数量接近,深度 %1 > %2").arg(dirA.depth).arg(dirB.depth);
            } else if (aInB && dirB.depth > dirA.depth) {
                c.decision = Decision::KeepB;
                c.note = QStringLiteral("数量接近,深度 %1 > %2").arg(dirB.depth).arg(dirA.depth);
            } else {
                c.decision = Decision::Manual;
                c.reason = ManualReason::EqualSizeSameDepth;
                /*
                 * 走到这里有两种真实情况,提示要能区分,不然用户看不懂为什么让他选:
                 *   a) 数量接近、深度也一样(两份几乎相同的目录);
                 *   b) 深度不一样,但只有一边满足包含关系 —— 提示词的规则是
                 *      "数量接近时用'候选合集更深'区分方向",这种单边情况不满足,
                 *      不能反过来按路径顺序猜一个方向。
                 */
                c.note = QStringLiteral("数量接近(%1/%2),深度 %3/%4 无法区分方向,需要人工")
                             .arg(nA).arg(nB).arg(dirA.depth).arg(dirB.depth);
                return c;
            }
        } else if (bInA && aInB) {
            // 双向都满足:多的那边当合集
            c.decision = (nA > nB) ? Decision::KeepA : Decision::KeepB;
            c.note = QStringLiteral("双向包含,按数量 %1/%2 定方向").arg(nA).arg(nB);
        } else if (bInA) {
            c.decision = Decision::KeepA;
            c.note = whyBA;
        } else {
            c.decision = Decision::KeepB;
            c.note = whyAB;
        }

        /*
         * 两侧都还有独有图片：谁也不是另一方的干净超集。
         * 相似率达标只证明"大部分重合"，不能据此把对方独有的作品判成多余内容 ——
         * 这种情况必须让人看一眼（提示词补充规则，阈值见 Options::bothUniqueMin）。
         */
        const int uniqA = nA - c.aToB.matched;
        const int uniqB = nB - c.bToA.matched;
        if (uniqA >= opt.bothUniqueMin && uniqB >= opt.bothUniqueMin) {
            c.decision = Decision::Manual;
            c.reason = ManualReason::BothSidesUnique;
            c.note = QStringLiteral("相似率达标,但两侧各有 %1/%2 张独有图片,方向需人工确认")
                         .arg(uniqA).arg(uniqB);
            return c;
        }

        /* 质量门槛:合集侧三项指标都要 >= 子集侧的 98% */
        const MatchStats &st = c.activeStats();
        if (!qualityOk(st, opt.qualityRatio)) {
            c.decision = Decision::Manual;
            c.reason = ManualReason::QualityBelow;
            c.note += QStringLiteral(" / 但 ") + qualityNote(st) + QStringLiteral(" 未达 98%");
            return c;
        }
        c.note += QStringLiteral(" / ") + qualityNote(st);
        return c;
    }

    /* ---- 不满足自动条件时,判断要不要进人工队列 ---- */
    const double rBA = c.bToA.ratio();
    const double rAB = c.aToB.ratio();
    const double maxRatio = std::max(rBA, rAB);
    const int maxIdentical = std::max(c.bToA.identical, c.aToB.identical);

    if (maxIdentical >= opt.manualIdenticalMin) {
        c.decision = Decision::Manual;
        c.reason = ManualReason::IdenticalDuplicates;
        c.note = QStringLiteral("完全重复 %1 张,但包含条件不足").arg(maxIdentical);
        return c;
    }
    if (maxRatio >= opt.manualRatio) {
        c.decision = Decision::Manual;
        c.reason = ManualReason::RatioInRange;
        c.note = QStringLiteral("相似率 %1% 落在人工区间")
                     .arg(QString::number(100.0 * maxRatio, 'f', 1));
        return c;
    }

    c.decision = Decision::None;
    c.note = QStringLiteral("相似率不足(%1%)")
                 .arg(QString::number(100.0 * maxRatio, 'f', 1));
    return c;
}

Analysis analyze(const QStringList &rootsIn, const QVector<ImageItem> &images, const Options &opt)
{
    Analysis out;
    out.images = images;

    /*
     * 根目录规范化 + 去重(大小写/分隔符/末尾斜杠都不该产生两个根)。
     * 比较用 lower,展示保留原样。
     */
    QStringList roots;
    QHash<QString, QString> rootByKey;   // lower(abs) -> 原样 abs
    for (const QString &r : rootsIn) {
        if (r.trimmed().isEmpty()) continue;
        const QString abs = QDir::cleanPath(QFileInfo(r.trimmed()).absoluteFilePath());
        const QString key = abs.toLower();
        if (rootByKey.contains(key)) continue;
        rootByKey.insert(key, abs);
        roots << abs;
    }
    out.rootDir = roots.value(0);

    if (roots.isEmpty())
        return out;

    QHash<QString, int> slotByDirKey;    // 顶层目录绝对路径(lower) -> dirs 下标
    QHash<int, QStringList> relByDir;    // dirs 下标 -> 相对路径(用于算深度)

    for (int i = 0; i < images.size(); ++i) {
        const QString abs = QDir::cleanPath(QFileInfo(images[i].path).absoluteFilePath());

        /*
         * 找出"包住这个文件"的根 —— 取**最长**的那个:
         * 多根目录时理论上不会互相包含(输入校验会拦住),但真出现了也按最具体的算,
         * 深度才是"相对它自己所属的根"。
         */
        QString ownerRoot;
        QString rel;
        for (auto it = rootByKey.constBegin(); it != rootByKey.constEnd(); ++it) {
            const QString &rootAbs = it.value();
            const QString tmpRel = QDir(rootAbs).relativeFilePath(abs);
            if (tmpRel.startsWith(QStringLiteral("..")) || QDir::isAbsolutePath(tmpRel))
                continue;   // 不在这个根下
            if (tmpRel.length() >= rel.length() && !ownerRoot.isEmpty())
                continue;   // 已有更长的相对路径 = 更具体的根
            ownerRoot = rootAbs;
            rel = tmpRel;
        }
        if (ownerRoot.isEmpty())
            continue;   // 不在任何根目录下:直接忽略

        const QStringList parts = rel.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (parts.size() < 2)
            continue;   // 直接放在根目录下的文件不属于任何顶层子目录

        const QString name = parts.first();
        const QString dirPath = QDir::cleanPath(ownerRoot + QLatin1Char('/') + name);
        const QString dirKey = dirPath.toLower();

        int slot;
        auto it = slotByDirKey.constFind(dirKey);
        if (it == slotByDirKey.constEnd()) {
            DirEntry d;
            d.path = dirPath;
            d.name = name;          // 只用于显示;跨根同名不代表同一个目录
            slot = out.dirs.size();
            out.dirs << d;
            slotByDirKey.insert(dirKey, slot);
        } else {
            slot = it.value();
        }

        out.dirs[slot].images << i;
        if (parts.size() == 2)
            out.dirs[slot].directImages << i;
        relByDir[slot] << rel;
    }

    /* 匹配必须与遍历顺序无关:每个目录内的图片按路径排序 */
    for (DirEntry &d : out.dirs) {
        std::sort(d.images.begin(), d.images.end(), [&images](int x, int y) {
            return images[x].path < images[y].path;
        });
        std::sort(d.directImages.begin(), d.directImages.end(), [&images](int x, int y) {
            return images[x].path < images[y].path;
        });
    }

    /* 深度 = 目录内最深图片的相对层级(直接放里面 = 1) */
    for (auto it = relByDir.constBegin(); it != relByDir.constEnd(); ++it) {
        int depth = 1;
        for (const QString &rel : it.value())
            depth = std::max(depth, int(rel.count(QLatin1Char('/'))));
        out.dirs[it.key()].depth = depth;
    }

    /* 预排序：把"更像合集"的那个放前面，界面与日志的顺序才不会反直觉 */
    QVector<DirProfile> profiles;
    profiles.reserve(out.dirs.size());
    for (const DirEntry &d : std::as_const(out.dirs))
        profiles << profileOf(d, images);

    for (int i = 0; i < out.dirs.size(); ++i) {
        for (int j = i + 1; j < out.dirs.size(); ++j) {
            int ia = i, ib = j;
            if (looksMoreLikeCollection(out.dirs[ib], profiles[ib],
                                        out.dirs[ia], profiles[ia])) {
                std::swap(ia, ib);
            }
            Comparison c = decide(out.dirs[ia], out.dirs[ib], images, opt);
            // 下标必须在这里补上:调用方靠 a/b 找目录,漏了就整对被跳过
            c.a = ia;
            c.b = ib;
            out.comparisons << c;
        }
    }

    return out;
}

QStringList previewPaths(const DirEntry &dir, const Analysis &analysis)
{
    QStringList all;
    all.reserve(dir.images.size());
    for (int idx : dir.images)
        all << analysis.images[idx].path;
    all.sort(Qt::CaseInsensitive);

    QStringList out;
    const int n = all.size();
    if (n == 0)
        return out;
    if (n <= 4) {
        out = all;
        return out;
    }

    // 第一张 / 中间两张 / 最后一张
    out << all.at(0)
        << all.at(n / 2 - 1)
        << all.at(n / 2)
        << all.at(n - 1);
    return out;
}

CompareView makeView(const Comparison &cmp, const DirEntry &dirA, const DirEntry &dirB,
                     const Analysis &analysis)
{
    CompareView v;
    v.dirA = dirA.path;
    v.dirB = dirB.path;
    v.countA = dirA.count();
    v.countB = dirB.count();
    v.note = cmp.note;

    switch (cmp.decision) {
    case Decision::KeepA: v.keepHint = QStringLiteral("A"); break;
    case Decision::KeepB: v.keepHint = QStringLiteral("B"); break;
    default: break;
    }

    /*
     * MatchStats 的 A/B 是"合集侧/子集侧"口径,不是"目录 A / 目录 B"。
     * 合集是 B(KeepB)时 activeStats() 落在 aToB 上,那个结构里的 avg*A* 存的是
     * 目录 B 的数据 —— 直接照抄会把界面上 A、B 两列的均值/大小/像素整列写反
     * (连"谁更绿"都会跟着反)。这里显式映射一次。
     */
    const MatchStats &st = cmp.activeStats();
    const bool sideSwapped = (cmp.decision == Decision::KeepB);
    v.qualityA = sideSwapped ? st.avgQualityB : st.avgQualityA;
    v.qualityB = sideSwapped ? st.avgQualityA : st.avgQualityB;
    v.sizeA    = sideSwapped ? st.avgSizeB    : st.avgSizeA;
    v.sizeB    = sideSwapped ? st.avgSizeA    : st.avgSizeB;
    v.pixelsA  = sideSwapped ? st.avgPixelsB  : st.avgPixelsA;
    v.pixelsB  = sideSwapped ? st.avgPixelsA  : st.avgPixelsB;
    v.widthA   = sideSwapped ? st.avgWidthB   : st.avgWidthA;
    v.widthB   = sideSwapped ? st.avgWidthA   : st.avgWidthB;
    v.heightA  = sideSwapped ? st.avgHeightB  : st.avgHeightA;
    v.heightB  = sideSwapped ? st.avgHeightA  : st.avgHeightB;
    /*
     * lb_duplicate_count 的口径:当前方向下"被判定相似/重复的图片对数"
     * (matched 是子集侧匹配到的图片数,一对一匹配所以两者相等)。
     * 这两项与方向无关:A 列永远是"目录 A 里被命中几张",B 列同理。
     */
    v.duplicateA = cmp.aToB.matched;   // A 侧被 B 匹配到的图片数
    v.duplicateB = cmp.bToA.matched;   // B 侧被 A 匹配到的图片数
    /* 一次都没匹配上时,质量/大小/分辨率没有统计意义 -> 界面显示 N/A */
    v.hasMatch = (cmp.bToA.matched > 0 || cmp.aToB.matched > 0);

    v.previewA = previewPaths(dirA, analysis);
    v.previewB = previewPaths(dirB, analysis);
    return v;
}

} // namespace foldercompare
