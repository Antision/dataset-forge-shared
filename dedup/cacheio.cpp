#include "cacheio.h"
#include "dedupenv.h"

#include <QDataStream>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QFileInfo>

using namespace cacheio;

namespace {

constexpr quint32 kMagic     = 0x54534331; // 'TSC1'
/*
 * v4: ORB 改成"长边1024 / 800特征 / 带关键点坐标",并额外保存关键点。
 * 判据也随之从"描述子命中率"换成"几何校验内点数",特征必须重算,
 * 所以 v1~v3 的记录一律不认(读到版本不符就当空缓存,由扫描重写)。
 *
 * v5(已废弃):曾在每条记录末尾多写两个 double(visualDetail / noise)。
 * 实测存量缓存里 792/792 条都是 -1 —— 扫描阶段从不计算这两个值,侧栏现算的
 * 结果也不回写,等于纯 16 字节死重量。2026-11-20 整体摘除:写回固定 v4,
 * 读到时只跳过那 16 字节,所以不需要重扫存量 v5 缓存。
 */
constexpr quint16 kVersion   = 4;
/*
 * 能读的最旧版本。v4 与 v5 的 ORB 特征完全一致,差的只有那段废弃尾段,
 * 所以 v5 仍然是"可读"的 —— 否则升级过的目录全部要重扫一遍。
 */
constexpr quint16 kOldestReadableVersion = 4;
/* 记录尾部带 16 字节废弃尾段的版本上限(只跳过,不解析) */
constexpr quint16 kSkipTailVersion = 5;

/* 部分结果日志 */
constexpr quint32 kPartMagic   = 0x54535031; // 'TSP1'
constexpr quint16 kPartVersion = 3;          // v3: payload 与 tscch v4 同构
/* 单条 payload 的上限:只是防御坏文件,正常一条记录不超过几百 KB */
constexpr quint32 kMaxPartPayload = 128u * 1024u * 1024u;

/*
 * 临时缓存目录(部分结果 + 未完成标记)。
 * 主窗口会用设置里的 temp 路径调 setWorkCacheDir();没调过就用 <程序目录>/cache/scan。
 * 只由 UI 线程在启动/改设置时写一次,扫描线程只读 —— 用 mutex 兜住这两个时机重叠的情况。
 */
QString        g_workCacheDir;
QMutex         g_workCacheDirMutex;

QString resolveWorkCacheDir(const QString &preferredTempDir)
{
    return dedupenv::cacheSubDir(preferredTempDir, QStringLiteral("scan"));
}

inline QString normalizedRelativePath(const QString &rootFolder, const QString &filePath)
{
    QDir root(rootFolder);
    QString rel = root.relativeFilePath(filePath);
    rel.replace('\\', '/');
    rel = QDir::cleanPath(rel);
    if (rel.isEmpty() || rel == QLatin1String("."))
        rel = QFileInfo(filePath).fileName();
    if (!rel.startsWith(QLatin1String("./")) && !rel.startsWith(QLatin1String("../")))
        rel.prepend(QLatin1String("./"));
    return rel;
}

inline QString resolveRelativePath(const QString &rootFolder, const QString &rel)
{
    QString cleaned = rel;
    cleaned.replace('\\', '/');
    while (cleaned.startsWith(QLatin1String("./")))
        cleaned.remove(0, 2);
    if (cleaned.isEmpty())
        return QString();
    return QDir::cleanPath(QDir(rootFolder).absoluteFilePath(cleaned));
}

/* 空矩阵写 0/0/0;否则写 rows/cols/type + 逐行原始字节 */
bool writeMatBlock(QDataStream &out, const cv::Mat &m)
{
    if (m.empty()) {
        out << static_cast<quint32>(0) << static_cast<quint32>(0)
            << static_cast<quint32>(0);
        return true;
    }
    const quint32 rows = static_cast<quint32>(m.rows);
    const quint32 cols = static_cast<quint32>(m.cols);
    const quint32 type = static_cast<quint32>(m.type());
    out << rows << cols << type;

    /*
     * 不能用 m.isContinuous() 就直接整块写:cv::Mat 可能是别人切出来的 ROI。
     * 逐行写永远正确,而且和下面的逐行读严格对称。
     */
    const int rowBytes = m.cols * static_cast<int>(m.elemSize());
    for (quint32 r = 0; r < rows; ++r) {
        const uchar *rowPtr = m.ptr<uchar>(static_cast<int>(r));
        if (out.writeRawData(reinterpret_cast<const char*>(rowPtr), rowBytes) != rowBytes)
            return false;
    }
    return true;
}

/* 与 writeMatBlock 对称。故意不信任文件里的尺寸,先做上限检查再分配。 */
bool readMatBlock(QDataStream &in, cv::Mat &m)
{
    quint32 rows = 0, cols = 0, type = 0;
    in >> rows >> cols >> type;
    if (in.status() != QDataStream::Ok) return false;

    constexpr quint32 kMaxDim = 100000u;
    if (rows == 0 || cols == 0) { m = cv::Mat(); return true; }
    if (rows > kMaxDim || cols > kMaxDim || type > 0xFFFFu) return false;

    cv::Mat out(static_cast<int>(rows), static_cast<int>(cols), static_cast<int>(type));
    if (out.empty()) return false;

    const int rowBytes = out.cols * static_cast<int>(out.elemSize());
    for (quint32 r = 0; r < rows; ++r) {
        const int got = in.readRawData(reinterpret_cast<char*>(out.ptr<uchar>(static_cast<int>(r))),
                                       rowBytes);
        if (got != rowBytes) return false;
    }
    m = out;
    return true;
}

bool writeRecord(QDataStream &out, const FileEntry &fe, const QString &rootFolder)
{
    out << normalizedRelativePath(rootFolder, fe.currentPath)
        << static_cast<quint64>(fe.filesize)
        << static_cast<qint64>(fe.modTime)
        << static_cast<quint32>(std::max(0, fe.size.width()))
        << static_cast<quint32>(std::max(0, fe.size.height()))
        << fe.format
        << static_cast<qint32>(fe.quality)
        << static_cast<quint64>(fe.pHash)
        << static_cast<quint64>(fe.dHash)
        << static_cast<qint32>(fe.orbKeypoints);
    out.writeRawData(fe.SHA256, 32);

    // ===== ORB 描述子 + 关键点坐标(几何校验要用,必须成对保存) =====
    if (!writeMatBlock(out, fe.orbDescriptors))
        return false;
    if (!writeMatBlock(out, fe.orbPoints))
        return false;

    /* v5 的记录尾部还有 16 字节(两个 double,恒为 -1),写的时候不再产出 */
    return out.status() == QDataStream::Ok;
}

/*
 * 跳过 v5 记录尾部那 16 字节废弃数据。
 *
 * 这是整个工程里**唯一**一处 tscch 版本兼容分支,存在理由只有一个:
 * 别让已经升级到 v5 的目录因为退回 v4 而整份作废重扫。
 * 读到 v4 记录时没有这一段,直接返回。
 */
bool skipRecordTail(QDataStream &in, quint16 version)
{
    if (version < kSkipTailVersion)
        return true;
    return in.skipRawData(16) == 16;
}

bool readRecord(QDataStream &in, const QString &rootFolder, FileEntry &fe, quint16 version)
{
    QString rel;
    quint64 filesize = 0;
    qint64 modTime = 0;
    quint32 w = 0, h = 0;
    QString format;
    qint32 quality = 0;
    quint64 pHash = 0, dHash = 0;
    qint32 orbKeypoints = 0;
    char sha[32]{};

    in >> rel >> filesize >> modTime >> w >> h >> format >> quality >> pHash >> dHash >> orbKeypoints;
    if (in.status() != QDataStream::Ok) return false;
    if (in.readRawData(sha, 32) != 32) return false;

    cv::Mat descriptors, points;
    if (!readMatBlock(in, descriptors)) return false;
    if (!readMatBlock(in, points)) return false;
    if (!skipRecordTail(in, version)) return false;

    // 类型不对就当没有:后面 verifyOrbPair 会据此拒绝,不会拿错数据去匹配
    if (!descriptors.empty() && descriptors.type() != CV_8U) descriptors = cv::Mat();
    if (!points.empty() && points.type() != CV_32F) points = cv::Mat();
    // 行数对不上说明记录坏了(或版本不该进来),坐标与描述子无法一一对应,一并丢掉
    if (!descriptors.empty() && !points.empty() && points.rows != descriptors.rows) {
        descriptors = cv::Mat();
        points = cv::Mat();
    }

    fe = FileEntry();
    fe.currentPath    = resolveRelativePath(rootFolder, rel);
    if (fe.currentPath.isEmpty()) return false;
    fe.originalPath   = fe.currentPath;
    fe.rootInputFolder = rootFolder;
    fe.filesize       = filesize;
    fe.modTime        = modTime;
    fe.size           = QSize(static_cast<int>(std::min<quint32>(w, 1000000u)),
                              static_cast<int>(std::min<quint32>(h, 1000000u)));
    fe.format         = format;
    fe.quality        = static_cast<int>(quality);
    fe.pHash          = pHash;
    fe.dHash          = dHash;
    fe.orbKeypoints   = static_cast<int>(orbKeypoints);
    fe.orbDescriptors = descriptors;
    fe.orbPoints      = points;
    std::memcpy(fe.SHA256, sha, sizeof(fe.SHA256));
    fe.filename = QFileInfo(fe.currentPath).fileName();
    fe.folder   = QFileInfo(fe.currentPath).absolutePath();
    fe.inDuplicate = false;
    return true;
}

/* 把条目按“绝对路径 -> FileEntry”收进map。
 * 只接受【位于该 tscch 所在目录之下】的记录:这正是"不要污染别人的缓存"这个约束,
 * 而且比原来按 rootInputFolder 判断更准确 —— 扫描 ./b/ 时回写上一层的 tscch,
 * 那些条目的 rootInputFolder 是 ./b/ 而不是上层目录,但路径确实在上层目录之下,应当保留。 */
void collectCacheable(const QVector<FileEntry> &entries, const QString &folder,
                      QHash<QString, FileEntry> &inout)
{
    for (const FileEntry &fe : entries) {
        if (fe.currentPath.isEmpty()) continue;
        if (!dedup::isUnderOrEqual(fe.currentPath, folder)) continue;
        FileEntry copy = fe;
        copy.currentPath = QDir::cleanPath(copy.currentPath);
        inout.insert(copy.currentPath, copy);
    }
}

} // namespace

