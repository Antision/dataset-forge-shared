#ifndef FOLDERCOMPARE_H
#define FOLDERCOMPARE_H

/*
 * ============================================================================
 *  半选 internal(cb_internal_x 半勾选)的"合集优先"目录级判定
 * ============================================================================
 *
 * 纯逻辑,不读盘、不依赖 OpenCV:调用方把已经算好的图片元信息(pHash/dHash/
 * SHA256/质量/尺寸)传进来,本模块只负责
 *   1) 按顶层子目录分组(递归图片集合 + 直属图片集合);
 *   2) 两两比较,判断"谁包含谁"(合集/子集);
 *   3) 给出 KeepA / KeepB / 需要人工(FolderCompareWidget)/ 无关。
 *
 * 相似判定沿用项目现有阈值语义:pHair/dHash 汉明距离、ORB 几何校验。
 * ORB 描述子是 cv::Mat,本模块不碰它 —— 由调用方提供 orbMatch 回调。
 */
#include <QByteArray>
#include <QHash>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace foldercompare {

/*
 * 一次 ORB 几何校验的结论(纯 POD)。
 * 刻意不用 cv:: 类型:本模块是纯逻辑、可以脱离 OpenCV 单测。
 */
struct OrbMatch {
    int    inliers     = 0;    // RANSAC 内点数
    double inlierRatio = 0.0;  // 内点 / Lowe 命中
};

/* 一张图片的判定所需信息(全部来自扫描结果,不需要再读盘) */
struct ImageItem {
    QString    path;
    int        quality = 0;       // 压缩质量估计(0=未知)
    qint64     filesize = 0;      // 字节
    int        width = 0;
    int        height = 0;
    quint64    pHash = 0;
    quint64    dHash = 0;
    QByteArray sha;               // 32 字节二进制哈希;空=未知

    qint64 pixels() const { return qint64(width) * height; }
};

struct Options {
    /* 相似判定(与去重窗口保持一致的默认值) */
    int    pHashThreshold = 10;
    int    dHashThreshold = 10;
    /*
     * ORB 几何校验的两个门槛(与 ImageDeduplicateWindow::m_orbMinInliers /
     * m_orbMinInlierRatio 一致)。判定条件是"与":
     *   内点数 >= orbMinInliers 且 内点/命中 >= orbMinRatio
     */
    int    orbMinInliers = 30;
    double orbMinRatio   = 0.55;

    /*
     * ORB 几何校验回调:(itemA 下标, itemB 下标) -> 校验结果。
     * 为空表示不参与判定(只用 pHash/dHash/SHA256)。便宜预筛之后才会调用。
     */
    std::function<OrbMatch(int, int)> orbMatch;

    /* 合集判定 */
    int    smallDirMax        = 3;      // "少于 4 张"的小目录
    double containRatio       = 0.80;   // 大目录要求的最低相似率
    double manualRatio        = 0.50;   // 达到这个比例就该让人工看一眼
    int    manualIdenticalMin = 2;      // 完全重复达到这个数就该人工看
    double qualityRatio       = 0.98;   // 合集侧三项指标的最低比例
    /*
     * 两侧"各有独有图片"的门槛（提示词补充规则）：
     * 相似率再高，只要两边都还有 >= 这么多张对不上，就说明谁也不是另一方的干净超集，
     * 方向必须让人来定 —— 自动判成"子集"会把对方独有的作品当成多余内容。
     */
    int    bothUniqueMin      = 2;
};

/* 一个顶层子目录 */
struct DirEntry {
    QString      path;          // 绝对路径
    QString      name;          // 目录名
    int          depth = 1;     // 目录内最大相对层级(文件直接放里面=1, a/b/x=3)
    QVector<int> images;        // 递归图片(全部后代)
    QVector<int> directImages;  // 直属图片(只在本层)

    int count() const { return images.size(); }
};

