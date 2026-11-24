#ifndef DEDUPWORKER_H
#define DEDUPWORKER_H

#include "deduppch.h"
#include <QElapsedTimer>
#include <functional>
#include "deduputils.h"

/*
 * 单个输入目录的"内部去重"模式(来自 MainWindow 里每个目录各自的 cb_internal_x):
 *   None          不内部去重:同一个输入目录内的文件之间不比较(目录之间照常比较)
 *   AcrossTopDirs 该目录下各【顶层子目录】互相去重,但单个顶层子目录内部不去重
 *                 (cb_internal_x 半勾选)。直接放在该目录下的文件算作一个独立单元。
 *   All           该目录内所有文件互相去重(全勾选)
 * 整数取值要和对勾状态一致:0=Unchecked, 1=PartiallyChecked, 2=Checked
 */
enum class InternalDedup : int {
    None         = 0,
    AcrossTopDirs = 1,
    All          = 2
};

class FileEntry {
public:
    FileEntry() = default;

    QString originalPath;   // initial scanned path
    QString currentPath;    // may change when moved to duplicate (always use currentPath for loading)
    QString rootInputFolder; // NEW: which top-level input folder this file belongs to (one of the `folders` passed to load)
    QString folder;         // folder containing the file (original folder)
    int folderPriority = 0; // smaller = higher priority (folders earlier in input)
    QString filename;
    qint64 filesize = 0;
    qint64 modTime = 0;     // 文件最后修改时间(ms since epoch)，用于判断缓存是否还有效
    QSize size{0,0};
    QString format;
    int quality=0;

    uint64_t pHash = 0;
    uint64_t dHash = 0;
    bool inDuplicate = false;

    /*
     * 该文件所属输入目录的内部去重模式(InternalDedup 的整数值)。
     * 只在本次运行内有效,不写进 tscch —— 从缓存复用时由扫描选项重新赋值。
     */
    int internalMode = static_cast<int>(InternalDedup::All);

    /*
     * clear file 的 A/B 组标记:-1 = 不分组(普通去重/导入/remove cover 全是这个),
     * 0 = A 组(压缩包解压出来的内容),1 = B 组(散图)。
     *
     * 只有"两侧都是同一个非负值"的文件之间才禁止建对 —— 也就是组内不比较,
     * 只有 A↔B 才比较。用 rootInputFolder 做不到这件事:多个压缩包解压出多个
     * 根目录,它们之间是不同的 rootInputFolder,默认会互相比较。
     */
    int compareSide = -1;

    QImage thumbnail;     // 缩略图，用于右侧列表快速加载(不写入tscch,运行时生成)
    char SHA256[32]{};    // 图片文件的SHA256,用于image_features.tscch中确认图片

    /*
     * 查重时使用:对该图片计算出的各项距离/相似度指标。
     * 键:"pHash" -> pHash距离, "dHash" -> dHash距离,
     *     "orb"   -> ORB相似度(0..1,越大越相似), "identical" -> 1.0 表示SHA256完全相同。
     * 不参与tscch序列化。
     */
    QHash<QString, double> compare;

    /* 写入某项指标 */
    void setMetric(const QString &key, double v) { compare.insert(key, v); }
    /* 读取某项指标(缺省时返回def) */
    double metric(const QString &key, double def = 0.0) const {
        return compare.value(key, def);
    }
    int pHashDistance() const { return static_cast<int>(metric(QStringLiteral("pHash"), -1.0)); }
    int dHashDistance() const { return static_cast<int>(metric(QStringLiteral("dHash"), -1.0)); }
    double orbSimilarity() const { return metric(QStringLiteral("orb"), 0.0); }
    bool identicalContent() const { return metric(QStringLiteral("identical"), 0.0) > 0.5; }

    /* pHash/dHash 是否已经计算过(全0视为没有) */
    bool hasHashes() const { return pHash != 0 || dHash != 0; }

    /* 通过SHA-256确认两个FileEntry指向的是否是同一个文件内容 */
    bool sameContentAs(const FileEntry &other) const {
        return std::memcmp(SHA256, other.SHA256, sizeof(SHA256)) == 0;
    }

    std::mutex matMutex;
    std::atomic_bool isMatLoaded=false;
    cv::Mat m_mat;//完整图片矩阵

    cv::Mat orbDescriptors;
    /*
     * 关键点坐标(Nx2 CV_32F),与 orbDescriptors 的行一一对应。
     * 没有坐标就没法做几何校验 —— 这正是旧版 ORB 匹配率不可靠的根因,
     * 所以它跟描述子一起进 tscch。
     */
    cv::Mat orbPoints;
    int orbKeypoints = 0;

    bool loadMat(){
        if(isMatLoaded) return false;

        m_mat = dedup::loadMat(dedup::readDataFromPath(currentPath));
        if(m_mat.empty()) {
            /*
             * 读不到文件是【正常情况】,绝不能抛异常 ——
             * auto remove / 导入整理会把文件搬进 duplicate/,而预取任务可能还拿着
             * 旧路径来加载;文件被其它程序占用、损坏、正在删除也一样。
             *
             * 这个函数是在 QThreadPool 的 QRunnable::run() 里被调用的:
             * 异常一旦逃出 run(),Qt 会 catch(...) 后调 qTerminate() ->
             * std::terminate -> abort()。实测就是这里:auto remove 之后两个预取
             * 线程同时按旧路径读不到文件,各弹一个 "abort() has been called"。
             *
             * 返回 false 让 ensureMatCached 干净地失败,下次显示时自然会重试。
             */
            qDebug() << "[FileEntry::loadMat] 读不到图片(可能已被移动/占用):" << currentPath;
            return false;
        }
        isMatLoaded = true;
        return true;
    }
    bool releaseMat() {
        if(!isMatLoaded) return false;
        isMatLoaded=false;
        m_mat.release();
        return true;
    }

    // 添加小于运算符
    bool operator<(const FileEntry& other) const {
        // 使用 originalPath 作为唯一标识符进行比较
        return originalPath < other.originalPath;
    }

    FileEntry(const FileEntry& other)
        : originalPath(other.originalPath),
        currentPath(other.currentPath),
        rootInputFolder(other.rootInputFolder),
        folder(other.folder),
        folderPriority(other.folderPriority),
        filename(other.filename),
        filesize(other.filesize),
        modTime(other.modTime),                 // 必须拷贝,否则缓存复用判定永远失效
        size(other.size),
        format(other.format),
        quality(other.quality),
        pHash(other.pHash),
        dHash(other.dHash),
        inDuplicate(other.inDuplicate),
        internalMode(other.internalMode),
        compareSide(other.compareSide),
        thumbnail(other.thumbnail),
        compare(other.compare),                 // 必须拷贝,否则查重指标会丢
        isMatLoaded(other.isMatLoaded.load()),
        m_mat(other.m_mat.clone()),             // cv::Mat 深拷贝
        orbDescriptors(other.orbDescriptors.clone()), // cv::Mat 深拷贝
        orbPoints(other.orbPoints.clone()),           // cv::Mat 深拷贝
        orbKeypoints(other.orbKeypoints)
    {
        std::memcpy(SHA256, other.SHA256, sizeof(SHA256));
    }

    FileEntry& operator=(const FileEntry& other) {
        if (this == &other) {
            return *this;
        }

        // 1. 释放当前资源
        releaseMat();
        orbDescriptors.release();
        orbPoints.release();

        // 2. 拷贝普通成员
        originalPath = other.originalPath;
        currentPath = other.currentPath;
        rootInputFolder = other.rootInputFolder;
        folder = other.folder;
        folderPriority = other.folderPriority;
        filename = other.filename;
        filesize = other.filesize;
        modTime = other.modTime;
        size = other.size;
        format = other.format;
        quality = other.quality;
        pHash = other.pHash;
        dHash = other.dHash;
        inDuplicate = other.inDuplicate;
        internalMode = other.internalMode;
        compareSide = other.compareSide;
        thumbnail = other.thumbnail;
        compare = other.compare;
        std::memcpy(SHA256, other.SHA256, sizeof(SHA256));

        // 3. mutex 不拷贝，保持当前对象的 matMutex 不变
        isMatLoaded.store(other.isMatLoaded.load());

        // 4. 深拷贝 cv::Mat
        m_mat = other.m_mat.clone();
        orbDescriptors = other.orbDescriptors.clone();
        orbPoints = other.orbPoints.clone();
        orbKeypoints = other.orbKeypoints;

        return *this;
    }

    FileEntry(FileEntry&& other) noexcept
        : originalPath(std::move(other.originalPath)),
        currentPath(std::move(other.currentPath)),
        rootInputFolder(std::move(other.rootInputFolder)),
        folder(std::move(other.folder)),
        folderPriority(other.folderPriority),
        filename(std::move(other.filename)),
        filesize(other.filesize),
        modTime(other.modTime),
        size(other.size),
        format(std::move(other.format)),
        quality(other.quality),
        pHash(other.pHash),
        dHash(other.dHash),
        inDuplicate(other.inDuplicate),
        internalMode(other.internalMode),
        compareSide(other.compareSide),
        thumbnail(std::move(other.thumbnail)),
        compare(std::move(other.compare)),
        // ⚠️ atomic_bool 需要拷贝其值
        isMatLoaded(other.isMatLoaded.load()),
        m_mat(std::move(other.m_mat)),  // cv::Mat 支持移动
        orbDescriptors(std::move(other.orbDescriptors)),
        orbPoints(std::move(other.orbPoints)),
        orbKeypoints(other.orbKeypoints)
    {
        std::memcpy(SHA256, other.SHA256, sizeof(SHA256));
        // 可选：清空源对象的状态，防止双重释放
        other.isMatLoaded.store(false);
        other.m_mat.release();
        other.orbDescriptors.release();
        other.orbPoints.release();
    }

    FileEntry& operator=(FileEntry&& other) noexcept {
        if (this != &other) {
            // 1. 释放当前资源
            releaseMat();
            orbDescriptors.release();
            orbPoints.release();

            // 2. 移动基本类型
            originalPath = std::move(other.originalPath);
            currentPath = std::move(other.currentPath);
            rootInputFolder = std::move(other.rootInputFolder);
            folder = std::move(other.folder);
            folderPriority = other.folderPriority;
            filename = std::move(other.filename);
            filesize = other.filesize;
            modTime = other.modTime;
            size = other.size;
            format = std::move(other.format);
            quality = other.quality;
            pHash = other.pHash;
            dHash = other.dHash;
            inDuplicate = other.inDuplicate;
            internalMode = other.internalMode;
            compareSide = other.compareSide;
            thumbnail = std::move(other.thumbnail);
            compare = std::move(other.compare);
            std::memcpy(SHA256, other.SHA256, sizeof(SHA256));
            // 3. 处理特殊类型
            // ⚠️ mutex 不能移动，保持原样
            isMatLoaded.store(other.isMatLoaded.load());

            // 4. 移动 cv::Mat（高效！）
            m_mat = std::move(other.m_mat);

            // 5. 移动 ORB 描述符
            orbDescriptors = std::move(other.orbDescriptors);
            orbPoints = std::move(other.orbPoints);
            orbKeypoints = other.orbKeypoints;

            // 6. 清理源对象
            other.isMatLoaded.store(false);
            other.m_mat.release();
            other.orbDescriptors.release();
            other.orbPoints.release();
            other.orbKeypoints = 0;
        }
        return *this;
    }
};


Q_DECLARE_METATYPE(FileEntry)
Q_DECLARE_METATYPE(QVector<FileEntry>)

/*
 * image_features.tscch 的读写接口。
 *
 * 声明放在这里(而不是只放 cacheio.h)是因为 cacheio 要序列化 FileEntry,
 * 而 scanFolders 又要调用 cacheio —— 放一起就不存在头文件循环依赖。
 * 文件格式的完整说明写在 cacheio.h 里。
 */