QString cacheio::cacheFilePathForFolder(const QString &folder)
{
    return QDir(folder).filePath(kCacheFileName);
}

QVector<FileEntry> cacheio::loadFileEntryCache(const QString &tscchPath, QString *outError,
                                               CacheMeta *outMeta)
{
    QVector<FileEntry> result;
    if (outMeta) *outMeta = CacheMeta();

    const QFileInfo info(tscchPath);
    if (!info.exists()) {
        if (outError) *outError = QStringLiteral("cache file not found: %1").arg(tscchPath);
        return result;
    }
    if (outMeta) outMeta->exists = true;

    const QString rootFolder = info.absolutePath();

    QFile f(tscchPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (outError) *outError = QStringLiteral("cannot open cache file: %1").arg(tscchPath);
        return result;
    }

    QDataStream in(&f);
    in.setVersion(QDataStream::Qt_6_0);

    quint32 magic = 0;
    quint16 version = 0;
    quint32 flags = 0;
    quint64 savedAt = 0;
    quint32 count = 0;
    in >> magic >> version >> flags >> savedAt >> count;

    if (in.status() != QDataStream::Ok || magic != kMagic) {
        if (outError) *outError = QStringLiteral("bad cache header: %1").arg(tscchPath);
        return result;
    }
    /*
     * 魔法数对得上但版本超出可读范围 = 确认是我们自己的文件,可以直接覆盖
     * (readable=true 是"允许重写"的开关,不设它这份文件会被永久当成"读不出来"
     * 而再也不被覆盖)。记录本身一条都不解析 —— 别的版本的 ORB 特征与当前判据不兼容。
     *
     * 上限用 kSkipTailVersion 而不是 kVersion:v5 只多了一段已废弃的尾段,
     * 跳过即可,不必让存量缓存重扫。
     */
    if (version < kOldestReadableVersion || version > kSkipTailVersion) {
        if (outError) *outError = QStringLiteral("cache version %1 not in [%2,%3], will be rewritten: %4")
                                      .arg(version).arg(kOldestReadableVersion).arg(kSkipTailVersion).arg(tscchPath);
        if (outMeta) outMeta->savedAt = savedAt;
        if (outMeta) outMeta->readable = true;
        return result;
    }

    if (outMeta) outMeta->savedAt = savedAt;
    if (outMeta) outMeta->readable = true;   // 头部通过校验,这份文件是可以安全覆盖的

    result.reserve(static_cast<int>(std::min<quint32>(count, 1000000u)));
    QSet<QString> seen;

    for (quint32 i = 0; i < count; ++i) {
        FileEntry fe;
        if (!readRecord(in, rootFolder, fe, version)) {
            if (outError) *outError = QStringLiteral("truncated cache file (record %1/%2): %3")
                                          .arg(i).arg(count).arg(tscchPath);
            break;
        }
        if (seen.contains(fe.currentPath)) continue; // 去重(同一路径只保留第一条)
        seen.insert(fe.currentPath);
        result.push_back(std::move(fe));
    }

    // 顶层子目录的更新时间表
    if (outMeta) {
        quint32 subCount = 0;
        in >> subCount;
        if (in.status() == QDataStream::Ok && subCount < 1000000u) {
            for (quint32 i = 0; i < subCount; ++i) {
                QString name;
                quint64 ts = 0;
                in >> name >> ts;
                if (in.status() != QDataStream::Ok) break;
                outMeta->subDirUpdatedAt.insert(name, ts);
            }
        }
    }

    f.close();
    return result;
}

