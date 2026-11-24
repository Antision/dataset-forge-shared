#include "deduputils.h"
#include "dedupworker.h"

#include <QFile>
#include "cacheio.h"

#include <QDirIterator>
#include <QDateTime>
#include <QtConcurrent/QtConcurrentMap>

using namespace dedup;


DedupWorker::DedupWorker(std::atomic<int>*currentEpoch,QObject *parent)
    : currentEpoch(currentEpoch),QObject(parent)
{
}

DedupWorker::~DedupWorker()
{
    cancel();
    if (m_thread.joinable())
        m_thread.join();
}



void DedupWorker::cancel()
{
    m_cancel = true;
    if (m_thread.joinable())
        m_thread.join();
}

/*
 * 递归收集图片。
 * regexFilter 匹配的是【文件名去掉后缀之后的部分】(completeBaseName),
 * 采用"包含即命中"的部分匹配、忽略大小写;为空表示不过滤。
 *
 * outCacheDirs:可选。返回递归范围内"已经存在 image_features.tscch"的目录
 * (只统计含匹配图片的目录),供就近复用缓存。
 */
QStringList collectImageFiles(const QString &folder,
                              const QString &regexFilter,
                              QStringList *outCacheDirs,
                              const std::function<bool(const QString&)> &exclude,
                              bool recursive)
{
    QStringList out;
    if (outCacheDirs) outCacheDirs->clear();

    if (folder.isEmpty() || !QDir(folder).exists())
        return out;

    QRegularExpression rx;
    bool hasRegex = false;
    const QString rxPattern = regexFilter.trimmed();
    if (!rxPattern.isEmpty()) {
        rx = QRegularExpression(rxPattern);
        rx.setPatternOptions(QRegularExpression::CaseInsensitiveOption);
        hasRegex = rx.isValid();
    }

    QStringList exts = supportedExtensions();
    QStringList patterns;
    for (const QString &e : exts) {
        patterns << "*." + e;
        patterns << "*." + e.toUpper();
    }

    QSet<QString> seenCacheDirs;
    const QString rootClean = QDir::cleanPath(QDir::toNativeSeparators(folder));
    /*
     * 同一路径的 "/" 形式(上面是原生分隔符形式,给 tscch 缓存用)。
     * duplicate 段判断要在 "/" 形式下做,否则 Windows 上按 '/' split 永远分不开。
     */
    const QString rootSlash =
        QDir::cleanPath(QDir::fromNativeSeparators(folder));

    QDirIterator it(folder, patterns, QDir::Files,
                    recursive ? QDirIterator::Subdirectories
                              : QDirIterator::NoIteratorFlags);
    while (it.hasNext()) {
        const QString path = it.next();
        const QFileInfo fi(path);
        if (!fi.exists()) continue;

        // 正则筛选:匹配"去掉最后一个后缀"的文件名
        if (hasRegex && !rx.match(fi.completeBaseName()).hasMatch())
            continue;

        // 调用方给的排除谓词(导入整理:二进制阶段已经安排好的文件不再进候选)
        const QString absPath = fi.absoluteFilePath();
        if (exclude && exclude(absPath))
            continue;

        /*
         * 解压冲突的暂存区(<解压根>/.temp/)永远不算作品内容。
         *
         * 正常流程里它会在解压结束前被搬空删掉;只有"上次被强制结束"才会留下。
         * 那些文件还没决定是覆盖、跳过还是改名,让它们进扫描/去重只会制造
         * 一堆假重复,而且和真正落位的文件同名同内容。
         */
        if (QDir::cleanPath(QDir::toNativeSeparators(absPath))
                .contains(QStringLiteral("/.temp/"), Qt::CaseInsensitive))
            continue;

        /*
         * duplicate/ 是去重模块的隔离区,不是作品内容 —— 普通去重把"被移除"的
         * 文件搬进 <原目录>/duplicate/,这些文件在磁盘上还在,但语义上已经
         * 处理掉了(FileEntry::inDuplicate)。
         *
         * 如果不排除:下一轮扫描会把它们当成 inDuplicate=false 的活文件重新
         * 建对;再删一次就搬进 duplicate/duplicate/,逐轮嵌套,而且"字段说在
         * 原处、实际在隔离区"直接矛盾。
         * importdedupe 的 isSkippedDirName 和 dedupeResourceFiles 早就跳过
         * 这个目录,这里补齐同一语义。
         *
         * 只比【根目录以下】的目录段:根目录自己叫 duplicate 时不能把
         * 整个数据集排空。最后一段是文件名,不参与判断。
         */
        {
            const QString fileSlash =
                QDir::cleanPath(QDir::fromNativeSeparators(absPath));
            QString relDir = fileSlash;
            /*
             * 按目录边界比较,不能用裸 startsWith:
             * 根是 E:/data 时,E:/data2/x.png 会被误判成它的子路径。
             */
            if (relDir.compare(rootSlash, Qt::CaseInsensitive) == 0) {
                relDir.clear();
            } else if (relDir.startsWith(rootSlash + QLatin1Char('/'),
                                         Qt::CaseInsensitive)) {
                relDir = relDir.mid(rootSlash.length());
            }

            const QStringList segs = relDir.split(QLatin1Char('/'), Qt::SkipEmptyParts);
            bool insideDuplicateDir = false;
            for (int si = 0; si + 1 < segs.size(); ++si) {
                if (segs.at(si).compare(QStringLiteral("duplicate"),
                                        Qt::CaseInsensitive) == 0) {
                    insideDuplicateDir = true;
                    break;
                }
            }
            if (insideDuplicateDir)
                continue;
        }

        // 真实内容不是图片的(例如伪装成.jpg的文本)由computeFileFeatures判定并跳过
        out << fi.absoluteFilePath();

        /*
         * 顺便记录哪些目录里有 tscch。
         * 关键是【从文件所在目录一路往上查到 folder(含)】都要查:
         * 比如 folder/sub/2024/a.jpg,缓存可能在 folder/sub 这一级
         * (folder/sub/image_features.tscch 覆盖 folder/sub/**),
         * 只查 a.jpg 自己所在目录就会漏掉它。
         */
        if (outCacheDirs) {
            QString dir = QDir::cleanPath(QDir::toNativeSeparators(fi.absolutePath()));
            while (!dir.isEmpty()) {
                if (!seenCacheDirs.contains(dir)) {
                    seenCacheDirs.insert(dir);
                    if (QFileInfo::exists(cacheio::cacheFilePathForFolder(dir)))
                        outCacheDirs->append(dir);
                }
                if (dir.compare(rootClean, Qt::CaseInsensitive) == 0) break;   // 到 folder 为止
                const QString parent = QDir::cleanPath(QDir::toNativeSeparators(
                    QFileInfo(dir).absolutePath()));
                // 不要越过 folder 继续往上(上一层由 scanFolders 单独处理)
                if (parent.isEmpty() || parent == dir
                    || !dedup::isUnderOrEqual(parent, rootClean))
                    break;
                dir = parent;
            }
        }
    }

    out.sort(Qt::CaseInsensitive);
    return out;
}