namespace cacheio {

inline const QString kCacheFileName = QStringLiteral("image_features.tscch");

/* 某个目录下的缓存文件完整路径 */
QString cacheFilePathForFolder(const QString &folder);

/* 读 tscch 时一并带出来的元信息 */
struct CacheMeta {
    /* 上一次完整扫描的时间(0 = 未知) */
    quint64 savedAt = 0;
    /* 顶层子目录名 -> 上次更新时该子目录的 mtime;空串代表"直接位于本目录下的文件" */
    QHash<QString, quint64> subDirUpdatedAt;
    /* 文件是否存在(用于区分"空缓存"和"没有缓存") */
    bool exists = false;
    /*
     * 头部是否成功解析并通过校验。
     * exists && !readable 表示"文件在、但读不出来"(被占用/损坏/版本不认识)——
     * 这种情况下【绝对不允许】写回,否则会拿"只有本次记录"的内容把原文件覆盖掉,
     * 把别人的记录整片抹掉。
     */
    bool readable = false;
};

/*
 * 写回策略。
 * 默认(mergeOnly=false)是"权威写入":这个 tscch 作用域内的记录以本次结果为准,
 * 磁盘上已经不存在的文件会被剔除,savedAt 更新为现在。
 * mergeOnly=true 是"只合并不清理"(按正则筛选的扫描 / 只覆盖了作用域一部分的扫描)。
 */
struct WriteOptions {
    bool    mergeOnly = false;
    /* 只更新这个顶层子目录的时间戳(空 = 不动时间戳表) */
    QString touchSubDir;
    quint64 touchSubDirMtime = 0;
    /*
     * mergeOnly 时的"局部清理范围":只在这个目录子树内做"文件已经不在磁盘上就剔除"。
     * 空 = 完全不动别人的记录(按正则筛选扫描时就是这个行为)。
     * 扫描 ./b/ 时回写上一层的 tscch 会把它设成 ./b/ ——
     * 这样 b 下面真的被删掉的记录会被清掉,而 b 之外的记录原封不动。
     */
    QString pruneWithin;
    /*
     * 必须从缓存里删掉的路径(绝对路径,内部会再 cleanPath 一遍)。
     *
     * 和 pruneWithin 的区别:prune 靠"文件已不存在"做判断,只在确认扫过整棵子树时
     * 才敢用;这里由调用方【亲眼确认】过这些路径已经不在了(见 scanFolders 的移位识别),
     * 所以不管 mergeOnly 与否都直接删。
     *
     * 典型场景:文件被移动到别处,它的旧记录被新位置"认领"了,旧路径这条必须消失,
     * 否则同一个文件会在缓存里留下一条指向不存在路径的僵尸记录,
     * 下次扫描又会被当成"丢失"再认领一次,越滚越多。
     */
    QSet<QString> dropPaths;
};

/* ===================== 扫描中途的部分结果(.part) ===================== */

/*
 * 扫描时"边算边存"的追加日志 —— 【只用于防崩溃的检查点】。
 *
 * 【存在哪儿】临时目录,不是被扫描的目录 ——
 *   用户在设置窗口里指定的 temp 路径下的 scan/ 子目录
 *   (通常把 temp 设到内存盘,所以这笔写入不打数据盘);
 *   没设置或不可写时退回 <程序目录>/cache/scan。
 *
 * 【它不是最终产物】
 *   · 正常跑完 -> 权威写入目标 tscch,然后删掉 .part;
 *   · 中途取消 / 关窗 -> 把 .part 里的记录 mergeOnly 写进**目标目录**的 tscch,
 *     再删掉 .part(用户要求:取消时算好的特征要进目标缓存,不能只留在 temp);
 *   · 进程崩溃 / 断电 -> .part 留在 temp,下次扫描同一目录时开头自动并回。
 *   画廊目录里只会出现最终的 image_features.tscch(外加未完成时的那份 JSON 在 temp)。
 *
 * 文件名:part-<目标 tscch 绝对路径的 SHA-1>.part
 *   (所有 .part 都堆在同一个 temp 目录里,必须靠名字区分;
 *    文件头里另外记着 rootFolder / tscchPath,读回来时能核对是不是同一份。)
 *
 * 【为什么是追加而不是每 128 张重写 tscch】
 *   tscch 是整份重写的(QSaveFile),一条带 ORB 描述子的记录就有 16KB 左右,
 *   一万张图 = 一份 160MB 的文件。每 128 张重写一次 -> 总共要写十几 GB,
 *   而且越到后面越慢(O(n²))。追加日志是 O(1) 的:算完一条写一条,
 *   每 128 条 flush 一次,崩溃最多丢 128 条。
 *
 * 文件格式(QDataStream,Qt_6_0):
 *   quint32  magic    'TSP1'
 *   quint16  version  2
 *   QString  rootFolder   相对路径的基准目录(读回来时用来还原绝对路径)
 *   QString  tscchPath    这份部分结果最终要写进哪个 tscch(核对用)
 *   之后重复:
 *     quint32 payloadBytes
 *     raw payload        内容与 tscch 的一条记录完全一致(相对路径 + 字段 + 描述子)
 *
 * 崩溃时最后一条可能是半截的 —— 读取端遇到"长度读不全 / payload 不够"就停下,
 * 前面的记录照样能用(不做 CRC,半截记录只会被丢弃,不会污染别的记录)。
 */
class PartialCacheLog
{
public:
    ~PartialCacheLog() { close(); }

    /* 打开(已存在会被截断:残留的那份由 mergePartialCacheLog 先并走) */
    bool open(const QString &tscchPath, const QString &rootFolder,
              const QString &partPath, QString *outError = nullptr);
    /* 追加一条;"成功返回"只代表写进了 QFile 的缓冲,不代表已经落盘(见 flush) */
    bool append(const FileEntry &fe);
    bool flush();
    void close();
    bool isOpen() const { return m_open; }

private:
    QFile   m_file;
    QString m_root;
    bool    m_open = false;
};

/* ---- 临时缓存目录(由主窗口用设置里的 temp 路径设置) ---- */

/*
 * 设置"部分结果"的存放根目录。传的是用户设置的 temp 路径(不是扫描目录):
 * 内部会取它的 scan/ 子目录;不可写时自动退回 <程序目录>/cache/scan。
 * 传空串 = 用默认位置。
 */
void    setWorkCacheDir(const QString &preferredTempDir);
QString workCacheDir();

/* 目标 tscch -> temp 里的 .part 路径(<workCacheDir>/part-<sha1>.part) */
QString partialCachePathFor(const QString &tscchPath);

/*
 * 读回 .part 里的全部记录(遇到截断就停)。
 * expectTscch 非空时核对文件头里记的"目标 tscch":对不上说明这个 .part
 * 不是给这份缓存准备的(比如路径大小写/短路径差异),直接当没读到,绝不并错地方。
 */
QVector<FileEntry> readPartialCacheLog(const QString &partPath, QString *outError = nullptr,
                                       const QString &expectTscch = QString());

/*
 * 把残留的 .part 合并进同目录的 tscch(只合并、不剔除、不动 savedAt),
 * 成功后删掉 .part。返回写入条数;没有 .part 返回 0;失败返回 -1(.part 保留)。
 */
int mergePartialCacheLog(const QString &tscchPath, QString *outError = nullptr);

/* 只读 .part 的文件头,取出它记录的目标 tscch(读不出来返回空串) */
QString partialCacheTarget(const QString &partPath);

/*
 * 清理"没有 JSON 标记指向"的孤儿 .part(比如崩溃发生在第一次 flush 之前):
 *   · 文件头里记着目标 tscch、且那个目录还在 -> 直接并回目标缓存再删;
 *   · 否则(读不出来 / 目录已经没了)-> 删掉。
 * referenced = 本次保留的 .part 列表(有标记指向的不能动)。
 * 返回处理掉的个数。
 */
int sweepOrphanPartialCaches(const QStringList &referenced);

/* ===================== "上次扫描没做完"标记 ===================== */

/*
 * 只保留【一份】,记录最近一次没有正常结束的扫描(进程崩溃 / 断电)——
 * 用来说明 temp 里那堆 .part 是谁的特征:哪台目录、写进哪个 tscch、算到第几张。
 *
 * 注意:它【不会】在启动时弹窗问"要不要继续"。用户主动取消 = 不想再算,
 * 那时标记会被立刻清掉(见 scanFolders 的取消分支);
 * 只有真的崩溃才会残留,而残留的 .part 会在下次扫到同一目录时自动并回缓存,
 * 不需要用户做任何决定。
 */
struct ScanJournal {
    bool      exists = false;
    QString   folder;          // 扫描的主目录
    QString   target;          // 最终要写进的 tscch(主槽)
    QString   mode;            // "cache"(缓存按钮) / "dedupe"(去重窗口)
    qint64    startedAt = 0;
    qint64    updatedAt = 0;
    int       done = 0;
    int       total = 0;
    QStringList parts;         // 本次已经写出内容的 .part(都在 temp 目录里)
};

QString     scanJournalPath();
ScanJournal readScanJournal();
bool        writeScanJournal(const QString &folder, const QString &target,
                             const QString &mode, int done, int total,
                             const QStringList &parts);
void        clearScanJournal();

/* 读取tscch;相对路径会被还原成绝对路径。不读缩略图、不算SHA-256。 */
QVector<FileEntry> loadFileEntryCache(const QString &tscchPath,
                                      QString *outError = nullptr,
                                      CacheMeta *outMeta = nullptr);

/* 把entries合并写入tscch(目录不存在会自动创建)。返回写入条数,-1表示失败。 */
int writeFileEntryCache(const QString &tscchPath,
                        const QVector<FileEntry> &entries,
                        const WriteOptions &options = WriteOptions(),
                        QString *outError = nullptr);

} // namespace cacheio

/*
 * 一条缓存记录能不能直接拿来复用。必须满足:
 *   - SHA256 不能是全0:全0 说明当初没算或算失败,拿它做"内容没变"的判断是错的,
 *     会造成"用着旧特征却盖上新时间戳"的永久性错误记录;
 *   - pHash / dHash 都不能是0:全0的记录塞进 BK-tree 会污染检索。
 * 不满足就走重算那条路,重算完正好把坏记录修好。
 */
inline bool cacheRecordUsable(const FileEntry &fe)
{
    char zero[32]{};
    if (std::memcmp(fe.SHA256, zero, 32) == 0) return false;   // 从没算过 / 截断
    /*
     * pHash / dHash 单侧为 0 是【合法结果】,不是坏记录。
     *
     * 典型是扫描件/同人本的纯白封面页:整页没有梯度 -> dHash 全 0,
     * pHash 只剩 DC -> 算出来是 1。以前这里写成 `pHash == 0 || dHash == 0`
     * 就拒,后果是这类页**每次扫描都被重算**(而且重算完还是 0),
     * 顺带把缓存判脏 -> 每次都整份重写 tscch。
     * 实测 G:\Imageset\ame_usagi... 这份缓存有 12 条这种记录
     * (1410x2024 白页,dHash=0/pHash=1),33MB 的 tscch 因此每扫一次就重写一次,
     * 表现就是"扫到最后几个文件总要卡一小会"。
     *
     * 只把"两个哈希都是 0"(等于没有任何特征)当成坏记录挡掉。
     */
    if (fe.pHash == 0 && fe.dHash == 0) return false;
    return true;
}

/*
 * 扫描参数,DedupWorker/独立cache按钮/训练集cache读取共用。
 *
 * 缓存更新策略(只要 reuseCache 或 cache 为真,就会读取同目录的 image_features.tscch):
 *   路径在缓存 && 大小/修改时间都没变  -> 复用,不做任何重活;
 *   大小或修改时间变了                -> 算SHA-256确认:相同则复用,不同则重新计算;
 *   路径不在缓存(新增文件)            -> 计算特征并加入;
 *   缓存里有但目录里已经没有           -> 写回时自动剔除。
 *   缓存里的记录"丢了"、同时新增的文件 SHA-256 和它相同
 *                                    -> 同一个文件只是移位/改名,直接搬特征(见 scanFolders)。
 */
struct ScanOptions {
    bool cache = false;      // 更新/写入 image_features.tscch(同时决定要不要算SHA-256)
    bool sha256 = false;     // 是否计算SHA-256(cache为true时自动为true)
    bool thumbnail = false;  // 是否生成缩略图(不写入tscch,只在内存里)
    bool reuseCache = false; // 允许复用tscch里未发生变化的记录

    /*
     * 是否连"上一层目录"的 tscch 也一起用:
     *   true  = 去重扫描用。除了本目录/子目录的 tscch,还沿父目录一路查到盘根;
     *            命中的那份只做部分更新(mergeOnly,只动 F 对应的子目录时间戳)。
     *   false = "cache image features"按钮 / 训练集。只处理 F 及其子目录的 tscch,
     *            F 的上级目录不参与 —— 用户按的是这个目录的按钮,不该顺手改它父目录的缓存。
     *
     * 两种模式都【就近写回】:子目录里的 tscch 不是只读复用源,
     * 某张图的新增/更新落在离它最近的那份缓存里(见 scanFolders 的缓存策略)。
     */
    bool useNestedCaches = false;

    /*
     * false = 只扫每个输入目录的**直属**文件,不进入子目录。
     *
     * 由主窗口的 cb_recursive 控制,只作用于"普通目录去重"和"普通内部去重":
     *   · 半选(InternalDedup::AcrossTopDirs)必须递归 —— 目录级判定要靠顶层子目录;
     *   · 导入整理 / remove cover / clear file 自己决定,不看这个开关。
     */
    bool recursive = true;

    /* 实际是否需要计算SHA-256:显式要求,或者要写cache */
    bool needSHA256() const { return sha256 || cache; }

    /*
     * 写 tscch 失败(几乎都是磁盘满)时的回调。
     *
     * 返回 true  = 调用方已经腾出空间,重试写回;
     * 返回 false = 放弃落盘(特征仍在内存里、.part 也保留,
     *              下次扫描同一目录会把它并回去)。
     *
     * 不设这个回调就是旧行为:只写一条 qWarning,界面上什么都看不到。
     * clear file 用它实现"连 tscch 都写不下时暂停特征扫描"。
     */
    std::function<bool(const QString &dir, qint64 needBytes, const QString &error)>
        onCacheWriteFailed;

    /*
     * 写进"未完成扫描标记"里的来源,只用于下次启动的提示文案:
     *   "cache"  = 缓存按钮 / 训练集缓存更新
     *   "dedupe" = 去重窗口自己发起的扫描(默认)
     */
    QString journalMode = QStringLiteral("dedupe");
};

/*
 * 扫描一个图片文件,把特征填进FileEntry。读取失败返回false(此时entry被重置)。
 * options 决定要不要做那些“贵”的步骤:
 *   - 只有 options.needSHA256() 为真时才计算SHA-256(不读/不写cache时算它没有意义);
 *   - 只有 options.thumbnail 为真时才生成缩略图;
 *   - pHash / dHash / ORB 这些查重必需的特征总是计算。
 * 任何需要“从文件得到FileEntry”的地方都应该复用这个函数,以保证特征口径一致。
 */