int cacheio::writeFileEntryCache(const QString &tscchPath,
                                 const QVector<FileEntry> &entries,
                                 const WriteOptions &options,
                                 QString *outError)
{
    if (tscchPath.isEmpty()) {
        if (outError) *outError = QStringLiteral("empty cache path");
        return -1;
    }

    const QFileInfo info(tscchPath);
    const QString rootFolder = info.absolutePath();
    if (!QDir().mkpath(rootFolder)) {
        if (outError) *outError = QStringLiteral("cannot create folder: %1").arg(rootFolder);
        return -1;
    }

    QHash<QString, FileEntry> merged;
    CacheMeta oldMeta;
    bool hadFile = false;

    // 1) 读入已有记录(连同元信息,以便保留 savedAt / 子目录时间表)
    if (info.exists()) {
        hadFile = true;
        QString err;
        const QVector<FileEntry> old = loadFileEntryCache(tscchPath, &err, &oldMeta);
        if (!oldMeta.readable) {
            /*
             * 文件在、但读不出来(被占用 / 损坏 / 魔数不对 / 版本不认识)。
             * 这时 merged 里只有本次扫描的记录,一旦写下去就等于把原有内容整片抹掉:
             *   - 只合并(正则筛选 / 上级目录部分更新)时,会删掉别人的记录;
             *   - 权威写入时,会删掉"本次重算失败"那些文件的记录。
             * 所以直接拒绝写入。
             */
            if (outError)
                *outError = QStringLiteral("已有缓存读不出来,拒绝覆盖:%1 (%2)")
                                .arg(tscchPath, err);
            return -1;
        }
        collectCacheable(old, rootFolder, merged);
        /*
         * 已有记录里SHA256为全0的(旧版本/被截断)直接丢弃,避免污染校验。
         * 只在权威写入时做:只合并的承诺是"不动别人的记录",
         * 而全0 SHA 的记录会在复用侧强制重算(见 scanFolders),最后被正常记录覆盖,不会一直是坏的。
         */
        if (!options.mergeOnly) {
            for (auto it = merged.begin(); it != merged.end(); ) {
                char zero[32]{};
                if (std::memcmp(it.value().SHA256, zero, 32) == 0)
                    it = merged.erase(it);
                else
                    ++it;
            }
        }
    }

    // 2) 新记录覆盖
    collectCacheable(entries, rootFolder, merged);

    /*
     * 2.5) 调用方明确要求删掉的记录(文件被移位、旧记录已被新位置认领)。
     *
     * 放在"新记录覆盖"【之后】:同一个路径如果这一轮又有新记录,以新记录为准。
     * 这一步不受 mergeOnly 影响 —— 调用方是 stat 过、确认路径已经不存在的,
     * 不是"本次没扫到"。不删的话,僵尸记录会一直留在缓存里,
     * 下次扫描又会被当成"丢失"重新认领一遍。
     */
    for (const QString &p : options.dropPaths)
        merged.remove(QDir::cleanPath(p));

    // 3) 剔除磁盘上已经不存在的文件。
    //    - 权威写入:整个作用域都是本次扫过的,所有"文件已不存在"的记录都清掉;
    //    - 只合并:默认一个都不动(按正则筛选时,"没扫到"绝不等于"不存在");
    //      但如果给了 pruneWithin,就只在这个子树里清理确认已不存在的记录。
    if (!options.mergeOnly) {
        for (auto it = merged.begin(); it != merged.end(); ) {
            if (!QFileInfo::exists(it.key()))
                it = merged.erase(it);
            else
                ++it;
        }
    } else if (!options.pruneWithin.isEmpty()) {
        for (auto it = merged.begin(); it != merged.end(); ) {
            if (dedup::isUnderOrEqual(it.key(), options.pruneWithin)
                && !QFileInfo::exists(it.key()))
                it = merged.erase(it);
            else
                ++it;
        }
    }

    // 4) 时间戳
    CacheMeta newMeta;
    newMeta.exists = true;
    if (options.mergeOnly) {
        // 部分更新:整体时间保持不变;只更新"我负责的那一块"
        newMeta.savedAt = hadFile ? oldMeta.savedAt : 0;
        newMeta.subDirUpdatedAt = oldMeta.subDirUpdatedAt;
    } else {
        newMeta.savedAt = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());
        newMeta.subDirUpdatedAt = oldMeta.subDirUpdatedAt;
    }
    if (!options.touchSubDir.isNull()) {
        newMeta.subDirUpdatedAt.insert(options.touchSubDir, options.touchSubDirMtime);
    }

    // 5) 稳定排序后写出
    QStringList keys = merged.keys();
    std::sort(keys.begin(), keys.end());

    QSaveFile out(tscchPath);
    if (!out.open(QIODevice::WriteOnly)) {
        if (outError) *outError = QStringLiteral("cannot write cache file: %1").arg(tscchPath);
        return -1;
    }

    QDataStream os(&out);
    os.setVersion(QDataStream::Qt_6_0);
    os << kMagic << kVersion << static_cast<quint32>(0)
       << newMeta.savedAt
       << static_cast<quint32>(keys.size());

    int written = 0;
    for (const QString &key : keys) {
        if (writeRecord(os, merged.value(key), rootFolder)) ++written;
    }

    // v3: 顶层子目录更新时间表
    QStringList subKeys = newMeta.subDirUpdatedAt.keys();
    std::sort(subKeys.begin(), subKeys.end());
    os << static_cast<quint32>(subKeys.size());
    for (const QString &k : subKeys)
        os << k << newMeta.subDirUpdatedAt.value(k);

    if (os.status() != QDataStream::Ok || !out.commit()) {
        if (outError) *outError = QStringLiteral("failed to commit cache file: %1").arg(tscchPath);
        return -1;
    }

    return written;
}