/*
 * 一次方向判定的匹配统计。
 *
 * !! 这里的 A/B 不是"目录 A / 目录 B",而是"被匹配进的侧 / 被匹配的侧":
 *      matchDirs(to, from)  ->  A = to(本次的候选合集侧), B = from(子集侧)
 *    bToA = matchDirs(dirA, dirB)  -> A=dirA 侧,B=dirB 侧
 *    aToB = matchDirs(dirB, dirA)  -> A=dirB 侧,B=dirA 侧   <-- 方向是反的
 *    界面要按"目录 A / 目录 B"显示时必须做一次映射,见 makeView()。
 */
struct MatchStats {
    int    nA = 0;              // A 侧(合集侧)递归图片数
    int    nB = 0;              // B 侧(子集侧)递归图片数
    int    matched = 0;         // B 中能在 A 里找到相似对应的图片数
    int    identical = 0;       // 其中二进制完全相同的数量

    double avgQualityA = 0.0;   // 仅统计 matched 的 A 侧
    double avgQualityB = 0.0;
    double avgSizeA = 0.0;      // MB
    double avgSizeB = 0.0;
    double avgPixelsA = 0.0;    // 宽*高
    double avgPixelsB = 0.0;
    double avgWidthA = 0.0, avgWidthB = 0.0;    // 仅用于显示 "1024x768"
    double avgHeightA = 0.0, avgHeightB = 0.0;

    double ratio() const { return nB > 0 ? double(matched) / double(nB) : 0.0; }
};

enum class Decision {
    None,     // 没有合集关系
    KeepA,    // A 是合集,应保留 A
    KeepB,    // B 是合集
    Manual    // 交给 FolderCompareWidget
};

enum class ManualReason {
    None,
    RatioInRange,        // 相似率落在 [50%, 自动阈值) 之间
    IdenticalDuplicates, // 有 >=2 张完全重复但包含条件不足
    QualityBelow,        // 满足包含条件但质量指标不到 98%
    EqualSizeSameDepth,  // 数量相同/差 1 且深度也分不出来
    BothSidesUnique,     // 相似率够高,但两边都还剩 >=2 张独有的图片(谁也不是干净超集)
    BothDirections       // 双向都满足包含(互为子集),人工定
};

struct Comparison {
    int          a = -1;
    int          b = -1;
    Decision     decision = Decision::None;
    ManualReason reason = ManualReason::None;

    /* 方向统计:A 为候选合集时看 bToA;B 为候选合集时看 aToB */
    MatchStats bToA;
    MatchStats aToB;

    /* 判定依据(日志/测试/界面提示) */
    QString note;

    bool needsManual() const { return decision == Decision::Manual; }
    /*
     * 判定口径下的统计(A=合集侧,B=子集侧)。只给"合集侧是否各项都不弱于子集侧"
     * 这类方向无关的判断用;要按"目录 A / 目录 B"展示请走 makeView()。
     */
    const MatchStats &activeStats() const { return decision == Decision::KeepB ? aToB : bToA; }
};

struct Analysis {
    QString              rootDir;
    QVector<ImageItem>   images;      // 与 Analysis 的 dirs[].images 下标对应
    QVector<DirEntry>    dirs;
    /* 所有 dirs 的两两组合;每条的 a/b 已按"更像合集者在前"排过序,不再是简单的 i<j */
    QVector<Comparison>  comparisons;
};

/*
 * 分析若干个根目录的直接子目录(不把更深层目录当成独立作品)。
 *
 * 多根目录 = 一次会话把它们放在**同一个池子**里处理:
 *   · 分组键是"属于哪个根 + 该根下的第一层子目录",所以 E:/A/sub1 与 E:/B/sub1
 *     是两个不同的组(名字相同也不会被并到一起);
 *   · 组间两两比较**跨根也做**(A/sub1 ↔ B/sub1 同样会出结论),
 *     这正是"输入多个根目录,统一处理"的含义;
 *   · 深度(depth)按该文件相对**它自己的根**的层级算,不是相对某个公共前缀。
 *
 * images 里的路径必须落在 roots 之内,顺序无所谓。
 */
Analysis analyze(const QStringList &roots,
                 const QVector<ImageItem> &images,
                 const Options &opt);

/* 单根目录的便捷重载(老调用点与测试继续可用) */
inline Analysis analyze(const QString &rootDir,
                        const QVector<ImageItem> &images,
                        const Options &opt)
{
    return analyze(QStringList{rootDir}, images, opt);
}