bool computeFileFeatures(FileEntry &entry, const QString &path,
                         const ScanOptions &options)
{
    entry = FileEntry();

    const QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile())
        return false;

    QFile file(fi.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray data = file.readAll();
    file.close();
    if (data.isEmpty())
        return false;

    // ===== 路径字段 =====
    entry.originalPath = fi.absoluteFilePath();
    entry.currentPath  = entry.originalPath;
    entry.inDuplicate  = false;
    entry.folder       = fi.absolutePath();
    entry.filename     = fi.fileName();
    entry.filesize     = fi.size();
    entry.modTime      = fi.lastModified().toMSecsSinceEpoch();

    // 格式以文件头为准(比扩展名可靠),识别不出来时退回扩展名
    const QString detected = detectImageFormat(data);
    entry.format = detected.isEmpty() ? fi.suffix().toLower() : detected;

    // ===== SHA-256:只有要写cache/显式要求时才算(不读不写tscch时算它没有意义) =====
    if (options.needSHA256())
        computeSHA256(data, entry.SHA256);

    // ===== 质量信息:jpg按量化表估计;png/无损webp=101;有损webp按VP8量化参数估计 =====
    entry.quality = estimateImageQuality(data, entry.format);

    /*
     * 这里只按 DQT 估质量,不解全图重算 DCT。
     * 以前的 det/mosq 要整解码 + 双边滤波(1000x1200 约 65ms、4000x3000 约 300ms),
     * 放进扫描会让"打开一个扫过的目录"凭空多出几十秒,已整体摘除。
     */

    // ===== OpenCV 特征 =====
    cv::Mat img = readImageBGR(data);
    if (!img.empty()) {
        entry.size = QSize(img.cols, img.rows);
        try { entry.pHash = computePHash(img); } catch (...) { entry.pHash = 0; }
        try { entry.dHash = computeDHash(img); } catch (...) { entry.dHash = 0; }
        try {
            cv::Mat gray;
            if (img.channels() == 3) cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);
            else gray = img;
            if (gray.depth() != CV_8U) gray.convertTo(gray, CV_8U);

            /*
             * 缩到长边 1024 再提特征(参数与原因见 deduputils.h 的 kOrbLongEdge)。
             * 特征点坐标是【缩放后画面的坐标】,两张图都缩到同一长边之后,
             * 它们之间的旋转/裁切/缩放关系才是一个干净的相似变换。
             */
            cv::Mat work = gray;
            const int longEdge = std::max(gray.cols, gray.rows);
            if (longEdge > dedup::kOrbLongEdge) {
                const double scale = double(dedup::kOrbLongEdge) / double(longEdge);
                cv::resize(gray, work, cv::Size(), scale, scale, cv::INTER_AREA);
            }

            cv::Ptr<cv::ORB> orb = cv::ORB::create(dedup::kOrbFeatureCount);
            std::vector<cv::KeyPoint> kps;
            cv::Mat descriptors;
            orb->detectAndCompute(work, cv::noArray(), kps, descriptors);

            entry.orbDescriptors = descriptors;
            entry.orbKeypoints = static_cast<int>(kps.size());

            /*
             * 关键点坐标必须和描述子行一一对应,否则几何校验会把 A 的点配到
             * B 的另一个点上,算出完全错误的变换。detectAndCompute 保证同序,
             * 但这里仍然显式校验行数,不一致就宁可不存。
             */
            if (!descriptors.empty() && static_cast<int>(kps.size()) == descriptors.rows) {
                entry.orbPoints.create(descriptors.rows, 2, CV_32F);
                for (int r = 0; r < descriptors.rows; ++r) {
                    entry.orbPoints.at<float>(r, 0) = kps[static_cast<size_t>(r)].pt.x;
                    entry.orbPoints.at<float>(r, 1) = kps[static_cast<size_t>(r)].pt.y;
                }
            } else {
                entry.orbPoints = cv::Mat();
            }
        } catch (...) {
            entry.orbDescriptors = cv::Mat();
            entry.orbPoints = cv::Mat();
            entry.orbKeypoints = 0;
        }
        // 缩略图只在明确要求时生成(列表是懒加载的)
        if (options.thumbnail) {
            try {
                entry.thumbnail = makeThumbnail(img);
            } catch (...) { entry.thumbnail = QImage(); }
        }
    } else {
        entry.size = QSize(0, 0);
        entry.pHash = 0;
        entry.dHash = 0;
        entry.orbDescriptors = cv::Mat();
        entry.orbPoints = cv::Mat();
        entry.orbKeypoints = 0;
        entry.thumbnail = QImage();
    }

    return true;
}