/* ========================= 部分结果日志(.part) ========================= */

void cacheio::setWorkCacheDir(const QString &preferredTempDir)
{
    const QString dir = resolveWorkCacheDir(preferredTempDir);
    QMutexLocker lock(&g_workCacheDirMutex);
    g_workCacheDir = dir;
}

QString cacheio::workCacheDir()
{
    QMutexLocker lock(&g_workCacheDirMutex);
    if (g_workCacheDir.isEmpty()) {
        /*
         * 默认位置也算一次就记住:writableCacheDir() 会真的写一个临时文件探测,
         * 而 scanJournalPath() 每 128 张就要问一次,不能每次都去探。
         */
        g_workCacheDir = resolveWorkCacheDir(QString());
    }
    return g_workCacheDir;
}

QString cacheio::partialCachePathFor(const QString &tscchPath)
{
    if (tscchPath.isEmpty()) return QString();
    const QString dir = workCacheDir();
    if (dir.isEmpty()) return QString();

    /*
     * 所有 .part 都堆在同一个 temp 目录里,靠名字区分目标 ——
     * 用目标 tscch 绝对路径的 SHA-1,同一份缓存每次算出来都一样,重启后能对上。
     */
    const QString key = QDir::cleanPath(QDir::toNativeSeparators(tscchPath)).toLower();
    const QString tag = QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex());
    return QDir(dir).filePath(QStringLiteral("part-%1.part").arg(tag));
}