/* 单对判定:dirA/dirB 里的下标都指向 images */
Comparison decide(const DirEntry &dirA, const DirEntry &dirB,
                  const QVector<ImageItem> &images, const Options &opt);

/* ---------------- 界面视图(FolderCompareWidget 用) ---------------- */
struct CompareView {
    QString dirA;
    QString dirB;
    QString keepHint;              // "A"/"B"/""(没有自动建议)

    int countA = 0;                // 递归图片数
    int countB = 0;

    double qualityA = 0.0, qualityB = 0.0;   // 相似匹配图片的平均质量
    double sizeA = 0.0, sizeB = 0.0;         // MB
    double pixelsA = 0.0, pixelsB = 0.0;     // 平均像素总数(比较用)
    double widthA = 0.0, heightA = 0.0;      // 平均宽高(显示用)
    double widthB = 0.0, heightB = 0.0;
    /*
     * lb_duplicate_count 的口径 = 各侧"被相似命中"的图片数(不是对数,
     * 也不是"可被覆盖的文件数")。两侧一对一匹配,所以通常相等;
     * 它是计数而不是优劣指标,界面不得把它当成"更多更好"。
     */
    int duplicateA = 0;                      // A 侧被匹配到的相似图片数
    int duplicateB = 0;                      // B 侧被匹配到的相似图片数
    /* 有没有匹配到任何相似图片:false 时质量/大小/分辨率显示 N/A 而不是 0 */
    bool hasMatch = false;

    QString note;                  // 判定依据
    QStringList previewA;          // 缩略图路径(最多 4 张)
    QStringList previewB;
};

/* 首张 / 中间两张 / 末张(按路径排序),不足 4 张时按实际数量 */
QStringList previewPaths(const DirEntry &dir, const Analysis &analysis);

CompareView makeView(const Comparison &cmp, const DirEntry &dirA, const DirEntry &dirB,
                     const Analysis &analysis);

/*
 * 单次贪心匹配(公开给测试用):在 collection 里为 subset 的每张图找一张
 * 还没被占用的最相似图片。
 */
MatchStats matchForTest(const DirEntry &collection, const DirEntry &subset,
                        const QVector<ImageItem> &images, const Options &opt);

/*
 * 完全重复图片的最终归属(提示词 4.2)。
 *   deletedFromA / deletedFromB:本次因为去重从两侧移走的图片数
 *   folderKeepIsA:目录级合集判定是否"应保留 A"
 * 规则:移出少的一侧优先保留;相等时按合集判定结果。
 * 返回 RestoreToSubset 表示要把之前从子集侧移走的完全重复文件恢复回去。
 */
enum class ExactDupAction { KeepCurrent, RestoreToSubset };
ExactDupAction decideExactDupRestore(int deletedFromA, int deletedFromB, bool folderKeepIsA);

/* 四舍五入到指定小数位(界面/测试共用,保证显示口径一致) */
QString formatDouble(double v, int decimals);

/*
 * 半选 internal 去重时"目录对分组"的排序键。
 *
 * 用途:ImageDeduplicateWindow 的图片对列表按"这一对文件分别属于哪两个顶层目录"
 * 分组,让用户能把 A 与 B 之间的相似图集中处理完,再去处理 A 与 C 的 ——
 * 而不是在同一次浏览里被三个目录的图片来回打断。
 *
 * 规则:
 *   - 两个目录都做 cleanPath + 小写(Windows 大小写不敏感,不能因为 C:\A 与 c:\a 分成两组);
 *   - 两者按字典序排序后再拼接,所以 A↔B 与 B↔A 得到同一个键(方向由摆位决定,不能影响分组);
 *   - 用 '\n' 分隔:它比任何可打印字符都小,保证 "a" < "ab" 这种前缀关系仍然是字典序。
 *
 * 任一目录为空(文件直接放在输入根目录下、不属于任何顶层子目录)时返回空串,
 * 调用方应当把空串排在最前 —— 这些文件没有"目录对"归属。
 */
QString folderPairSortKey(const QString &dirA, const QString &dirB);

} // namespace foldercompare

#endif // FOLDERCOMPARE_H