bool computeFileFeatures(FileEntry &entry, const QString &path,
                         const ScanOptions &options);

/*
 * 递归收集folder下所有支持的图片(剔除image_features.tscch等非图片文件)。
 * regexFilter 匹配的是【文件名去掉后缀之后的部分】(用 completeBaseName),
 * 采用"包含即命中"的部分匹配、忽略大小写;为空表示不过滤。
 * 旧的 -r"..." / -s"..." 用法已经彻底去掉。
 *
 * outCacheDirs:可选。返回"递归范围内已经存在 image_features.tscch 的目录"列表
 * (只统计含匹配图片的目录),用于就近复用缓存。
 *
 * exclude(可选):返回 true 表示这个文件【不要】收集。
 *   给"导入整理"用:二进制阶段已经判定为重复/已安排迁移的文件不必再参与
 *   视觉比较,提前排除可以省掉它们的特征计算(大图 pHash/ORB 很贵),
 *   也让候选列表里的每一张图都是真正需要用户决策的。
 *   注意这是"排除法"而不是显式白名单:调用方只需要提供排除集合。
 *
 * recursive:false = 只收 folder 的**直属**文件,不进子目录。
 *   clear file 的"散图"这一侧必须是这个语义:zip 解压出来的图片就在
 *   folder/<子目录>/ 里,递归收集会把它们和散图混在一起(自己比自己),
 *   也会把它们的特征写进文件夹自己的 tscch。
 */
QStringList collectImageFiles(const QString &folder,
                              const QString &regexFilter = QString(),
                              QStringList *outCacheDirs = nullptr,
                              const std::function<bool(const QString&)> &exclude = {},
                              bool recursive = true);

class DedupWorker : public QObject {
    Q_OBJECT
public:
    /* 一次缓存更新的统计信息 */
    struct CacheUpdateStats {
        int total = 0;       // 目录里被处理的图片总数
        int reused = 0;      // 直接复用旧缓存记录的(含 moved)
        int recomputed = 0;  // 新增 / 内容变化而重新计算的
        /*
         * 其中"只是移位/改名"的条数:缓存里丢失的那条记录和本次新增的文件
         * SHA-256 相同,特征直接搬了过来,没有重新解码。
         * 已经计入 reused(不重复计入 total)。
         */
        int moved = 0;
    };

    DedupWorker(std::atomic<int>*currentEpoch,QObject *parent=nullptr);
    ~DedupWorker();
    std::atomic<int>* currentEpoch;
    int m_epoch;
    /* 给 QStringList 形式的输入(QString 不带 cache 信息)用的默认值 */
    void setCacheEnabled(bool on) { m_cacheEnabled = on; }
    bool cacheEnabled() const { return m_cacheEnabled; }
    // start scanning folders; returns immediately. emits signals when done/progress.
    void startScan(const auto &folders,int epoch)
    {
        cancel();
        m_epoch = epoch;
        m_cancel = false;

        m_thread = std::thread([this, folders, epoch]() {
            threadMain(folders,epoch);
        });
    }