bool cacheio::PartialCacheLog::open(const QString &tscchPath, const QString &rootFolder,
                                    const QString &partPath, QString *outError)
{
    close();
    m_root = QDir::cleanPath(QDir::toNativeSeparators(rootFolder));

    if (partPath.isEmpty() || m_root.isEmpty()) {
        if (outError) *outError = QStringLiteral("empty partial cache path");
        return false;
    }
    if (!QDir().mkpath(QFileInfo(partPath).absolutePath())) {
        if (outError) *outError = QStringLiteral("cannot create folder for %1").arg(partPath);
        return false;
    }

    m_file.setFileName(partPath);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (outError) *outError = QStringLiteral("cannot open %1").arg(partPath);
        return false;
    }

    QDataStream os(&m_file);
    os.setVersion(QDataStream::Qt_6_0);
    os << kPartMagic << kPartVersion << m_root
       << QDir::cleanPath(QDir::toNativeSeparators(tscchPath));
    if (os.status() != QDataStream::Ok) {
        m_file.close();
        if (outError) *outError = QStringLiteral("cannot write header: %1").arg(partPath);
        return false;
    }

    m_open = true;
    return true;
}

bool cacheio::PartialCacheLog::append(const FileEntry &fe)
{
    if (!m_open) return false;

    /* 单条记录先序列化到内存:这样"写一半就崩"只会影响这一条,不会把长度字段写脏 */
    QByteArray payload;
    {
        QDataStream ps(&payload, QIODevice::WriteOnly);
        ps.setVersion(QDataStream::Qt_6_0);
        if (!writeRecord(ps, fe, m_root))
            return false;
    }

    QDataStream os(&m_file);
    os.setVersion(QDataStream::Qt_6_0);
    os << static_cast<quint32>(payload.size());
    if (os.writeRawData(payload.constData(), payload.size()) != payload.size()) {
        m_open = false;
        return false;
    }
    return true;
}