/*
 * 注意:-r"..." / -s"..." 这种写在路径框里的旧用法已经彻底去掉,
 * 筛选统一由每个目录自己的 le_regex_x 正则负责(见 collectImageFiles)。
 */





QVector<FileEntry> DedupWorker::updateFileEntryCache(const QString &path,
                                                    bool doThumbnail,
                                                    bool doSHA256,
                                                    const std::function<void(int,int)> &progress,
                                                    const std::function<bool()> &cancelled,
                                                    QString *outError,
                                                    CacheUpdateStats *outStats)
{
    QVector<FileEntry> result;

    if (path.isEmpty()) {
        if (outError) *outError = QStringLiteral("empty path");
        return result;
    }

    const QString folder = QDir::cleanPath(QDir::toNativeSeparators(path.trimmed()));
    if (folder.isEmpty() || !QDir(folder).exists()) {
        if (outError) *outError = QStringLiteral("folder not found: %1").arg(path);
        return result;
    }

    ScanOptions options;
    options.cache      = true;      // 复用旧记录 + 更新后写回
    options.sha256     = doSHA256;
    options.thumbnail  = doThumbnail;
    options.reuseCache = true;
    /* 中途取消/崩溃时,下次启动的提示文案据此区分来源 */
    options.journalMode = QStringLiteral("cache");
    /*
     * 不去碰上一层目录的缓存:用户按的是这个目录的按钮,没有理由顺手改它父目录的 tscch。
     * 子目录的 tscch 照常读写,而且是【就近写回】—— 每张图的记录只落在离它最近的
     * 那份缓存里,不再像以前那样全部并到 F(见 scanFolders 的缓存策略)。
     */
    options.useNestedCaches = false;

    // scanFolders内部:复用未变的、重算变化的、加入新增的、写回时剔除丢失的
    result = scanFolders(QStringList{folder}, options, progress, cancelled, outStats);

    if (cancelled && cancelled()) {
        /*
         * 取消不是成功:必须把话说清楚,否则调用方会显示"缓存已更新,共 0 条记录"。
         * 但"已算好的特征"并没有白算 —— scanFolders 在取消时会把它们
         * mergeOnly 写进目标目录的 image_features.tscch(见写回那段)。
         */
        result.clear();
        if (outError)
            *outError = QStringLiteral("已取消(已算好的图片特征已写入 image_features.tscch)");
    }

    return result;
}

int DedupWorker::cacheImageFeatures(const QString &path,
                                    bool doThumbnail,
                                    bool doSHA256,
                                    const std::function<void(int,int)> &progress,
                                    const std::function<bool()> &cancelled,
                                    QString *outError,
                                    CacheUpdateStats *outStats)
{
    const QVector<FileEntry> merged = updateFileEntryCache(
        path, doThumbnail, doSHA256, progress, cancelled, outError, outStats);

    if (outError && !outError->isEmpty())
        return -1;

    return merged.size();
}