    /*
     * 只扫描指定的文件列表(remove cover 用:调用方已经筛好 A/B 对,只缺特征)。
     *
     * 缓存:读,不写。
     *
     * 这批文件确实只是"临时挑出来的一组",但它们就躺在用户的画廊目录里,而那里的
     * image_features.tscch 早就有现成的 pHash/dHash/ORB(普通去重默认开着 cache)。
     * 不读的话,封面 + 内容全部重新解码 —— 32MP 一张接近 1 秒,一个几千张的目录
     * 就是几十分钟,而用户看到的只是"remove cover 卡住了"。
     *
     * 不写:这里只扫整体里的一小撮,"没扫到"绝不等于"磁盘上不存在",写回会把
     * 没扫到的记录当成过期删掉(mergeOnly 也挡不住语义上的错)。
     * 复用的判定和 scanFolders 一致:路径命中 + 大小/修改时间都没变 + 记录可用。
     */
    void startScanFiles(const QStringList &paths, int epoch)
    {
        cancel();
        m_epoch = epoch;
        m_cancel = false;

        m_thread = std::thread([this, paths, epoch]() {
            try {
                ScanOptions options;
                options.cache = false;
                options.sha256 = false;
                options.thumbnail = false;
                options.reuseCache = false;
                options.useNestedCaches = false;

                QVector<FileEntry> results;
                results.reserve(paths.size());
                int reused = 0;
                int computed = 0;

                /* ---------- 缓存复用:先把"需要哪些路径"整理成两个键 ---------- */
                /*
                 * Windows 路径大小写不敏感,而 tscch 里存的是写缓存那一刻的写法,
                 * 调用方给的路径也可能来自另一处枚举。所以精确键命中不了时,
                 * 再用小写键兜一次;存进结果时统一用【调用方给的写法】,
                 * 免得同一张图在列表里出现两种大小写不同的路径。
                 */
                QSet<QString> wantExact;
                QHash<QString, QString> wantByLower;   // lower -> 调用方写法
                QSet<qint64> requestedSizes;           // 这批文件的字节数(移位识别用的粗筛)
                wantExact.reserve(paths.size());
                wantByLower.reserve(paths.size());
                for (const QString &p : paths) {
                    const QString clean = QDir::cleanPath(QDir::toNativeSeparators(p));
                    wantExact.insert(clean);
                    wantByLower.insert(clean.toLower(), clean);
                    requestedSizes.insert(QFileInfo(p).size());
                }

                /* 文件目录 -> 往上第一个存在的 tscch 所在目录("" = 没有) */
                QHash<QString, QString> cacheDirByFileDir;
                /* 已载入的缓存:只保留本次真正用得上的记录,大缓存也不会整份驻留 */
                QHash<QString, std::shared_ptr<QHash<QString, FileEntry>>> loadedCaches;

                /*
                 * ---- 移位识别的索引 ----
                 * 缓存里记着、但磁盘上已经没有这个路径的记录,按 SHA-256 记下来;
                 * 本次要算特征的文件里如果有内容完全相同的,直接把记录搬过来。
                 * 和 scanFolders 里那条规则完全一致:
                 *   - 只认字节数对得上的记录(移位不改文件大小),省掉无关记录的 stat;
                 *   - 同一个 SHA 对应多条不同路径 = 有重复文件 -> 不认领
                 *     (用户去重删掉一份的可能性更大,拿它的记录去认领会造出假记录)。
                 */
                QHash<QByteArray, FileEntry> movedUnique;   // SHA -> 唯一那条丢失记录
                QHash<QByteArray, QString>   movedShaPath;  // SHA -> 见过的路径(防同一记录重复计数)
                QHash<QByteArray, int>       movedCounts;   // SHA -> 记录条数
                QSet<QByteArray>             movedUsed;     // 已经被认领过的
                QSet<qint64>                 movedSizes;    // 有丢失记录的文件大小
                int moved = 0;

                auto findCacheDir = [&](const QString &fileDir) -> QString {
                    const auto hit = cacheDirByFileDir.constFind(fileDir);
                    if (hit != cacheDirByFileDir.constEnd()) return hit.value();

                    /*
                     * 一路向上找第一个存在的 tscch,结束时把走过的整条链都记下来
                     * (不只是起点):同一棵树下成千上万个文件目录,第一次走完就填好了,
                     * 之后每个文件都只是一次哈希查找,不会每个目录都爬到盘根。
                     * found 为空表示这条链上一层缓存都没有,同样可以直接记住 ——
                     * 本次扫描只读不写,中途不会凭空出现新的 tscch。
                     */
                    QString dir = fileDir;
                    QString found;
                    QStringList visited;
                    for (int guard = 0; !dir.isEmpty() && guard < 256; ++guard) {
                        visited << dir;
                        if (QFileInfo::exists(cacheio::cacheFilePathForFolder(dir))) {
                            found = dir;
                            break;
                        }
                        const QString up = QDir::cleanPath(
                            QDir::toNativeSeparators(QFileInfo(dir).absolutePath()));
                        if (up.isEmpty() || up.compare(dir, Qt::CaseInsensitive) == 0)
                            break;      // 到盘根了
                        dir = up;
                    }
                    for (const QString &d : std::as_const(visited))
                        cacheDirByFileDir.insert(d, found);
                    cacheDirByFileDir.insert(fileDir, found);
                    return found;
                };

                auto loadCache = [&](const QString &cacheDir)
                    -> std::shared_ptr<QHash<QString, FileEntry>> {
                    const auto hit = loadedCaches.constFind(cacheDir);
                    if (hit != loadedCaches.constEnd()) return hit.value();

                    auto wanted = std::make_shared<QHash<QString, FileEntry>>();
                    QString err;
                    const QVector<FileEntry> all = cacheio::loadFileEntryCache(
                        cacheio::cacheFilePathForFolder(cacheDir), &err);
                    for (const FileEntry &fe : all) {
                        if (!cacheRecordUsable(fe)) continue;      // SHA/pHash 坏的不要
                        const QString key = QDir::cleanPath(
                            QDir::toNativeSeparators(fe.currentPath));

                        /* ---- 移位识别:先把"这一批文件可能用得上的记录"整理出来 ---- */
                        if (requestedSizes.contains(fe.filesize)) {
                            const QByteArray sha(reinterpret_cast<const char *>(fe.SHA256), 32);
                            // 同一份记录可能被多个层级的 tscch 各存了一份,只算一条
                            if (movedShaPath.value(sha) != key) {
                                ++movedCounts[sha];
                                movedShaPath.insert(sha, key);
                            }
                            const bool requested = wantExact.contains(key)
                                                   || wantByLower.contains(key.toLower());
                            if (!requested && !QFileInfo::exists(key)) {
                                // 路径真的没了 -> 是"丢失记录",可以当来源
                                if (!movedUnique.contains(sha))
                                    movedUnique.insert(sha, fe);
                                movedSizes.insert(fe.filesize);
                            }
                        }

                        if (wantExact.contains(key)) {
                            wanted->insert(key, fe);
                        } else {
                            const auto lower = wantByLower.constFind(key.toLower());
                            if (lower != wantByLower.constEnd())
                                wanted->insert(lower.value(), fe);
                        }
                    }
                    loadedCaches.insert(cacheDir, wanted);
                    return wanted;
                };

                /*
                 * 先把缓存全部载入(loadCache 内部有 memo),再开始按文件走。
                 * 否则"丢失记录索引"会随着遍历一个个文件才慢慢长出来,
                 * 排在前面的文件看不到后面的记录,移位识别就漏了。
                 * 只多一次目录存在性判断(已经被 memo 吃掉了),不读文件。
                 */
                for (const QString &p : std::as_const(paths)) {
                    const QFileInfo fi(p);
                    const QString cd = findCacheDir(
                        QDir::cleanPath(QDir::toNativeSeparators(fi.absolutePath())));
                    if (!cd.isEmpty()) loadCache(cd);
                }

                int done = 0;
                for (const QString &p : paths) {
                    if (m_cancel.load()) return;

                    const QFileInfo fi(p);
                    const QString clean = QDir::cleanPath(QDir::toNativeSeparators(p));

                    FileEntry fe;
                    bool reusedThis = false;
                    const QString fileDir = QDir::cleanPath(
                        QDir::toNativeSeparators(fi.absolutePath()));
                    const QString cacheDir = findCacheDir(fileDir);
                    if (!cacheDir.isEmpty()) {
                        const auto cache = loadCache(cacheDir);
                        const auto hit = cache->constFind(clean);
                        if (hit != cache->constEnd()) {
                            const FileEntry &ce = hit.value();
                            if (ce.filesize == fi.size()
                                && ce.modTime == fi.lastModified().toMSecsSinceEpoch()) {
                                fe = ce;          // 深拷贝(orbDescriptors 会 clone)
                                reusedThis = true;
                            }
                        }
                    }

                    if (reusedThis) {
                        ++reused;
                    } else {
                        /*
                         * 路径不在缓存里(或者大小/时间变了):先看它是不是"只是被移位了" ——
                         * 缓存里有一条丢了的记录,内容和它完全相同。
                         * 只有字节数对得上的文件才算 SHA,常规情况(没有移位)一次都不多读。
                         */
                        bool movedThis = false;
                        if (!movedUnique.isEmpty() && movedSizes.contains(fi.size())) {
                            bool ok = false;
                            const QByteArray sha = dedup::fileSHA256(p, &ok);
                            if (ok && sha.size() == 32
                                && movedCounts.value(sha) == 1
                                && !movedUsed.contains(sha)) {
                                const auto it = movedUnique.constFind(sha);
                                if (it != movedUnique.constEnd()) {
                                    fe = it.value();          // 深拷贝(orbDescriptors 会 clone)
                                    std::memcpy(fe.SHA256, sha.constData(), 32);
                                    fe.filesize = fi.size();
                                    fe.modTime  = fi.lastModified().toMSecsSinceEpoch();
                                    movedUsed.insert(sha);
                                    movedThis = true;
                                    ++moved;
                                    ++reused;   // 和 scanFolders 一致:移位也算"复用"(不重算)
                                }
                            }
                        }
                        if (!movedThis) {
                            if (!computeFileFeatures(fe, p, options)) {
                                emit scanProgress(++done, paths.size());
                                continue;
                            }
                            ++computed;
                        }
                    }

                    // 路径字段以本次调用为准(缓存里记的是写缓存那一刻的位置)
                    fe.originalPath    = fi.absoluteFilePath();
                    fe.currentPath     = fe.originalPath;
                    fe.folder          = fi.absolutePath();
                    fe.filename        = fi.fileName();
                    fe.rootInputFolder = fi.absolutePath();
                    fe.folderPriority  = 0;
                    fe.inDuplicate     = false;
                    results.push_back(fe);

                    emit scanProgress(++done, paths.size());
                }

                if (m_cancel.load()) return;
                /*
                 * 一律打一行:没有缓存时也打(用到 0 份),这样"为什么这次这么慢"
                 * 和"缓存到底有没有被用上"在日志里能直接读出来。
                 */
                emit logMessage(QString("[%1] 文件列表扫描: %2 个文件(缓存复用 %3 / 重新计算 %4,其中移位复用 %5,用到 %6 份 tscch)")
                                    .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"))
                                    .arg(results.size()).arg(reused).arg(computed).arg(moved)
                                    .arg(loadedCaches.size()));
                emit scanFinished(results, epoch);
            } catch (const std::exception &e) {
                emit logMessage(QString("[%1] scanFiles exception: %2")
                                    .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"))
                                    .arg(e.what()));
            } catch (...) {
                emit logMessage(QString("[%1] scanFiles unknown exception")
                                    .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")));
            }
        });
    }

    // request cancel
    void cancel();
    void threadMain(const auto &folders,int epoch)
    {
        try {
            ScanOptions options;
            // cache 由每个 folder 自己带(见 FolderAndCompareOption::cache),
            // 这里的 m_cacheEnabled 只是给 QStringList 输入兜底。
            options.cache     = m_cacheEnabled;
            options.sha256    = m_cacheEnabled;
            // 缩略图改成懒加载:去重窗口和训练集窗口都只在真的要显示到列表里时才生成
            options.thumbnail = false;
            /*
             * 复用旧记录也跟着 cache 开关走:没勾 cache 就完全无视已有的 tscch,
             * 老老实实重算一遍(和加这个开关之前的行为一致)。
             */
            options.reuseCache = m_cacheEnabled;
            options.useNestedCaches = true; // 去重扫描:子目录/上一层目录的 tscch 都拿来用

            const QVector<FileEntry> results = scanFolders(
                folders, options,
                [this](int done, int total) { emit scanProgress(done, total); },
                [this]() { return m_cancel.load(); },
                /*outStats=*/nullptr,
                [this](const QString &line) { emit logMessage(line); });

            if (m_cancel) return;

            emit scanFinished(results, epoch);
        } catch (const std::exception &e) {
            emit logMessage(QString("[%1] worker thread top-level exception: %2")
                                .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"))
                                .arg(e.what()));
        } catch (...) {
            emit logMessage(QString("[%1] worker thread top-level unknown exception")
                                .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")));
        }
    }

    /*
     * 核心扫描实现(不依赖QObject,可被任何线程/任何调用方复用)。
     *
     * folders 支持两种形态(用 if constexpr 适配,调用方不用关心):
     *   - QStringList / QVector<QString>:只有目录名,cache 用 options.cache;
     *   - QVector<FolderAndCompareOption> 这类结构体:每个目录自带 folder / regexFilter /
     *     internal / cache(见 MainWindow::on_btn_duplicate_clicked)。
     *
     * 缓存策略(核心,改动很大,务必看清):
     *   1) 一个文件归"离它最近的、且已经存在的 tscch"管,候选依次是:
     *        - 文件所在目录、以及它各级上级目录里【已经存在】的 tscch(取最近的一个);
     *        - 本次扫描目录 F 自己的 tscch(没有也会在开着 cache 时被创建);
     *        - F 的**任意一级上级目录**的 tscch(从父目录一路查到盘根,见 parentCacheDirs)。
     *      读取按"从深到浅":深的那份更新,优先信它;深层没有再看浅层。
     *      **写回也走同一条链,取最近的可写槽** —— 即就近写回:
     *      某张图的新增/更新只落在离它最近的 tscch 里,F/image_features.tscch
     *      不再充当整棵子树的汇总缓存。
     *      (2026-11-20 修正:以前权威模式把子目录的 tscch 当"只读复用源",
     *       所有记录一律写进 F,按一次 cache image features 就把子缓存全并进顶层。
     *       子目录缓存被打散成两份,用户明确不要这个语义。)
     *   2) 每个 tscch 的写回分两种:
     *        - 权威写入(mergeOnly=false):目录就在 F 之下(含 F 自己),而且本次没有正则筛选。
     *          这时本次扫描覆盖了它的整个作用域,可以剔除磁盘上已不存在的记录,并更新 savedAt。
     *        - 只合并(mergeOnly=true):① 有正则筛选(正则排除掉的文件"没扫到"≠"不存在",
     *          既不能剔除也不能动 savedAt);② 只是 F 的上级目录(只覆盖了它的一部分,
     *          所以只更新"F 这个顶层子目录"的时间戳,不动整体 savedAt)。
     *   3) 剔除记录的依据是"文件在磁盘上已经不存在",不是"本次没扫到" —— 这样局部扫描
     *      永远不会误删别人的记录。
     *   4) 命中浅层记录、但更近处有可写 tscch 时,记录"下沉"到更近那份,同时从浅层
     *      drop 掉(见 pushEntry)。顶层 tscch 里历史遗留的子目录记录会随扫描逐步收敛,
     *      不做一次性批量搬迁 —— 搬错一个就白丢一批特征,而收敛慢一点没有代价。
     *   5) 文件只是被移位/改名(缓存里那条路没了、别处冒出一个内容完全相同的文件)时,
     *      特征直接搬过去,不重新解码;同一内容的重复文件不参与(用户去重的可能性更大),
     *      细节见下面"识别移位/改名"那一段。
     */
    static QVector<FileEntry> scanFolders(const auto &folders,
                                          const ScanOptions &options,
                                          const std::function<void(int,int)> &progress = {},
                                          const std::function<bool()> &cancelled = {},
                                          CacheUpdateStats *outStats = nullptr,
                                          /*
                                           * 可选日志出口。
                                           * scanFolders 是静态函数(不依赖 QObject),
                                           * 所以不能自己 emit;调用方(线程入口)把
                                           * logMessage 包一层传进来,时序探针就能进
                                           * 用户看得到的日志窗口。
                                           */
                                          const std::function<void(const QString&)> &logger = {})
    {
        /* 把两种输入统一成一种规格 */
        struct FolderSpec {
            QString folder;
            QString regexFilter;
            int     internal = static_cast<int>(InternalDedup::All);
            bool    cache = false;
            /*
             * 可选的"排除谓词"(见 collectImageFiles 的说明)。
             * 输入结构体里带 fileFilter 成员时才会被取用 —— 普通调用方
             * (MainWindow 的 FolderAndCompareOption)没有这个成员,行为不变。
             */
            std::function<bool(const QString&)> fileFilter = {};
            /*
             * 只读复用 image_features.tscch,不写回(和 cache 分开)。
             *
             * 这个开关给"只想拿现成特征、不想动别人缓存"的调用方用;
             * 导入整理(importdedupe)现在同时开 cache + reuseCache:
             * 读现成的 pHash/dHash/ORB(不读就得把几万张图重新解码一遍),
             * 也算完写回(mergeOnly,只覆盖本次候选子集)。
             * 早先它只读不写,理由是"关窗后文件会被搬走,记录马上指向不存在的路径";
             * 现在 scanFolders 能靠 SHA-256 把丢了记录的旧路径和新增文件对上
             * (见"移位复用"),这条理由不成立了。
             *
             * 默认 false:普通去重仍然只看 cache(cb_cache)。
             */
            bool    reuseCache = false;
            /* 见 FileEntry::compareSide(-1 = 不分组) */
            int     compareSide = -1;
            /* false = 只收直属文件(见 ScanOptions::recursive) */
            bool    recursive = true;
            /* 差集:这些子目录里的文件不收(用户把它们作为独立输入目录) */
            QStringList excludeDirs;
        };
        auto toSpec = [&options](const auto &item) -> FolderSpec {
            using T = std::decay_t<decltype(item)>;
            FolderSpec spec;
            spec.recursive = options.recursive;
            if constexpr (std::is_same_v<T, QString>) {
                spec.folder   = item;
                spec.internal = static_cast<int>(InternalDedup::All);
                spec.cache    = options.cache;
            } else {
                spec.folder      = item.folder;
                spec.regexFilter = item.regexFilter;
                spec.internal    = item.internal;
                spec.cache       = item.cache || options.cache;
                if constexpr (requires { item.fileFilter; })
                    spec.fileFilter = item.fileFilter;
                if constexpr (requires { item.reuseCache; })
                    spec.reuseCache = item.reuseCache;
                if constexpr (requires { item.compareSide; })
                    spec.compareSide = item.compareSide;
                if constexpr (requires { item.recursive; })
                    spec.recursive = item.recursive;
                if constexpr (requires { item.excludeDirs; })
                    spec.excludeDirs = item.excludeDirs;
            }
            return spec;
        };

        auto isCancelled = [&cancelled]() -> bool { return cancelled && cancelled(); };

        QVector<FileEntry> results;
        CacheUpdateStats stats;
        if (outStats) *outStats = stats;

        /* 每个输入目录一个 job */
        struct FolderJob {
            FolderSpec       spec;
            QString          folder;        // cleanPath 后的绝对路径
            QStringList      files;
            QStringList      subCacheDirs;  // 递归范围内已有 tscch 的目录(含 folder 自己)
            /*
             * folder 的**各级**上级目录里已经有 tscch 的那些(从近到远)。
             *
             * 以前只看一层(父目录)。但实际目录结构经常是
             *   D:/pics/image_features.tscch            <- 只在很上层有一份
             *   D:/pics/作者A/2024/xxx/*.jpg
             * 这时扫 D:/pics/作者A/2024 既命中不了自己的 tscch、也命中不了父目录的,
             * 于是整棵树被当成"没有缓存"全部重算 —— 而那份上层 tscch 里其实
             * 记着这些文件的全部特征。所以现在一路查到盘根为止。
             */
            QStringList      parentCacheDirs;
            bool             hasRegex = false;
            /* 有排除谓词:文件"没被收集"不等于"不存在",写回缓存只能合并 */
            bool             hasFileFilter = false;
            /*
             * 正则写了但语法不合法。
             * 这种输入【不能】当成"没有筛选":那样写入会变成权威写入(剔除+更新 savedAt),
             * 而实际上用户是有筛选意图的。保守做法 = 当成"有筛选":扫描不过滤(别漏图),
             * 但写回一律只合并、不剔除、不动 savedAt。
             */
            bool             regexInvalid = false;
            QRegularExpression re;
        };

        // ================= 收集阶段:一次遍历拿到图片 + 候选 tscch =================
        QVector<FolderJob> jobs;
        int totalCount = 0;

        for (const auto &rawItem : folders) {
            if (isCancelled()) return results;

            FolderJob job;
            job.spec = toSpec(rawItem);
            job.folder = QDir::cleanPath(QDir::toNativeSeparators(job.spec.folder.trimmed()));
            if (job.folder.isEmpty() || !QDir(job.folder).exists()) {
                qWarning() << "[scanFolders] folder not found:" << job.spec.folder;
                continue;
            }

            const QString rx = job.spec.regexFilter.trimmed();
            if (!rx.isEmpty()) {
                job.re = QRegularExpression(rx);
                job.re.setPatternOptions(QRegularExpression::CaseInsensitiveOption);
                if (job.re.isValid()) {
                    job.hasRegex = true;
                } else {
                    /*
                     * 语法不合法:不能静默当成"没有筛选"(那会让写回变成权威写入,
                     * 剔除记录并更新 savedAt)。这里保守处理:扫描不过滤,
                     * 但按"有筛选"对待 —— 写回只合并、不剔除、不动 savedAt。
                     * 正常来说 MainWindow 会先校验并拦下来,这里只是兜底。
                     */
                    job.hasRegex = true;
                    job.regexInvalid = true;
                    qWarning() << "[scanFolders] invalid regex, scan without filter "
                                  "but keep caches merge-only:" << rx << job.re.errorString();
                }
            }

            const QString scanFilter = (job.hasRegex && !job.regexInvalid) ? rx : QString();
            /*
             * 差集:用户把 a 和 a/b 同时作为输入时,a 这一边要把 a/b 子树排除掉
             * (见 MainWindow 的包含关系处理)。和 fileFilter 一样,这属于"没扫到
             * ≠ 不存在",写回只能合并不剔除。
             */
            const QStringList excludedDirs = job.spec.excludeDirs;
            job.hasFileFilter = static_cast<bool>(job.spec.fileFilter)
                                || !excludedDirs.isEmpty();

            auto scanExclude = [&job, excludedDirs](const QString &path) -> bool {
                if (job.spec.fileFilter && job.spec.fileFilter(path))
                    return true;
                for (const QString &d : excludedDirs) {
                    if (!d.isEmpty() && dedup::isUnderOrEqual(path, d))
                        return true;
                }
                return false;
            };

            job.files = collectImageFiles(job.folder, scanFilter, &job.subCacheDirs,
                                          scanExclude, job.spec.recursive);

            if (options.useNestedCaches) {
                /*
                 * 向上逐级找 tscch,一直到盘根(不再只看一层)。
                 *
                 * 只把"真的存在 tscch"的目录记下来 —— 不存在的上级目录加进槽位没有意义,
                 * 还会让"写入策略"那段无缘无故多出一堆 mergeOnly 的槽。
                 * 上限 256 层纯粹是防御(真到极限也会先被盘符/盘根条件打断)。
                 */
                QString dir = QDir::cleanPath(
                    QDir::toNativeSeparators(QFileInfo(job.folder).absolutePath()));
                for (int guard = 0; !dir.isEmpty() && guard < 256; ++guard) {
                    if (QFileInfo::exists(cacheio::cacheFilePathForFolder(dir)))
                        job.parentCacheDirs << dir;

                    const QString up = QDir::cleanPath(
                        QDir::toNativeSeparators(QFileInfo(dir).absolutePath()));
                    if (up.isEmpty() || up.compare(dir, Qt::CaseInsensitive) == 0)
                        break;      // 到盘根了
                    dir = up;
                }
            }

            totalCount += job.files.size();
            jobs.push_back(std::move(job));
        }

        int done = 0;
        auto reportProgress = [&progress, &done, totalCount]() {
            if (!progress) return;
            const int total = totalCount > 0 ? totalCount : done;
            progress(std::min(done, total), total);
        };

        // ================= 扫描阶段:逐目录处理 =================
        for (int jobIndex = 0; jobIndex < jobs.size(); ++jobIndex) {
            if (isCancelled()) break;

            FolderJob &job = jobs[jobIndex];
            if (job.files.isEmpty()) continue;

            /*
             * 计时探针:用户反馈"扫到最后几个文件总要卡一小会",而缓存本身是完整的
             * (1042 张图 0 条缺失)。卡顿只可能来自三处,这里分别计时,下次日志里
             * 直接能看出是哪一处:
             *   · 补算 SHA(大小/时间戳变了的那几个)
             *   · 重算特征(解码最慢,逐文件记最慢的那一个)
             *   · 收尾写回(整份 tscch 读出来合并再原子重写,33MB 量级)
             */
            QElapsedTimer jobTimer;
            jobTimer.start();
            int  hashCheckedCount = 0;
            qint64 slowestComputeMs = 0;
            QString slowestComputeFile;
            qint64 cacheWriteMs = 0;
            int    cacheWrittenCount = 0;

            ScanOptions jobOptions = options;
            jobOptions.cache      = job.spec.cache;
            /*
             * 要写缓存就必须算 SHA-256;同时保留调用方显式要求的(例如 cache 按钮的 doSHA256)。
             *
             * 另外:半选目录(AcrossTopDirs = 文件夹去重)**无条件**算 SHA-256。
             * 那个模式的承诺就是"B 里和 A 二进制或像素重复的图自动移除",
             * 而"二进制重复"完全靠 SHA256 判断(`FileEntry::sameContentAs`)。
             * 以前 SHA 只跟着 cache 开关走,用户没勾 cache 时 SHA 全 0,
             * 于是字节级去重一个都不会发生 —— 看起来像"自动去重没生效"。
             * 代价几乎为零:computeFileFeatures 早把整个文件读进内存了,
             * SHA 直接从这块内存算,不额外读盘。
             *
             * 注意:这只影响"算不算",不影响"写不写" —— 落盘仍然只看 jobOptions.cache。
             */
            jobOptions.sha256     = options.sha256 || job.spec.cache
                                    || job.spec.internal == int(InternalDedup::AcrossTopDirs);
            /*
             * 复用旧记录:全局开关(options.reuseCache)或者这个目录自己的 cache 开关,
             * 再加上第三种情况 —— 目录只要求"只读复用"(导入整理)。
             */
            jobOptions.reuseCache = options.reuseCache || job.spec.cache
                                    || job.spec.reuseCache;

            const bool useExistingCache = jobOptions.reuseCache || jobOptions.cache;
            const bool needThumb = jobOptions.thumbnail;

            /* 一个 tscch 就是一个"槽" */
            struct CacheSlot {
                QString dir;
                bool    exists = false;
                bool    writable = false;
                bool    mergeOnly = false;
                bool    dirty = false;      // 有记录被重算/时间戳变了
                bool    unreadable = false; // 文件在但读不出来 -> 绝不能覆盖它
                QString touchSubDir;
                quint64 touchSubDirMtime = 0;
                QHash<QString, FileEntry> byPath;
                QVector<FileEntry> out;
                /*
                 * 本槽里被"移位"消费掉的旧记录(文件已经不在这个路径上,
                 * 特征被搬到新位置了)。写回时无条件删除,见 WriteOptions::dropPaths。
                 */
                QSet<QString> dropPaths;
                int reused = 0;
                int recomputed = 0;
                /* 写回失败:.part 要保留,否则这些刚算出来的特征就白算了 */
                bool writeFailed = false;
                /*
                 * 边算边存的追加日志(见 cacheio::PartialCacheLog)。
                 * 用 shared_ptr:QVector 扩容会搬元素,而 QFile 不可拷贝。
                 * 只有"本 job 的目录或它下面"的可写槽才开 —— 上一层目录只是部分更新,
                 * 在那儿留 .part 意义不大,还会把 .part 撒得到处都是。
                 */
                std::shared_ptr<cacheio::PartialCacheLog> part;
            };

            QVector<CacheSlot> cacheSlots;
            /* 目录(统一小写,Windows 路径大小写不敏感)-> 槽下标 */
            QHash<QString, int> slotByDir;

            auto addSlot = [&](const QString &dir, bool exists, bool writable) -> int {
                const QString d = QDir::cleanPath(QDir::toNativeSeparators(dir));
                const QString dKey = d.toLower();
                const auto it = slotByDir.constFind(dKey);
                if (it != slotByDir.constEnd()) {
                    CacheSlot &s = cacheSlots[it.value()];
                    s.exists   = s.exists || exists;
                    s.writable = s.writable || writable;
                    return it.value();
                }
                CacheSlot s;
                s.dir = d;
                s.exists = exists;
                s.writable = writable;
                cacheSlots.push_back(s);
                slotByDir.insert(dKey, cacheSlots.size() - 1);
                return cacheSlots.size() - 1;
            };

            /*
             * 本目录:即使还没有 tscch 也占一个槽(开着 cache 时我们会创建它)。
             * 就近写回之后,它只负责两批文件:F 的直属文件,以及"从文件所在目录
             * 一直到 F 之间一个已存在 tscch 都没有"的那些子目录文件。
             */
            const int localSlot = addSlot(job.folder,
                                          QFileInfo::exists(cacheio::cacheFilePathForFolder(job.folder)),
                                          jobOptions.cache);

            if (jobOptions.useNestedCaches && useExistingCache) {
                for (const QString &d : std::as_const(job.subCacheDirs)) {
                    // 本目录已经作为 localSlot 加过了,这里不能再用 writable=true 覆盖它
                    // (那样会在 cache 关掉的时候也去写)
                    if (QDir::cleanPath(d).compare(job.folder, Qt::CaseInsensitive) == 0)
                        continue;
                    /*
                     * 可写性跟着 cache 开关走,不能写死 true:
                     * 只读复用(reuseCache-only)的调用方,本地槽必须不可写;
                     * 子目录的槽若一律可写,名义上的"只读"照样会改掉别人的缓存。
                     * 导入整理现在 cache=true,所以它的槽是可写的(mergeOnly)。
                     */
                    addSlot(d, true, jobOptions.cache);
                }
                if (!job.parentCacheDirs.isEmpty()) {
                    for (const QString &d : std::as_const(job.parentCacheDirs))
                        addSlot(d, true, jobOptions.cache);
                }
            } else if (jobOptions.cache) {
                /*
                 * 权威模式("cache image features"按钮 / 训练集):同样是就近写回。
                 *
                 * 子目录里早就躺着自己的 tscch(按作者/年份分开缓存过)时,那些记录
                 * 属于那个目录的作用域,本次递归扫描也完整覆盖了它们 —— 所以那个槽
                 * 既可读也可写,和 useNestedCaches 模式一致。
                 *
                 * 以前这里写死 writable=false(只读复用源),结果 F 的 tscch 成了
                 * 整棵子树的汇总文件:按一次按钮就把所有子缓存并进顶层,子目录那份
                 * 变成"死副本",下次改图两边各写一份、互不同步。
                 */
                for (const QString &d : std::as_const(job.subCacheDirs)) {
                    if (QDir::cleanPath(d).compare(job.folder, Qt::CaseInsensitive) == 0)
                        continue;
                    addSlot(d, true, /*writable=*/jobOptions.cache);
                }
            }

            // 读入各槽已有的记录
            if (useExistingCache) {
                /*
                 * 先处理上次没跑完留下的 .part:里面是已经算好的特征,
                 * 并回 tscch 之后这一轮就能直接复用,不用重算(见需求:"不要浪费")。
                 * 必须在"读入已有记录"之前做,否则这一轮读到的 byPath 里没有它们。
                 */
                for (CacheSlot &s : cacheSlots) {
                    if (!s.writable) continue;
                    const QString tscchPath = cacheio::cacheFilePathForFolder(s.dir);
                    if (!QFileInfo::exists(cacheio::partialCachePathFor(tscchPath)))
                        continue;
                    QString err;
                    const int merged = cacheio::mergePartialCacheLog(tscchPath, &err);
                    if (merged < 0) {
                        qWarning() << "[scanFolders] merge leftover partial cache failed:"
                                   << s.dir << err;
                    } else {
                        /*
                         * 合并会把 tscch 创建出来(原来可能只有 .part)。
                         * 必须同步更新 exists —— slotChainFor() 只把 exists 的槽
                         * 放进查找链,不更新的话这一轮照样找不到刚并进去的记录,
                         * 于是又把它们全部重算一遍,"不浪费"就落空了。
                         */
                        s.exists = QFileInfo::exists(tscchPath);
                        qDebug() << "[scanFolders] merged leftover partial cache" << s.dir
                                 << merged << "entries";
                    }
                }

                for (CacheSlot &s : cacheSlots) {
                    if (!s.exists) continue;
                    QString err;
                    cacheio::CacheMeta meta;
                    const QVector<FileEntry> old =
                        cacheio::loadFileEntryCache(cacheio::cacheFilePathForFolder(s.dir),
                                                    &err, &meta);
                    if (!meta.readable) {
                        // 读不出来:记为"不可用",后面也不会去写它(写回时会拒绝覆盖)
                        s.unreadable = true;
                        qWarning() << "[scanFolders] cache unreadable (will not be overwritten):"
                                   << s.dir << err;
                        continue;
                    }
                    s.byPath.reserve(old.size());
                    for (const FileEntry &fe : old)
                        s.byPath.insert(QDir::cleanPath(fe.currentPath), fe);
                    qDebug() << "[scanFolders] loaded cache" << s.dir << old.size()
                             << "entries, savedAt"
                             << QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(meta.savedAt))
                                    .toString("yyyy-MM-dd HH:mm:ss");
                }
            }

            // 写回策略
            for (int si = 0; si < cacheSlots.size(); ++si) {
                CacheSlot &s = cacheSlots[si];
                if (!s.writable) continue;   // 只读复用源不参与写回策略
                if (dedup::isUnderOrEqual(s.dir, job.folder)) {
                    // F 自己或 F 之下的子目录:本次递归扫描覆盖了它的整个作用域
                    // 有正则筛选或有排除谓词时只能合并,不能剔除/改时间 ——
                    // "没被收集"不等于"磁盘上不存在"(被筛掉的文件照旧在磁盘上)。
                    s.mergeOnly = job.hasRegex || job.hasFileFilter;
                } else {
                    /*
                     * F 的上级目录:只覆盖了它的一部分
                     *
                     * touchSubDir 必须是"job.folder 相对 s.dir 的第一级子目录名",
                     * 不能简单取 job.folder 的叶子名 —— 以前只回看一层,
                     * 两者刚好相等;现在允许查到盘根,扫 D:/a/b/c 时
                     * D:/ 那份 tscch 里的键应该是 "a",写成 "c" 就永远对不上了。
                     */
                    s.mergeOnly = true;
                    const QString rel = QDir(s.dir).relativeFilePath(job.folder);
                    QString first = rel.section(QLatin1Char('/'), 0, 0);
                    first = first.section(QLatin1Char('\\'), 0, 0);
                    if (first.isEmpty() || first == QLatin1String(".."))
                        first = QFileInfo(job.folder).fileName();
                    s.touchSubDir = first;
                    s.touchSubDirMtime = static_cast<quint64>(
                        QFileInfo(QDir(s.dir).filePath(first)).lastModified().toMSecsSinceEpoch());
                }
            }

            /* ---- 边算边存:每算完 kFlushEveryImages 张就把 .part 刷到磁盘 ---- */
            constexpr int kFlushEveryImages = 128;
            int pendingPartAppends = 0;
            QStringList journalParts;
            const qint64 journalStartedAt = QDateTime::currentMSecsSinceEpoch();

            /*
             * 给本 job 里"本目录及以下"的可写槽开一份 .part。
             *
             * 为什么只开这些:上一层目录(盘根附近那份 tscch)只是部分更新,
             * 在那儿留 .part 会把文件撒得到处都是,收益却很小 ——
             * 绝大多数新算出来的记录都落在 job.folder 这一层。
             */
            for (CacheSlot &s : cacheSlots) {
                if (!s.writable || s.unreadable) continue;
                if (!dedup::isUnderOrEqual(s.dir, job.folder)) continue;

                auto log = std::make_shared<cacheio::PartialCacheLog>();
                const QString tscchPath = cacheio::cacheFilePathForFolder(s.dir);
                QString err;
                if (log->open(tscchPath, s.dir,
                              cacheio::partialCachePathFor(tscchPath), &err))
                    s.part = log;
                else
                    qWarning() << "[scanFolders] cannot open partial cache log:" << s.dir << err;
            }

            /* 把 .part 刷到磁盘,并更新"上次没跑完"的标记 */
            auto flushPartialCaches = [&](int doneNow) {
                for (CacheSlot &s : cacheSlots) {
                    if (!s.part) continue;
                    s.part->flush();
                    const QString p = cacheio::partialCachePathFor(
                        cacheio::cacheFilePathForFolder(s.dir));
                    if (!journalParts.contains(p))
                        journalParts << p;
                }
                if (!journalParts.isEmpty()) {
                    cacheio::writeScanJournal(job.folder,
                                              cacheio::cacheFilePathForFolder(job.folder),
                                              jobOptions.journalMode,
                                              doneNow, totalCount, journalParts);
                }
            };

            /*
             * 一个文件目录能用的缓存槽,按"从深到浅"排好。
             *
             * 以前是每个文件遍历全部槽 + isUnderOrEqual;槽一多(按作者分开缓存的
             * 目录动辄几百个)就是 O(文件数 x 槽数) 次 cleanPath,光路径比较就能
             * 把扫描拖住。这里改成从文件目录沿父目录往上走、每层查一次哈希,
             * 同一个目录只算一次(目录数远小于文件数)。
             */
            QHash<QString, QVector<int>> slotChainCache;
            auto slotChainFor = [&](const QString &fileDir) -> QVector<int> {
                const auto cached = slotChainCache.constFind(fileDir);
                if (cached != slotChainCache.constEnd()) return cached.value();

                QVector<int> chain;
                QString dir = fileDir;
                for (int guard = 0; !dir.isEmpty() && guard < 256; ++guard) {
                    const auto hit = slotByDir.constFind(dir.toLower());
                    if (hit != slotByDir.constEnd()) {
                        const int si = hit.value();
                        if (cacheSlots[si].exists && useExistingCache)
                            chain.push_back(si);
                    }
                    const QString up = QDir::cleanPath(
                        QDir::toNativeSeparators(QFileInfo(dir).absolutePath()));
                    if (up.isEmpty() || up.compare(dir, Qt::CaseInsensitive) == 0)
                        break;      // 到盘根了
                    dir = up;
                }
                slotChainCache.insert(fileDir, chain);
                return chain;
            };

            /*
             * 一个文件在哪个槽里有记录。
             * 从深到浅找【第一个真的有这条路径的】槽:深的那份缓存更新,优先信它;
             * 深层没有的时候再看浅层 —— 子目录后来新建了一份 tscch
             * (只写了它自己扫到的几张)之后,上层 tscch 里明明有的记录也还能用上。
             */
            struct RecordHit {
                int slot = -1;
                const FileEntry *entry = nullptr;
            };
            auto findCachedRecord = [&](const QString &fileDir, const QString &key) -> RecordHit {
                const QVector<int> chain = slotChainFor(fileDir);
                for (int si : chain) {
                    const auto it = cacheSlots[si].byPath.constFind(key);
                    if (it != cacheSlots[si].byPath.constEnd())
                        return RecordHit{si, &it.value()};
                }
                return RecordHit();
            };

            struct HashCandidate {
                int readSlot = -1;      // 记录来自哪个槽
                int writeSlot = -1;     // 记录最终写到哪个槽(-1 = 只进结果,不落盘)
                QString path;
                FileEntry cached;
            };
            struct RecomputeItem {
                int writeSlot = -1;
                QString path;
                qint64 size = 0;
                bool resolved = false;  // 已经被"移位复用"认领,不用再算
            };
            QVector<HashCandidate> hashCandidates;
            QVector<RecomputeItem> recomputeList;

            int jobReused = 0;
            int jobRecomputed = 0;
            int jobMoved = 0;
            /*
             * 没有归到任何缓存槽的文件(典型情况:没勾 cache 时压根没有可写的槽)。
             * 这些文件算完特征必须直接进总结果 —— 之前忘了收,导致"不勾 cache 时
             * 扫到的图片数是对的,但一个候选重复对都没有"。
             */
            QVector<FileEntry> noCacheOut;

            /*
             * 记录最终写进哪个槽:**就近** —— 沿文件所在目录往上,取第一个
             * "存在 + 可写 + 读得出来"的 tscch。
             *
             * 读和写用同一条链(见 findCachedRecord),所以:
             *   - 文件在 F/a 下、F/a 有自己的 tscch -> 写 F/a 那份;
             *   - 文件直属 F、F 有 tscch            -> 写 F 那份;
             *   - 整条链上一个可写槽都没有           -> -1,结果只进内存。
             *
             * 为什么不再"哪条记录给的就写回哪个槽":顶层 tscch 里往往留着很早
             * 以前合进去的子目录记录,按"谁给写谁"就会一直续在顶层,子目录那份
             * 永远补不齐 —— 那正是"一次 cache image features 就把所有子缓存并到
             * 顶层"的成因。
             */
            auto nearestWritableSlotFor = [&](const QString &fileDir) -> int {
                for (int si : slotChainFor(fileDir)) {
                    const CacheSlot &s = cacheSlots[si];
                    if (s.writable && !s.unreadable) return si;
                }
                return -1;
            };
            auto writeSlotFor = [&](const QString &fileDir) -> int {
                /* 注意别用 near 当变量名:windows.h 里那个 16 位时代的 near 宏会把它吃掉 */
                const int nearestSlot = nearestWritableSlotFor(fileDir);
                if (nearestSlot >= 0) return nearestSlot;
                /* 链上没有已存在的槽:退回本目录(开着 cache 时会被创建) */
                if (localSlot >= 0 && cacheSlots[localSlot].writable
                    && !cacheSlots[localSlot].unreadable)
                    return localSlot;
                return -1;
            };

            auto pushEntry = [&](int writeSlot, int recordSlot, FileEntry &&fe) {
                if (writeSlot < 0) {
                    noCacheOut.push_back(std::move(fe));
                    return;
                }
                CacheSlot &dst = cacheSlots[writeSlot];
                const QString key = QDir::cleanPath(fe.currentPath);
                /*
                 * 本槽里已经有一条一模一样的记录时不要置脏:每按一次按钮都会
                 * 把整棵树过一遍,不加这个判断就是每次都重写全部 tscch
                 * (一个几百个子目录的数据集能写几十 GB)。
                 */
                const auto existing = dst.byPath.constFind(key);
                const bool sameAsStored =
                    existing != dst.byPath.constEnd()
                    && existing.value().filesize == fe.filesize
                    && existing.value().modTime == fe.modTime
                    && std::memcmp(existing.value().SHA256, fe.SHA256, 32) == 0;
                if (!sameAsStored) dst.dirty = true;

                /*
                 * 下沉:记录原本读自更浅的槽(典型是 F 的 tscch 里那批历史残留),
                 * 现在写进了更近的一份 —— 浅层那份要删掉,否则同一张图在两份缓存里
                 * 各存一套,改一次图只更新一边,另一边永远停在旧时间戳。
                 *
                 * 只在"目标确实位于源之下、且源本身可写"时删:源不可写说明它是
                 * 别人/只读的缓存,不归本次扫描负责。
                 */
                if (recordSlot >= 0 && recordSlot != writeSlot) {
                    CacheSlot &src = cacheSlots[recordSlot];
                    if (src.writable && !src.unreadable
                        && dedup::isUnderOrEqual(dst.dir, src.dir)) {
                        src.dropPaths.insert(key);
                    }
                }
                dst.out.push_back(std::move(fe));
            };

            /*
             * 老缓存兼容(一次性):这个版本之前的 webp 质量估计是坏的(关键帧位
             * 判反 + 读错偏移),所有有损 webp 在缓存里存的是 -1,界面于是统一
             * 显示兜底的 60。命中缓存复用路径时补算一次,并把槽标记为脏;写回
             * 之后 quality 变成正数,下次就不会再进这里。
             *
             * 只读前 1MB:VP8 chunk 通常紧跟文件头;真被巨大的 EXIF/XMP 推到
             * 后面(或文件本身更大),再退回整文件读取。这些是扫描的快速路径,
             * 不能无条件 readAll。
             */
            auto upgradeWebpQuality = [&](FileEntry &fe, int writeSlot) {
                if (fe.format.compare(QLatin1String("webp"), Qt::CaseInsensitive) != 0)
                    return;
                if (fe.quality >= 0)
                    return;

                QFile qf(fe.currentPath);
                if (!qf.open(QIODevice::ReadOnly))
                    return;

                const QByteArray head = qf.read(1 << 20);
                int q = dedup::estimateImageQuality(head, fe.format);
                if (q < 0 && qf.size() > head.size()) {
                    qf.seek(0);
                    q = dedup::estimateImageQuality(qf.readAll(), fe.format);
                }
                if (q >= 0) {
                    fe.quality = q;
                    if (writeSlot >= 0)
                        cacheSlots[writeSlot].dirty = true;
                }
            };


            // ---- 第一趟:便宜判断(路径在不在缓存里、大小、修改时间) ----
            for (const QString &filePath : std::as_const(job.files)) {
                if (isCancelled()) break;

                const QFileInfo fi(filePath);
                const QString key = QDir::cleanPath(fi.absoluteFilePath());
                const qint64 size = fi.size();
                const qint64 mtime = fi.lastModified().toMSecsSinceEpoch();

                const RecordHit hit = findCachedRecord(QDir::cleanPath(fi.absolutePath()), key);
                /* 写回槽只看"文件住在哪",和从哪份缓存读到无关 —— 见 writeSlotFor */
                const int writeSlot = writeSlotFor(QDir::cleanPath(fi.absolutePath()));
                bool classified = false;

                if (hit.slot >= 0 && hit.entry && cacheRecordUsable(*hit.entry)) {
                    const FileEntry &ce = *hit.entry;
                    if (ce.filesize == size && ce.modTime == mtime) {
                        // 快速路径:大小和时间都没变,直接复用
                        FileEntry fe = ce;
                        fe.originalPath    = fi.absoluteFilePath();
                        fe.currentPath     = fe.originalPath;
                        fe.folder          = fi.absolutePath();
                        fe.filename        = fi.fileName();
                        fe.inDuplicate     = false;
                        fe.rootInputFolder = job.folder;
                        fe.folderPriority  = jobIndex;
                        fe.internalMode    = job.spec.internal;
                        fe.compareSide     = job.spec.compareSide;
                        upgradeWebpQuality(fe, writeSlot);
                        if (needThumb && fe.thumbnail.isNull())
                            fe.thumbnail = dedup::makeThumbnailFromFile(fe.currentPath);
                        pushEntry(writeSlot, hit.slot, std::move(fe));
                        if (writeSlot >= 0) ++cacheSlots[writeSlot].reused;
                        ++jobReused;
                        ++done;
                        reportProgress();
                        classified = true;
                    } else if (ce.filesize == size) {
                        // 大小一样但时间变了:需要 SHA-256 确认
                        HashCandidate hc;
                        hc.readSlot  = hit.slot;
                        hc.writeSlot = writeSlot;
                        hc.path = key;
                        hc.cached = ce;
                        hashCandidates.push_back(std::move(hc));
                        classified = true;
                    }
                }

                if (!classified)
                    recomputeList.push_back(RecomputeItem{writeSlot, filePath, size, false});
            }

            /*
             * ---- 第二趟之一:识别"移位/改名"(缓存里丢的记录 <-> 本次新增的文件) ----
             *
             * 缓存里还记着、但磁盘上已经没有这个路径的记录 = "丢失";
             * 本次扫到、但缓存里没有这个路径的文件 = "新增"。
             * 两批文件 SHA-256 相同 = 同一个文件只是换了位置,特征(pHash/dHash/ORB)
             * 完全可以直接搬过来 —— 一张 32MP 的图解码就要几百毫秒,
             * 搬一条记录是零成本。
             *
             * 三条约束把风险压住:
             *   1) 只认【唯一】的 SHA:同一个 SHA 在缓存里出现多次(重复文件)时
             *      一律不做移位复用。这种情况更可能是用户去重时删掉了其中一份,
             *      拿它的记录去认领新文件,等于凭空造出一条"这个文件早就缓存过"的
             *      假记录 —— 用户明确要求按"删了重复文件"处理;
             *   2) 只给"字节数对得上"的记录做存在性检查:移位不会改文件大小,
             *      这一条能挡掉绝大多数无关记录,否则上层 tscch 几万条记录
             *      要 stat 几万次;
             *   3) 只给"字节数对得上丢失记录"的新文件算 SHA:缓存里一条都没丢时
             *      完全不产生额外 I/O —— 常规的重复扫描(没有增删)一次也不多读。
             */
            struct MovedSource {
                int slot = -1;
                QString path;
                const FileEntry *rec = nullptr;
            };
            QHash<QByteArray, MovedSource> movedSources;

            if (!recomputeList.isEmpty() && !isCancelled()) {
                QSet<qint64> newSizes;
                for (const RecomputeItem &rc : std::as_const(recomputeList))
                    newSizes.insert(rc.size);

                struct ShaGroup {
                    int slot = -1;
                    QString path;
                    const FileEntry *rec = nullptr;
                    bool multiPath = false;   // 同一 SHA 对应多条不同路径 = 有重复文件
                };
                QHash<QByteArray, ShaGroup> shaGroups;
                for (int si = 0; si < cacheSlots.size(); ++si) {
                    const CacheSlot &sl = cacheSlots[si];
                    if (!sl.exists) continue;
                    for (auto it = sl.byPath.constBegin(); it != sl.byPath.constEnd(); ++it) {
                        const FileEntry &fe = it.value();
                        if (!cacheRecordUsable(fe)) continue;
                        // 大小和任何一个新增文件都对不上 -> 不可能是它变的
                        if (!newSizes.contains(fe.filesize)) continue;
                        const QByteArray sha(reinterpret_cast<const char *>(fe.SHA256), 32);
                        auto g = shaGroups.find(sha);
                        if (g == shaGroups.end()) {
                            ShaGroup grp;
                            grp.slot = si;
                            grp.path = it.key();
                            grp.rec  = &fe;
                            shaGroups.insert(sha, grp);
                        } else if (g->path.compare(it.key(), Qt::CaseInsensitive) != 0) {
                            g->multiPath = true;
                        }
                    }
                }

                QSet<qint64> wantedSizes;
                for (auto it = shaGroups.constBegin(); it != shaGroups.constEnd(); ++it) {
                    const ShaGroup &g = it.value();
                    if (g.multiPath || !g.rec) continue;        // 有重复内容,不认领
                    // 路径还在(只是这次可能被正则筛掉/没扫到)-> 不是"丢失"
                    if (QFileInfo::exists(g.path)) continue;
                    MovedSource src;
                    src.slot = g.slot;
                    src.path = g.path;
                    src.rec  = g.rec;
                    movedSources.insert(it.key(), src);
                    wantedSizes.insert(g.rec->filesize);
                }

                if (!movedSources.isEmpty()) {
                    QVector<int> idx;
                    QVector<QString> paths;
                    for (int i = 0; i < recomputeList.size(); ++i) {
                        if (!recomputeList[i].resolved
                            && wantedSizes.contains(recomputeList[i].size)) {
                            idx.push_back(i);
                            paths.push_back(recomputeList[i].path);
                        }
                    }

                    const QVector<QByteArray> digests = QtConcurrent::blockingMapped(
                        paths, [](const QString &p) {
                            bool ok = false;
                            const QByteArray d = dedup::fileSHA256(p, &ok);
                            return ok ? d : QByteArray();
                        });

                    /*
                     * 两个新增文件内容一样(用户复制了一份)时同样是"重复内容":
                     * 不认领丢失的那条记录,各自重算(算出来本来也一样)。
                     */
                    QHash<QByteArray, int> newShaCount;
                    for (const QByteArray &d : std::as_const(digests))
                        if (d.size() == 32) ++newShaCount[d];

                    for (int k = 0; k < idx.size(); ++k) {
                        if (isCancelled()) break;
                        const QByteArray &digest = digests[k];
                        if (digest.size() != 32) continue;
                        if (newShaCount.value(digest) != 1) continue;
                        const auto srcIt = movedSources.constFind(digest);
                        if (srcIt == movedSources.constEnd()) continue;

                        const MovedSource &src = srcIt.value();
                        RecomputeItem &item = recomputeList[idx[k]];
                        const QFileInfo fi(item.path);

                        // 内容一模一样 -> 特征直接搬,连解码都省了
                        FileEntry fe = *src.rec;
                        std::memcpy(fe.SHA256, digest.constData(), 32);
                        fe.originalPath    = fi.absoluteFilePath();
                        fe.currentPath     = fe.originalPath;
                        fe.folder          = fi.absolutePath();
                        fe.filename        = fi.fileName();
                        fe.filesize        = fi.size();
                        fe.modTime         = fi.lastModified().toMSecsSinceEpoch();
                        fe.inDuplicate     = false;
                        fe.rootInputFolder = job.folder;
                        fe.folderPriority  = jobIndex;
                        fe.internalMode    = job.spec.internal;
                        fe.compareSide     = job.spec.compareSide;
                        upgradeWebpQuality(fe, item.writeSlot);
                        if (needThumb && fe.thumbnail.isNull())
                            fe.thumbnail = dedup::makeThumbnailFromFile(fe.currentPath);

                        pushEntry(item.writeSlot, src.slot, std::move(fe));
                        /*
                         * 旧记录一定要删:路径已经确认不存在,不删的话下次扫描
                         * 又会被当成"丢失",同一份特征被反复认领,缓存里全是僵尸记录。
                         */
                        if (src.slot >= 0 && cacheSlots[src.slot].writable) {
                            cacheSlots[src.slot].dropPaths.insert(src.path);
                            cacheSlots[src.slot].dirty = true;
                        }
                        if (item.writeSlot >= 0) {
                            cacheSlots[item.writeSlot].dirty = true;   // 路径变了,必须落盘
                            ++cacheSlots[item.writeSlot].reused;
                        }
                        item.resolved = true;
                        ++jobMoved;
                        ++jobReused;
                        ++done;
                        reportProgress();
                    }

                    if (jobMoved > 0)
                        qDebug() << "[scanFolders] 移位复用" << jobMoved
                                 << "条(丢失记录与新增文件 SHA-256 相同,已跳过重新计算)";
                }
            }

            // ---- 第二趟之二:并发计算 SHA-256(大小没变、修改时间变了的) ----
            if (!hashCandidates.isEmpty() && !isCancelled()) {
                hashCheckedCount = hashCandidates.size();
                QElapsedTimer shaTimer;
                shaTimer.start();
                QVector<QString> paths;
                paths.reserve(hashCandidates.size());
                for (const HashCandidate &hc : std::as_const(hashCandidates))
                    paths.push_back(hc.path);

                const QVector<QByteArray> digests = QtConcurrent::blockingMapped(
                    paths, [](const QString &p) {
                        bool ok = false;
                        const QByteArray d = dedup::fileSHA256(p, &ok);
                        return ok ? d : QByteArray();
                    });
                if (shaTimer.elapsed() > 300) {
                    if (logger) logger(QStringLiteral("[%1] %2: %3 个文件大小没变但时间戳变了,"
                                                   "补算 SHA-256 用了 %4 ms")
                                        .arg(QDateTime::currentDateTime().toString("HH:mm:ss"))
                                        .arg(QDir::toNativeSeparators(job.folder))
                                        .arg(hashCandidates.size())
                                        .arg(shaTimer.elapsed()));
                }

                for (int k = 0; k < hashCandidates.size(); ++k) {
                    if (isCancelled()) break;

                    HashCandidate &hc = hashCandidates[k];
                    const QByteArray &digest = digests[k];

                    /*
                     * 能走到这里说明缓存记录的 SHA256 一定是非全0的
                     * (cacheRecordUsable 已经把全0的挡掉了),所以这里只比"是否相同"。
                     */
                    const bool sameContent = (digest.size() == 32) &&
                                             (std::memcmp(hc.cached.SHA256,
                                                          digest.constData(), 32) == 0);

                    if (sameContent) {
                        // 内容没变,只是时间戳变了 -> 复用,并写回新的 modTime
                        const QFileInfo fi(hc.path);
                        FileEntry fe = hc.cached;
                        std::memcpy(fe.SHA256, digest.constData(), 32);
                        fe.originalPath    = fi.absoluteFilePath();
                        fe.currentPath     = fe.originalPath;
                        fe.folder          = fi.absolutePath();
                        fe.filename        = fi.fileName();
                        fe.filesize        = fi.size();
                        fe.modTime         = fi.lastModified().toMSecsSinceEpoch();
                        fe.inDuplicate     = false;
                        fe.rootInputFolder = job.folder;
                        fe.folderPriority  = jobIndex;
                        fe.internalMode    = job.spec.internal;
                        fe.compareSide     = job.spec.compareSide;
                        upgradeWebpQuality(fe, hc.writeSlot);
                        if (needThumb && fe.thumbnail.isNull())
                            fe.thumbnail = dedup::makeThumbnailFromFile(fe.currentPath);
                        pushEntry(hc.writeSlot, hc.readSlot, std::move(fe));
                        if (hc.writeSlot >= 0) {
                            ++cacheSlots[hc.writeSlot].reused;
                            cacheSlots[hc.writeSlot].dirty = true;   // modTime/SHA256 有更新
                        }
                        ++jobReused;
                        // 只有"复用"才在这里计数;内容变了的交给下面重算那趟统一计数,
                        // 否则同一个文件会被算两次(进度条会提前冲到100%)
                        ++done;
                        reportProgress();
                    } else {
                        // 内容真的变了 -> 重新计算特征
                        recomputeList.push_back(
                            RecomputeItem{hc.writeSlot, hc.path, hc.cached.filesize, false});
                    }
                }
            }

            // ---- 第二趟之三:重算(新增 / 内容变化 / 缓存里没有 / 原来就没有缓存) ----
            for (int ri = 0; ri < recomputeList.size(); ++ri) {
                if (isCancelled()) break;

                const RecomputeItem &rc = recomputeList[ri];
                if (rc.resolved) continue;   // 已经被"移位复用"认领

                const int writeSlot = rc.writeSlot;
                const QString &filePath = rc.path;

                FileEntry fresh;
                QElapsedTimer computeTimer;
                computeTimer.start();
                const bool computed = computeFileFeatures(fresh, filePath, jobOptions);
                {
                    /*
                     * 只记"最慢的那一个":日志窗口很窄,逐文件打印会刷屏;
                     * 用户报的"最后几个文件卡一下"只要能指出具体文件就够了。
                     */
                    const qint64 ms = computeTimer.elapsed();
                    if (ms > slowestComputeMs) {
                        slowestComputeMs = ms;
                        slowestComputeFile = filePath;
                    }
                }
                if (!computed) {
                    ++done;
                    reportProgress();
                    continue;
                }
                fresh.rootInputFolder = job.folder;
                fresh.folderPriority  = jobIndex;
                fresh.internalMode    = job.spec.internal;
                fresh.compareSide     = job.spec.compareSide;
                if (needThumb && fresh.thumbnail.isNull())
                    fresh.thumbnail = dedup::makeThumbnailFromFile(fresh.currentPath);

                if (writeSlot >= 0) {
                    CacheSlot &slot = cacheSlots[writeSlot];
                    /*
                     * 先追加再 move:这一张算完就已经进 .part 的缓冲了,
                     * 之后进程崩掉 / 用户取消,下次都不用重算(最多丢当前这 128 张的一批)。
                     */
                    if (slot.part && slot.part->append(fresh))
                        ++pendingPartAppends;

                    slot.out.push_back(std::move(fresh));
                    ++slot.recomputed;
                    slot.dirty = true;
                } else {
                    // 没有缓存槽(比如没勾 cache):结果直接进总结果
                    noCacheOut.push_back(std::move(fresh));
                }
                ++jobRecomputed;
                ++done;
                reportProgress();

                /* 每 128 张落一次盘(用户指定的阈值) */
                if (pendingPartAppends >= kFlushEveryImages) {
                    pendingPartAppends = 0;
                    flushPartialCaches(done);
                }
            }

            // ---- 写回 ----
            if (isCancelled()) {
                /*
                 * 取消 / 关窗:
                 *   · 绝不把"半截结果"当成完整缓存写下去(不动 savedAt、不剔除记录);
                 *   · 但已经算好的特征必须落到【目标目录】的 image_features.tscch
                 *     (mergeOnly 部分更新)—— 用户明确要求:中途取消、导入整理这类
                 *     不完全扫描,算出来的特征要进目标缓存,不能只躺在 temp 里;
                 *   · temp 里的 .part 只是"防崩溃检查点",既然已经并进目标缓存,
                 *     就删掉,免得下次再并一遍。
                 */
                flushPartialCaches(done);

                int committed = 0;
                for (CacheSlot &s : cacheSlots) {
                    if (!s.part) continue;
                    s.part->close();

                    const QString tscchPath = cacheio::cacheFilePathForFolder(s.dir);
                    QString err;
                    const int merged = cacheio::mergePartialCacheLog(tscchPath, &err);
                    if (merged < 0) {
                        qWarning() << "[scanFolders] commit partial cache on cancel failed:"
                                   << s.dir << err;   // 失败则 .part 保留,temp 里还有一份
                    } else {
                        committed += merged;
                    }
                }

                /*
                 * 用户取消了就是不想再算了:temp 里一点痕迹都别留 ——
                 * 不写"未完成"标记(下次启动不会再来问"要不要继续"),.part 也已经
                 * 并进目标缓存后删掉了。已经算好的特征留在目标目录的 tscch 里,
                 * 不浪费,也不会重算;剩下的图等用户下次自己决定要不要再扫。
                 */
                cacheio::clearScanJournal();

                qDebug() << "[scanFolders] cancelled:" << committed
                         << "computed record(s) committed into target cache for" << job.folder;
                break;
            }
            /*
             * 写回顺序:深的先写。
             *
             * 下沉是"目标槽写入 + 源槽 drop"两件事。若先写浅层(源),一旦深层
             * 写失败,源里的记录已经被删掉,这批特征只能下次重算;先写深层,
             * 失败时源里还留着一份旧副本,下次扫描继续收敛,永远不丢。
             */
            QVector<int> writeOrder;
            writeOrder.reserve(cacheSlots.size());
            for (int si = 0; si < cacheSlots.size(); ++si) {
                // 只读复用源 / 读不出来的一律不动(写回去会整片覆盖别人的记录)
                if (!cacheSlots[si].writable || cacheSlots[si].unreadable) continue;
                writeOrder.push_back(si);
            }
            std::stable_sort(writeOrder.begin(), writeOrder.end(), [&](int a, int b) {
                return cacheSlots[a].dir.count(QLatin1Char('/'))
                       > cacheSlots[b].dir.count(QLatin1Char('/'));
            });

            /*
             * 历史重复收敛(纯去重,零信息损失)。
             *
             * 旧版权威模式把子目录记录全并进了 F,同一张图于是 F 和子目录各存一份。
             * 这里对每个可写槽扫一遍它读到的旧记录:如果这条记录"本来就该归更近的
             * 一份 tscch 管",而更近那份里确实有同路径、且可用的记录,就把浅层这条删掉。
             *
             * 不满足条件的一律保留(更近那份没有 / 记录是坏的 / 磁盘上那条已不存在),
             * 等下次扫描真正命中该文件时再搬 —— 绝不做"先删了再说"。
             */
            for (int si : writeOrder) {
                CacheSlot &s = cacheSlots[si];
                if (s.byPath.isEmpty()) continue;
                /*
                 * 只收敛"权威写入"的槽:上级目录(mergeOnly)和带筛选的扫描
                 * 只覆盖了作用域的一部分,在那里删记录风险大于收益,保持原样。
                 */
                if (s.mergeOnly) continue;
                for (auto it = s.byPath.constBegin(); it != s.byPath.constEnd(); ++it) {
                    const QString &p = it.key();
                    const int nearestSlot = nearestWritableSlotFor(
                        QDir::cleanPath(QDir::toNativeSeparators(QFileInfo(p).absolutePath())));
                    if (nearestSlot < 0 || nearestSlot == si) continue;
                    const auto hit = cacheSlots[nearestSlot].byPath.constFind(p);
                    if (hit == cacheSlots[nearestSlot].byPath.constEnd()) continue;
                    if (!cacheRecordUsable(hit.value())) continue;
                    /*
                     * 更近那份比本槽还旧时不动:那说明本槽才是较新的副本
                     * (典型是"上一次只更新了顶层"),删了就退回到旧特征。
                     * 读取时按"深到浅"命中,这种旧记录本来也不会被用到,
                     * 等下一次真正扫到这张图再把它更新掉。
                     */
                    if (hit.value().modTime < it.value().modTime) continue;
                    s.dropPaths.insert(p);
                }
            }

            for (int si : writeOrder) {
                CacheSlot &s = cacheSlots[si];

                /*
                 * 不要凭空造一个空缓存:本目录的 tscch 还不存在、而且这一轮没有任何
                 * 记录落进来(文件全都被上一层/子目录的缓存接管了)时,就不创建它。
                 * 这正是"扫描 ./b/ 只更新上一层 ./image_features.tscch,
                 * 不在 ./b/ 里另建一个"的行为。
                 */
                if (!s.exists && s.out.isEmpty() && !s.dirty) continue;

                bool needWrite = s.dirty || !s.exists || !s.dropPaths.isEmpty();
                if (!needWrite && !s.mergeOnly) {
                    // 权威写入:还要检查有没有"磁盘上已经不存在"的旧记录需要剔除
                    for (auto it = s.byPath.constBegin(); it != s.byPath.constEnd(); ++it) {
                        if (!QFileInfo::exists(it.key())) { needWrite = true; break; }
                    }
                }
                if (!needWrite && s.mergeOnly && !job.hasRegex && !job.hasFileFilter) {
                    /*
                     * 只合并 + 既没有正则筛选也没有排除谓词:本次真的扫过了
                     * job.folder 这棵子树,所以可以在这棵子树里清理"文件已经不在磁盘上"
                     * 的记录。(s.dirty 已经覆盖了"有新记录被重算"的情况;
                     *  有筛选时一律不清理,也不为了让 savedAt 变化而写。)
                     */
                    for (auto it = s.byPath.constBegin(); it != s.byPath.constEnd(); ++it) {
                        if (dedup::isUnderOrEqual(it.key(), job.folder)
                            && !QFileInfo::exists(it.key())) { needWrite = true; break; }
                    }
                }
                if (!needWrite) continue;

                cacheio::WriteOptions wo;
                wo.mergeOnly        = s.mergeOnly;
                wo.touchSubDir      = s.touchSubDir;
                wo.touchSubDirMtime = s.touchSubDirMtime;
                wo.dropPaths        = s.dropPaths;   // 被移位消费掉的旧路径:无条件删
                // 只合并时(且没有正则筛选/排除谓词),允许在"本次扫过的子树"里清理已不存在的记录
                if (s.mergeOnly && !job.hasRegex && !job.hasFileFilter)
                    wo.pruneWithin = job.folder;

                QString err;
                const QString tscchPath = cacheio::cacheFilePathForFolder(s.dir);
                /*
                 * 估算"重写这份 tscch 要多少空间":现有文件 + 本次新记录,
                 * 再加 1MB 余量。QSaveFile 是写完临时文件再 rename,
                 * 峰值需要接近"新旧两份之和"。
                 */
                const qint64 needBytes = QFileInfo(tscchPath).size()
                                         + qint64(s.out.size()) * 20 * 1024
                                         + (1 << 20);
                QElapsedTimer writeTimer;
                writeTimer.start();
                int written = cacheio::writeFileEntryCache(tscchPath, s.out, wo, &err);

                /*
                 * 磁盘满时把这次写回交给调用方等空间。
                 * QSaveFile 保证失败时原 tscch 不动,所以重试是安全的;
                 * 每次重试都重新写整份,最多 8 次(和空间门的循环上限一个量级)。
                 */
                for (int attempt = 0; written < 0 && options.onCacheWriteFailed && attempt < 8;
                     ++attempt) {
                    if (!options.onCacheWriteFailed(s.dir, needBytes, err))
                        break;
                    err.clear();
                    written = cacheio::writeFileEntryCache(tscchPath, s.out, wo, &err);
                }
                const qint64 thisWriteMs = writeTimer.elapsed();
                cacheWriteMs += thisWriteMs;
                cacheWrittenCount += qMax(0, written);
                if (thisWriteMs >= 300) {
                    /*
                     * 写回是"整份读出 + 合并 + 原子重写"(QSaveFile)。
                     * 33MB 的 tscch(1042 张图 × 800 个 ORB 描述子)在机械盘上
                     * 一两秒很正常 —— 这段日志就是给"最后卡一下"一个明确出处。
                     */
                    if (logger) logger(QStringLiteral("[%1] 写回缓存 %2: %3 条,耗时 %4 ms"
                                                   "(文件约 %5 MB,mergeOnly=%6)")
                                        .arg(QDateTime::currentDateTime().toString("HH:mm:ss"))
                                        .arg(QDir::toNativeSeparators(tscchPath))
                                        .arg(qMax(0, written))
                                        .arg(thisWriteMs)
                                        .arg(QFileInfo(tscchPath).size() / (1024 * 1024))
                                        .arg(s.mergeOnly ? 1 : 0));
                }

                if (written < 0) {
                    s.writeFailed = true;
                    qWarning() << "[scanFolders] write cache failed:" << s.dir << err;
                } else {
                    qDebug() << "[scanFolders] wrote cache" << s.dir << written
                             << "(mergeOnly" << s.mergeOnly
                             << ", reused" << s.reused
                             << ", recomputed" << s.recomputed << ")";
                }
            }

            /*
             * 写回结束:.part 里的内容要么已经在 tscch 里(删掉),
             * 要么这次写失败了 / 被跳过(保留,下次扫描开头自动并回去)。
             */
            for (CacheSlot &s : cacheSlots) {
                if (!s.part) continue;
                s.part->close();
                if (s.writeFailed) continue;
                QFile::remove(cacheio::partialCachePathFor(
                    cacheio::cacheFilePathForFolder(s.dir)));
            }

            /*
             * 每个目录一行"扫描结算":这是排查"扫到最后卡一下"的直接证据。
             *   写回 = 0 条  -> 这次没有任何东西要落盘,收尾不慢;
             *   写回 > 0 条  -> 看耗时;33MB 的 tscch 整份原子重写,
             *                   机械盘上就是会有一两秒的停顿。
             *
             * 只在"有东西值得看"时输出,否则扫一棵几百个目录的树会刷屏:
             * 有重算/移位、有写回、或者某一项超过 300ms。
             */
            if (jobRecomputed > 0 || jobMoved > 0 || cacheWrittenCount > 0
                || cacheWriteMs >= 300 || slowestComputeMs >= 300) {
                QString slowInfo;
                if (slowestComputeMs >= 300 && !slowestComputeFile.isEmpty()) {
                    slowInfo = QStringLiteral(";最慢的单张 %1(%2 ms)")
                                   .arg(QDir::toNativeSeparators(slowestComputeFile))
                                   .arg(slowestComputeMs);
                }
                if (logger) logger(QStringLiteral("[%1] 扫描 %2: %3 个文件 —— 复用 %4 / 移位 %5 / "
                                                  "补算SHA %6 / 重算 %7;缓存写回 %8 条 %9 ms;"
                                                  "总耗时 %10 ms%11")
                                       .arg(QDateTime::currentDateTime().toString("HH:mm:ss"))
                                       .arg(QDir::toNativeSeparators(job.folder))
                                       .arg(job.files.size())
                                       .arg(jobReused).arg(jobMoved)
                                       .arg(hashCheckedCount).arg(jobRecomputed)
                                       .arg(cacheWrittenCount).arg(cacheWriteMs)
                                       .arg(jobTimer.elapsed())
                                       .arg(slowInfo));
            }

            // 结果汇总(把所有槽的产出 + 没进槽的都收进总结果)
            for (CacheSlot &s : cacheSlots) {
                for (FileEntry &fe : s.out)
                    results.push_back(std::move(fe));
            }
            for (FileEntry &fe : noCacheOut)
                results.push_back(std::move(fe));

            stats.reused     += jobReused;
            stats.recomputed += jobRecomputed;
            stats.moved      += jobMoved;

            if (jobRecomputed > 0 || jobMoved > 0)
                qDebug() << "[scanFolders]" << job.folder
                         << "reused" << jobReused << "recomputed" << jobRecomputed
                         << "moved" << jobMoved;
        }

        stats.total = stats.reused + stats.recomputed;
        /*
         * 全部 job 正常结束 -> 清掉"上次没跑完"的标记(下次启动不再提示)。
         * 被取消时不清:那正是要给用户提示"还能接着算"的场景。
         */
        if (!isCancelled())
            cacheio::clearScanJournal();
        qDebug() << "[scanFolders] total" << stats.total
                 << "reused" << stats.reused << "recomputed" << stats.recomputed
                 << "moved" << stats.moved;
        if (outStats) *outStats = stats;

        // 收尾:确保进度条走到 100%
        if (progress) {
            const int total = totalCount > 0 ? totalCount : done;
            progress(total, total);
        }

        return results;
    }

    /*
     * “cache image features”按钮的后端:扫描 path 下的所有图片、计算全部特征,
     * 按【就近写回】把结果落盘 —— 子目录里已经有 image_features.tscch 的,
     * 该子目录的图片就写进那一份;只有直属文件(以及更深层没有自己缓存的目录)
     * 才写 <path>/image_features.tscch。
     *
     * 返回写入的条目数(小于0表示参数/目录错误)。
     */
    static int cacheImageFeatures(const QString &path,
                                  bool doThumbnail = true,
                                  bool doSHA256 = true,
                                  const std::function<void(int,int)> &progress = {},
                                  const std::function<bool()> &cancelled = {},
                                  QString *outError = nullptr,
                                  CacheUpdateStats *outStats = nullptr);

    /*
     * 缓存更新(给训练集管理窗口/去重窗口用):
     *   - 目录下已有 image_features.tscch 时,不重扫整棵树:
     *       路径在缓存里且大小/修改时间没变   -> 直接复用;
     *       大小或修改时间变了               -> 用SHA-256确认:相同则复用,不同则重算;
     *       缓存里有但目录里已不存在          -> 写回时自动剔除;
     *       目录里新增的文件                  -> 计算特征后加入。
     *   - 更新后的缓存会写回磁盘。
     * 返回更新后的完整条目列表(内存里就是这份数据,不需要再读一遍文件)。
     */
    static QVector<FileEntry> updateFileEntryCache(const QString &path,
                                                   bool doThumbnail = true,
                                                   bool doSHA256 = true,
                                                   const std::function<void(int,int)> &progress = {},
                                                   const std::function<bool()> &cancelled = {},
                                                   QString *outError = nullptr,
                                                   CacheUpdateStats *outStats = nullptr);

signals:
    void scanProgress(int done, int total);
    void scanFinished(QVector<FileEntry> files,int epoch);
    void logMessage(const QString &msg);

private:
    std::atomic<bool> m_cancel{false};
    std::thread m_thread;
    /* QStringList 形式的输入不带 cache 信息,用这个兜底 */
    bool m_cacheEnabled = false;
};

#endif // DEDUPWORKER_H