bool cacheio::PartialCacheLog::flush()
{
    if (!m_open) return false;
    m_file.flush();
    return true;
}

void cacheio::PartialCacheLog::close()
{
    if (m_file.isOpen()) {
        m_file.flush();
        m_file.close();
    }
    m_open = false;
}

QVector<FileEntry> cacheio::readPartialCacheLog(const QString &partPath, QString *outError,
                                               const QString &expectTscch)
{
    QVector<FileEntry> result;

    QFile f(partPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (outError) *outError = QStringLiteral("cannot open %1").arg(partPath);
        return result;
    }

    QDataStream in(&f);
    in.setVersion(QDataStream::Qt_6_0);

    quint32 magic = 0;
    quint16 version = 0;
    QString root;
    QString partTarget;
    in >> magic >> version >> root;
    if (version >= kPartVersion)
        in >> partTarget;

    /*
     * .part 只是崩溃检查点:payload 里的记录结构跟 tscch 一样会随特征改版而变,
     * 所以版本不符直接当没有(重新扫描会重写它),不做兼容解析。
     */
    if (in.status() != QDataStream::Ok || magic != kPartMagic || version != kPartVersion) {
        if (outError) *outError = QStringLiteral("bad partial cache header: %1").arg(partPath);
        return result;
    }
    if (root.isEmpty() || !QDir(root).exists()) {
        if (outError) *outError = QStringLiteral("partial cache folder is gone: %1").arg(root);
        return result;
    }
    if (!expectTscch.isEmpty() && !partTarget.isEmpty()) {
        const QString a = QDir::cleanPath(QDir::toNativeSeparators(expectTscch)).toLower();
        if (a != partTarget.toLower()) {
            if (outError)
                *outError = QStringLiteral("partial cache belongs to another target: %1")
                                .arg(partTarget);
            return result;
        }
    }

    int truncated = 0;
    for (;;) {
        quint32 len = 0;
        in >> len;
        if (in.status() != QDataStream::Ok)
            break;                       // 正常结尾,或者最后一条只写了一半
        if (len == 0 || len > kMaxPartPayload) {
            ++truncated;
            break;                       // 长度字段坏了,后面的无法再对齐
        }

        QByteArray payload(static_cast<int>(len), '\0');
        if (in.readRawData(payload.data(), static_cast<int>(len)) != static_cast<int>(len)) {
            ++truncated;                 // 崩溃时正在写的那一条
            break;
        }

        QDataStream ps(payload);
        ps.setVersion(QDataStream::Qt_6_0);
        FileEntry fe;
        if (readRecord(ps, root, fe, version))
            result.push_back(std::move(fe));
        else
            ++truncated;                 // 单条坏了:跳过,继续读后面的
    }

    if (truncated > 0)
        qWarning() << "[partial cache] dropped" << truncated << "incomplete record(s) in" << partPath;

    return result;
}

int cacheio::mergePartialCacheLog(const QString &tscchPath, QString *outError)
{
    const QString part = partialCachePathFor(tscchPath);
    if (part.isEmpty() || !QFileInfo::exists(part))
        return 0;

    const QVector<FileEntry> entries = readPartialCacheLog(part, outError, tscchPath);
    int written = 0;
    if (!entries.isEmpty()) {
        /*
         * 只能"只合并":这不是一次完整扫描,不能剔除记录、也不能把 savedAt 更新成现在
         * (那等于宣称"这个目录刚刚完整扫过",下次就会跳过真正没算过的图)。
         */
        WriteOptions wo;
        wo.mergeOnly = true;
        written = writeFileEntryCache(tscchPath, entries, wo, outError);
        if (written < 0)
            return -1;           // 合并失败:留着 .part,下次再试
    }

    QFile::remove(part);
    return written;
}

/* ========================= 未完成扫描标记(JSON) ========================= */

QString cacheio::partialCacheTarget(const QString &partPath)
{
    QFile f(partPath);
    if (!f.open(QIODevice::ReadOnly))
        return QString();

    QDataStream in(&f);
    in.setVersion(QDataStream::Qt_6_0);

    quint32 magic = 0;
    quint16 version = 0;
    QString root;
    QString target;
    in >> magic >> version >> root;
    if (version >= kPartVersion)
        in >> target;

    if (in.status() != QDataStream::Ok || magic != kPartMagic)
        return QString();
    return target;
}

int cacheio::sweepOrphanPartialCaches(const QStringList &referenced)
{
    const QString dir = workCacheDir();
    if (dir.isEmpty())
        return 0;

    QSet<QString> keep;
    for (const QString &p : referenced)
        keep.insert(QDir::cleanPath(QDir::toNativeSeparators(p)).toLower());

    int handled = 0;
    const QFileInfoList files = QDir(dir).entryInfoList(
        QStringList{QStringLiteral("part-*.part")}, QDir::Files);
    for (const QFileInfo &fi : files) {
        const QString path = QDir::cleanPath(QDir::toNativeSeparators(fi.absoluteFilePath()));
        if (keep.contains(path.toLower()))
            continue;                       // 有标记指向,交给用户决定

        const QString target = partialCacheTarget(path);
        if (target.isEmpty() || !QFileInfo::exists(QFileInfo(target).absolutePath())) {
            QFile::remove(path);            // 读不出来 / 目标目录已经没了
            ++handled;
            continue;
        }

        QString err;
        if (mergePartialCacheLog(target, &err) >= 0)
            ++handled;
        else
            qWarning() << "[partial cache] sweep merge failed:" << target << err;
    }
    return handled;
}

QString cacheio::scanJournalPath()
{
    const QString dir = workCacheDir();
    if (dir.isEmpty())
        return QString();
    return QDir(dir).filePath(QStringLiteral("scan_in_progress.json"));
}

ScanJournal cacheio::readScanJournal()
{
    ScanJournal st;
    const QString path = scanJournalPath();
    if (path.isEmpty() || !QFileInfo::exists(path))
        return st;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return st;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return st;

    const QJsonObject o = doc.object();
    st.exists    = true;
    st.folder    = o.value(QStringLiteral("folder")).toString();
    st.target    = o.value(QStringLiteral("target")).toString();
    st.mode      = o.value(QStringLiteral("mode")).toString();
    st.startedAt = static_cast<qint64>(o.value(QStringLiteral("startedAt")).toDouble());
    st.updatedAt = static_cast<qint64>(o.value(QStringLiteral("updatedAt")).toDouble());
    st.done      = o.value(QStringLiteral("done")).toInt();
    st.total     = o.value(QStringLiteral("total")).toInt();
    for (const QJsonValue &v : o.value(QStringLiteral("parts")).toArray())
        st.parts << v.toString();
    return st;
}

bool cacheio::writeScanJournal(const QString &folder, const QString &target,
                               const QString &mode, int done, int total,
                               const QStringList &parts)
{
    const QString path = scanJournalPath();
    if (path.isEmpty() || folder.isEmpty())
        return false;

    /*
     * 已有文件里的 startedAt 要保留:一次扫描会写几十次,不能每次都重置开始时间
     * (提示框里"从什么时候开始算的"就没意义了)。
     */
    qint64 startedAt = 0;
    {
        const ScanJournal old = readScanJournal();
        if (old.exists && old.folder.compare(folder, Qt::CaseInsensitive) == 0)
            startedAt = old.startedAt;
    }
    if (startedAt <= 0)
        startedAt = QDateTime::currentMSecsSinceEpoch();

    QJsonObject o;
    o.insert(QStringLiteral("folder"), folder);
    o.insert(QStringLiteral("target"), target);
    o.insert(QStringLiteral("mode"), mode);
    o.insert(QStringLiteral("startedAt"), static_cast<double>(startedAt));
    o.insert(QStringLiteral("updatedAt"),
             static_cast<double>(QDateTime::currentMSecsSinceEpoch()));
    o.insert(QStringLiteral("done"), done);
    o.insert(QStringLiteral("total"), total);

    QJsonArray arr;
    for (const QString &p : parts)
        arr.append(p);
    o.insert(QStringLiteral("parts"), arr);

    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly))
        return false;
    out.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    return out.commit();
}

void cacheio::clearScanJournal()
{
    const QString path = scanJournalPath();
    if (!path.isEmpty())
        QFile::remove(path);
}
